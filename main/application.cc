#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "boards/common/music.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <algorithm>
#include <string>
#include <vector>
#include <esp_netif.h>
#include <nvs_flash.h>

// Include otto-robot specific headers if building for otto-robot board
#ifdef CONFIG_BOARD_TYPE_OTTO_ROBOT
#include "boards/otto-robot/otto_webserver.h"
#include "boards/otto-robot/otto_emoji_display.h"
#endif

// Include kiki-robot specific headers if building for kiki board
#ifdef CONFIG_BOARD_TYPE_KIKI
#include "boards/kiki/otto_webserver.h"
#include "boards/kiki/otto_emoji_display.h"
#endif

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        // Use std::string to prevent buffer overflow with long URLs
        std::string message = std::string(Lang::Strings::FOUND_NEW_ASSETS) + download_url;
        Alert(Lang::Strings::LOADING_ASSETS, message.c_str(), "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            // Use Schedule instead of detached thread to prevent dangling pointer
            Schedule([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            });
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    // Increased stack size to 12KB to prevent overflow with complex event processing
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 6, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        // Accept audio packets in speaking state OR when transitioning to speaking
        // This prevents audio loss when state transition is slightly delayed
        if (device_state_ == kDeviceStateSpeaking || device_state_ == kDeviceStateListening) {
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
                ESP_LOGW(TAG, "Audio decode queue full, packet dropped");
            }
        } else {
            ESP_LOGD(TAG, "Ignoring audio packet in state: %d", device_state_);
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        // Log raw JSON for debugging
        char* json_string = cJSON_Print(root);
        ESP_LOGI(TAG, "=========================================");
        ESP_LOGI(TAG, "Received JSON from server:");
        ESP_LOGI(TAG, "  JSON length: %d bytes", json_string ? (int)strlen(json_string) : 0);
        if (json_string) {
            ESP_LOGI(TAG, "  JSON (first 500): %.500s", json_string);
            free(json_string);
        }
        
        auto type = cJSON_GetObjectItem(root, "type");
        if (!type || !cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Invalid or missing 'type' field in JSON");
            ESP_LOGI(TAG, "=========================================");
            return;
        }
        ESP_LOGI(TAG, "  Message type: %s", type->valuestring);
        ESP_LOGI(TAG, "=========================================");
        
        if (strcmp(type->valuestring, "tts") == 0) {
            // 🎵 LOGIC QUAN TRỌNG: Block TTS khi nhạc đang tải
            // 
            // Khi user nói "phát bài X", flow là:
            //   1. Server gửi MCP play_music → client bắt đầu tải nhạc
            //   2. Server có thể gửi thêm TTS hỏi "bạn muốn phát X đúng không?"
            //   3. Server có thể gửi LLM emotion
            //
            // NHƯNG khi nhạc ĐÃ BẮT ĐẦU TẢI (IsPreparing/IsDownloading):
            //   - User ĐÃ CHỌN BÀI THÀNH CÔNG → không cần AI hỏi thêm
            //   - Device NÊN IM LẶNG → về IDLE để phát nhạc
            //   - TTS/LLM THỪA THÃI → vì user đã quyết định rồi
            //   - SSL CẦN SRAM → TTS sẽ chiếm SRAM gây fail SSL
            //
            // 🎵 Block TTS khi nhạc đang hoạt động (preparing/playing/downloading)
            // Lý do: User đã chọn bài → không cần AI hỏi thêm
            // Học từ Maggotxy/TienHuyIoT: Block TTS trong toàn bộ quá trình phát nhạc
            auto music = Board::GetInstance().GetMusic();
            if (music && (music->IsPreparing() || music->IsPlaying() || music->IsDownloading())) {
                ESP_LOGI(TAG, "🎵 Music active (prep=%d, play=%d, dl=%d) - blocking TTS",
                         music->IsPreparing(), music->IsPlaying(), music->IsDownloading());
                return;
            }
            
            auto state = cJSON_GetObjectItem(root, "state");
            if (!state || !cJSON_IsString(state)) {
                ESP_LOGW(TAG, "TTS message missing 'state' field");
                return;
            }
            ESP_LOGI(TAG, "TTS state: %s", state->valuestring);
            
            if (strcmp(state->valuestring, "start") == 0) {
                // 🎵 Block TTS start khi nhạc đang hoạt động
                auto music_check = Board::GetInstance().GetMusic();
                if (music_check && (music_check->IsPreparing() || music_check->IsPlaying() || music_check->IsDownloading())) {
                    ESP_LOGI(TAG, "🎵 Music active - blocking TTS start");
                    return;
                }
                
                Schedule([this]() {
                    aborted_ = false;
                    ESP_LOGI(TAG, "TTS start received, current state: %d, setting to speaking", device_state_);
                    // Force state to speaking immediately to ensure audio packets are received
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this, display]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        // Reset force emoji flags when TTS ends
                        // Use compare_exchange to prevent race condition
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                        bool expected = true;
                        if (force_silly_emoji_.compare_exchange_strong(expected, false)) {
                            ESP_LOGI(TAG, "🎉 TTS ended, resetting force_silly_emoji flag");
                        }
                        expected = true;
                        if (force_shocked_emoji_.compare_exchange_strong(expected, false)) {
                            ESP_LOGI(TAG, "💀 TTS ended, resetting force_shocked_emoji flag");
                        }
                        expected = true;
                        if (force_delicious_emoji_.compare_exchange_strong(expected, false)) {
                            ESP_LOGI(TAG, "🍕 TTS ended, resetting force_delicious_emoji flag");
                        }
                        
                        // Restore chat message and emoji overlay mode when TTS ends
                        auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                        if (otto_display && otto_display->IsEmojiOverlayMode()) {
                            otto_display->SetChatMessageHidden(false);
                            otto_display->SetEmojiOverlayMode(false);
                            otto_display->SetEmotion("neutral");
                            ESP_LOGI(TAG, "✅ Restored chat message and emoji overlay after TTS ended");
                        }
#endif
                        // Clear IP display flag when TTS ends
                        if (showing_ip_address_.load()) {
                            ESP_LOGI(TAG, "🌐 TTS ended, clearing IP address display");
                            showing_ip_address_.store(false);
                        }
                        
                        // Keep chat message visible after TTS ends (user requested)
                        // if (display) {
                        //     display->SetChatMessage("assistant", "");
                        // }
                        
                        // Don't go to Listening if music is playing or downloading - stay in IDLE
                        auto music = Board::GetInstance().GetMusic();
                        if (music && (music->IsPlaying() || music->IsDownloading() || music->IsPreparing())) {
                            ESP_LOGI(TAG, "🎵 Music active (play=%d, dl=%d, prep=%d), staying in IDLE state (not going to Listening)",
                                     music->IsPlaying(), music->IsDownloading(), music->IsPreparing());
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            // Always go to Listening after TTS ends (user requested)
                            // Reset listening mode to auto-stop for continuous conversation
                            listening_mode_ = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                // 🎵 Block TTS sentence_start khi nhạc đang hoạt động
                auto music_check = Board::GetInstance().GetMusic();
                if (music_check && (music_check->IsPreparing() || music_check->IsPlaying() || music_check->IsDownloading())) {
                    ESP_LOGI(TAG, "🎵 Music active - blocking TTS sentence_start");
                    return;
                }
                
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::string tts_text = text->valuestring;
                    ESP_LOGI(TAG, "TTS sentence_start: %s", tts_text.c_str());
                    // Only display TTS text if it's meaningful and different from what LLM already displayed
                    // LLM message usually contains the full response, TTS is just for audio playback
                    // So we display TTS text as a fallback if LLM didn't provide text
                    if (tts_text.length() > 0) {
                        Schedule([this, display, message = std::string(tts_text)]() {
                            display->SetChatMessage("assistant", message.c_str());
                            ESP_LOGD(TAG, "Displayed TTS sentence text: %s", message.c_str());
                        });
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            ESP_LOGI(TAG, "Processing STT message from server");
            auto text = cJSON_GetObjectItem(root, "text");
            if (!text || !cJSON_IsString(text)) {
                ESP_LOGW(TAG, "Invalid or missing 'text' field in STT message");
                return;
            }
            std::string message = text->valuestring;
            ESP_LOGI(TAG, "STT message text: '%s' (length: %d)", message.c_str(), (int)message.length());
            
            // Skip placeholder responses used when we trigger virtual wake word from web input
            // Server may echo back the wake word as STT message, so we filter it out
            // "Ly Ly" is the default wake word that server uses when receiving empty wake word
            // "text" is the default wake word we use for web text input
            if (message == "text_input" || message == "web_input" || message == "text input" || 
                message.empty() || message == "Ly Ly" || message == "ly ly" || 
                message == "text" || message == "Text") {
                ESP_LOGI(TAG, "Ignoring placeholder STT message from server: '%s'", message.c_str());
                return;
            }
            
            ESP_LOGI(TAG, ">> %s", message.c_str());
            
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
            // Check for "đứng lên" or "đứng dậy" commands to go to home position
            // This is processed BEFORE sending to AI server to ensure correct action
            std::string lower_message = message;
            std::transform(lower_message.begin(), lower_message.end(), lower_message.begin(), ::tolower);
            
            // Check for stand up commands (various Vietnamese and English forms)
            bool is_stand_up_command = 
                lower_message.find("đứng lên") != std::string::npos || 
                lower_message.find("đứng dậy") != std::string::npos ||
                lower_message.find("dung len") != std::string::npos ||
                lower_message.find("dung day") != std::string::npos ||
                (lower_message.find("stand") != std::string::npos && 
                 (lower_message.find("up") != std::string::npos || lower_message.find("straight") != std::string::npos)) ||
                lower_message.find("home position") != std::string::npos ||
                lower_message == "home";
            
            if (is_stand_up_command) {
                ESP_LOGI(TAG, "🧍 Detected 'stand up' command: '%s', standing up from sitting/lying position", message.c_str());
                otto_controller_queue_action(ACTION_DOG_STAND_UP, 1, 500, 0, 0);
                ESP_LOGI(TAG, "✅ ACTION_DOG_STAND_UP queued successfully");
            }
            
            // Check for QR code commands (hiện mã QR, mã quy rờ, mã ngân hàng)
            // Force "winking" emoji for 30 seconds and block other emojis
            bool is_qr_code_command = 
                lower_message.find("hiện mã qr") != std::string::npos ||
                lower_message.find("hien ma qr") != std::string::npos ||
                lower_message.find("mã qr") != std::string::npos ||
                lower_message.find("ma qr") != std::string::npos ||
                lower_message.find("mã quy rờ") != std::string::npos ||
                lower_message.find("ma quy ro") != std::string::npos ||
                lower_message.find("quy rờ") != std::string::npos ||
                lower_message.find("quy ro") != std::string::npos ||
                lower_message.find("mã ngân hàng") != std::string::npos ||
                lower_message.find("ma ngan hang") != std::string::npos ||
                lower_message.find("ngân hàng") != std::string::npos ||
                lower_message.find("ngan hang") != std::string::npos ||
                lower_message.find("qr code") != std::string::npos ||
                lower_message.find("show qr") != std::string::npos ||
                lower_message.find("bank code") != std::string::npos;
            
            // Check for goodbye commands (tạm biệt, bye bye)
            // Trigger lie down pose
            bool is_goodbye_command = 
                lower_message.find("tạm biệt") != std::string::npos ||
                lower_message.find("tam biet") != std::string::npos ||
                lower_message.find("tạm biệt nhé") != std::string::npos ||
                lower_message.find("tam biet nhe") != std::string::npos ||
                lower_message.find("bye bye") != std::string::npos ||
                lower_message.find("goodbye") != std::string::npos ||
                lower_message.find("see you") != std::string::npos;
            
            if (is_goodbye_command) {
                ESP_LOGI(TAG, "👋 Detected goodbye command: '%s', robot will lie down", message.c_str());
                otto_controller_queue_action(ACTION_DOG_LIE_DOWN, 1, 2000, 0, 0);  // Lie down slowly
                ESP_LOGI(TAG, "✅ ACTION_DOG_LIE_DOWN queued for goodbye");
            }
            
            // Check for control panel commands (mở bảng điều khiển, mở trang điều khiển, web control)
            // Display IP with control panel URL
            bool is_control_panel_command = 
                lower_message.find("mở bảng điều khiển") != std::string::npos ||
                lower_message.find("mo bang dieu khien") != std::string::npos ||
                lower_message.find("bảng điều khiển") != std::string::npos ||
                lower_message.find("bang dieu khien") != std::string::npos ||
                lower_message.find("mở trang điều khiển") != std::string::npos ||
                lower_message.find("mo trang dieu khien") != std::string::npos ||
                lower_message.find("mở lại trang điều khiển") != std::string::npos ||
                lower_message.find("mo lai trang dieu khien") != std::string::npos ||
                lower_message.find("trang điều khiển") != std::string::npos ||
                lower_message.find("trang dieu khien") != std::string::npos ||
                lower_message.find("web control") != std::string::npos ||
                lower_message.find("control panel") != std::string::npos ||
                lower_message.find("mở web") != std::string::npos ||
                lower_message.find("mo web") != std::string::npos;
            
            ESP_LOGI(TAG, "🔍 Control panel detection: %s (message: '%s')", 
                     is_control_panel_command ? "MATCHED" : "not matched", 
                     lower_message.c_str());
            
            if (is_control_panel_command) {
                ESP_LOGI(TAG, "📱 Detected control panel command: '%s', starting webserver and showing IP", message.c_str());
                
                // Don't send this command to AI - just show IP immediately
                // Get display and show IP right away (not in Schedule to avoid delay)
                auto display = Board::GetInstance().GetDisplay();
                
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                // Start webserver in background
                Schedule([this, display]() {
                    try {
                        // Start webserver
                        esp_err_t webserver_result = otto_start_webserver();
                        if (webserver_result == ESP_OK) {
                            ESP_LOGI(TAG, "✅ Webserver started successfully (will auto-stop after 5 min)");
                        } else {
                            ESP_LOGW(TAG, "⚠️ Webserver already running or failed to start: %s", esp_err_to_name(webserver_result));
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGE(TAG, "❌ Exception starting webserver: %s", e.what());
                    }
                });
#endif
                
                // Get and display IP immediately (outside Schedule for faster response)
                if (!display) {
                    ESP_LOGE(TAG, "Display is null, cannot show control panel IP");
                    return;
                }
                
                esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif == nullptr) {
                    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                }
                
                std::string control_url = "http://192.168.4.1";
                if (netif) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        char ip_str[16];
                        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                        control_url = "http://";
                        control_url += ip_str;
                        ESP_LOGI(TAG, "📍 Device IP: %s", ip_str);
                    }
                }
                
                // Display QR code instead of IP text (15 seconds)
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    // Show IP as notification first, then QR code on top
                    display->ShowNotification("🌐 " + control_url, 15000);
                    otto_display->ShowQRCode(control_url.c_str(), 15000);  // 15 seconds
                    ESP_LOGI(TAG, "✅ QR CODE + IP DISPLAYED: %s (15s)", control_url.c_str());
                } else {
                    display->ShowNotification("🌐 " + control_url, 15000);
                }
#else
                if (display) {
                    display->ShowNotification("🌐 " + control_url, 15000);
                    ESP_LOGI(TAG, "✅ IP DISPLAYED: %s (15s notification)", control_url.c_str());
                }
#endif
                
                // Play notification sound
                Schedule([this]() {
                    PlaySound("ding");
                });
                
                // Don't send to AI server - return early
                return;
            }
            
            if (is_qr_code_command) {
                ESP_LOGI(TAG, "🤑 Detected QR code command: '%s', showing winking emoji until TTS ends", message.c_str());
                force_winking_emoji_.store(true);
                
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                auto display = Board::GetInstance().GetDisplay();
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    otto_display->SetChatMessageHidden(true);
                    otto_display->SetEmojiOverlayMode(true);
                    otto_display->SetEmotion("winking");
                    ESP_LOGI(TAG, "😉 Set winking emoji with overlay mode, chat hidden until TTS ends");
                }
#else
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->SetEmotion("winking");
                    ESP_LOGI(TAG, "😉 Set 'winking' emoji for QR code display");
                }
#endif
            }
            
            // Check for celebration commands (sinh nhật, năm mới, noel, ngày cưới)
            // Force "silly" emoji and block other emojis until TTS ends
            bool is_celebration_command = 
                lower_message.find("chúc mừng sinh nhật") != std::string::npos ||
                lower_message.find("chuc mung sinh nhat") != std::string::npos ||
                lower_message.find("sinh nhật") != std::string::npos ||
                lower_message.find("sinh nhat") != std::string::npos ||
                lower_message.find("happy birthday") != std::string::npos ||
                lower_message.find("chúc mừng năm mới") != std::string::npos ||
                lower_message.find("chuc mung nam moi") != std::string::npos ||
                lower_message.find("năm mới") != std::string::npos ||
                lower_message.find("nam moi") != std::string::npos ||
                lower_message.find("happy new year") != std::string::npos ||
                lower_message.find("chúc mừng noel") != std::string::npos ||
                lower_message.find("chuc mung noel") != std::string::npos ||
                lower_message.find("mừng noel") != std::string::npos ||
                lower_message.find("mung noel") != std::string::npos ||
                lower_message.find("merry christmas") != std::string::npos ||
                lower_message.find("chúc mừng giáng sinh") != std::string::npos ||
                lower_message.find("chuc mung giang sinh") != std::string::npos ||
                lower_message.find("chúc mừng ngày cưới") != std::string::npos ||
                lower_message.find("chuc mung ngay cuoi") != std::string::npos ||
                lower_message.find("mừng ngày cưới") != std::string::npos ||
                lower_message.find("mung ngay cuoi") != std::string::npos ||
                lower_message.find("ngày cưới") != std::string::npos ||
                lower_message.find("ngay cuoi") != std::string::npos ||
                lower_message.find("happy wedding") != std::string::npos;
            
            if (is_celebration_command) {
                ESP_LOGI(TAG, "🎉 Detected celebration command: '%s', forcing 'silly' emoji until TTS ends", message.c_str());
                force_silly_emoji_.store(true);
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->SetEmotion("silly");
                    ESP_LOGI(TAG, "✅ Set 'silly' emoji for celebration");
                }
                // Perform celebration pose: bow + wag tail
                otto_controller_queue_action(ACTION_DOG_BOW, 1, 1500, 0, 0);  // Bow (cúi chào)
                otto_controller_queue_action(ACTION_DOG_WAG_TAIL, 5, 100, 0, 0);  // Wag tail 5 times (vẫy đuôi)
                ESP_LOGI(TAG, "🎊 Queued celebration actions: BOW + WAG_TAIL");
            }
            
            // Check for shooting/gun commands (súng nè, bang bang, bùm bùm, bắn nè)
            // Trigger play dead pose immediately
            bool is_shoot_command = 
                lower_message.find("súng nè") != std::string::npos ||
                lower_message.find("sung ne") != std::string::npos ||
                lower_message.find("bắn") != std::string::npos ||
                lower_message.find("ban ne") != std::string::npos ||
                lower_message.find("bang bang") != std::string::npos ||
                lower_message.find("bùm") != std::string::npos ||
                lower_message.find("bum") != std::string::npos ||
                lower_message.find("shoot") != std::string::npos ||
                lower_message.find("gun") != std::string::npos;
            
            if (is_shoot_command) {
                ESP_LOGI(TAG, "🔫 Detected shoot command: '%s', forcing 'shocked' emoji until TTS ends", message.c_str());
                force_shocked_emoji_.store(true);
                otto_controller_queue_action(ACTION_DOG_PLAY_DEAD, 1, 5, 0, 0);  // Play dead for 5 seconds
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->SetEmotion("shocked");
                    ESP_LOGI(TAG, "✅ Set 'shocked' emoji for shoot command (will be locked during TTS)");
                }
                // Do NOT return - allow LLM to process and respond, but emoji stays locked
            }
            
            // Check for custom delicious keyword (cached in memory for instant matching)
            // Học từ cách shoot command hoạt động: match ngay lập tức, không đọc NVS mỗi lần
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
            {
                // Load keywords from NVS on first use (lazy loading)
                if (!keywords_loaded_) {
                    ReloadCustomKeywords();
                }
                
                if (!cached_keywords_.empty()) {
                    bool keyword_found = false;
                    std::string matched_kw;
                    
                    for (const auto& kw : cached_keywords_) {
                        // Direct find on lower_message (same pattern as shoot command)
                        // Both STT message and saved keywords are UTF-8 Vietnamese
                        if (lower_message.find(kw) != std::string::npos) {
                            ESP_LOGI(TAG, "🍕 Detected keyword '%s' in message", kw.c_str());
                            keyword_found = true;
                            matched_kw = kw;
                            break;
                        }
                        // Also try matching against original message (case-sensitive fallback)
                        if (message.find(kw) != std::string::npos) {
                            ESP_LOGI(TAG, "🍕 Detected keyword '%s' in original message", kw.c_str());
                            keyword_found = true;
                            matched_kw = kw;
                            break;
                        }
                    }
                    
                    if (keyword_found) {
                        ESP_LOGI(TAG, "🍕 Keyword matched! kw='%s', emoji='%s', pose='%s', action_slot=%d", 
                                 matched_kw.c_str(), cached_emoji_.c_str(), cached_pose_.c_str(), cached_action_slot_);
                        force_delicious_emoji_.store(true);
                        
                        // Set custom emoji immediately
                        auto display = Board::GetInstance().GetDisplay();
                        if (display && !cached_emoji_.empty()) {
                            display->SetEmotion(cached_emoji_.c_str());
                            ESP_LOGI(TAG, "✅ Set '%s' emoji for custom keyword", cached_emoji_.c_str());
                        }
                        
                        // Execute pose action if configured (map pose name → ACTION_DOG_*)
                        if (!cached_pose_.empty() && cached_pose_ != "none") {
                            int pose_action = -1;
                            if (cached_pose_ == "sit") pose_action = ACTION_DOG_SIT_DOWN;
                            else if (cached_pose_ == "wave") pose_action = ACTION_DOG_WAVE_RIGHT_FOOT;
                            else if (cached_pose_ == "bow") pose_action = ACTION_DOG_BOW;
                            else if (cached_pose_ == "stretch") pose_action = ACTION_DOG_STRETCH;
                            else if (cached_pose_ == "swing") pose_action = ACTION_DOG_SWING;
                            else if (cached_pose_ == "dance") pose_action = ACTION_DOG_DANCE;
                            
                            if (pose_action >= 0) {
                                ESP_LOGI(TAG, "🐕 Executing pose '%s' (action=%d) for keyword", cached_pose_.c_str(), pose_action);
                                otto_controller_queue_action(pose_action, 1, 1500, 0, 0);
                                ESP_LOGI(TAG, "✅ Pose '%s' queued successfully", cached_pose_.c_str());
                            }
                        }
                        
                        // Also execute saved memory slot if configured (1-3)
                        if (cached_action_slot_ >= 1 && cached_action_slot_ <= 3) {
                            ESP_LOGI(TAG, "🎭 Executing action slot %d for keyword", cached_action_slot_);
                            int actions_played = otto_play_memory_slot(cached_action_slot_);
                            ESP_LOGI(TAG, "✅ Played %d actions from slot %d", actions_played, cached_action_slot_);
                        }
                    }
                }
            }
#endif
            
            // Check for emoji mode toggle commands (đổi biểu cảm, chuyển biểu cảm)
            bool is_emoji_toggle_command = 
                lower_message.find("đổi biểu cảm") != std::string::npos ||
                lower_message.find("doi emoji") != std::string::npos ||
                lower_message.find("chuyển emoji") != std::string::npos ||
                lower_message.find("chuyen emoji") != std::string::npos ||
                lower_message.find("thay đổi biểu cảm") != std::string::npos ||
                lower_message.find("thay doi emoji") != std::string::npos ||
                lower_message.find("đổi biểu tượng") != std::string::npos ||
                lower_message.find("doi bieu tuong") != std::string::npos ||
                lower_message.find("toggle emoji") != std::string::npos ||
                lower_message.find("switch emoji") != std::string::npos ||
                lower_message.find("change emoji") != std::string::npos;
            
            if (is_emoji_toggle_command) {
                ESP_LOGI(TAG, "🔄 Detected emoji toggle command: '%s'", message.c_str());
                
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    // Cast to OttoEmojiDisplay to access SetEmojiMode
                    #if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                    auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                    if (otto_display) {
                        // Check current mode and toggle
                        bool current_mode = otto_display->IsUsingOttoEmoji();
                        bool new_mode = !current_mode;
                        
                        ESP_LOGI(TAG, "🔍 Current emoji mode: %s", current_mode ? "Otto GIF" : "Twemoji Unicode");
                        
                        // Toggle the mode
                        otto_display->SetEmojiMode(new_mode);
                        
                        std::string mode_name = new_mode ? "Otto GIF" : "Twemoji Unicode";
                        ESP_LOGI(TAG, "✅ Switched emoji mode to: %s", mode_name.c_str());
                        
                        // Show notification about mode change
                        std::string notification = "🎭 Emoji: " + mode_name;
                        display->ShowNotification(notification, 3000);
                        
                        // Update emoji immediately to show the change
                        display->SetEmotion("happy");
                    } else {
                        ESP_LOGW(TAG, "⚠️ Display is not OttoEmojiDisplay, cannot toggle emoji mode");
                    }
                    #else
                    ESP_LOGW(TAG, "⚠️ Emoji toggle only available on Otto Robot/Kiki boards");
                    #endif
                } else {
                    ESP_LOGE(TAG, "❌ Display is NULL, cannot toggle emoji mode");
                }
                
                // Don't send to AI server - return early
                return;
            }
            
            // Check for clock display command ("đồng hồ", "mấy giờ", "xem giờ")
            // Note: Use original message for Vietnamese with diacritics (UTF-8 safe)
            if (message.find("đồng hồ") != std::string::npos ||
                message.find("Đồng hồ") != std::string::npos ||
                lower_message.find("dong ho") != std::string::npos ||
                message.find("mấy giờ") != std::string::npos ||
                message.find("Mấy giờ") != std::string::npos ||
                lower_message.find("may gio") != std::string::npos ||
                message.find("xem giờ") != std::string::npos ||
                lower_message.find("xem gio") != std::string::npos ||
                message.find("hiện giờ") != std::string::npos ||
                lower_message.find("hien gio") != std::string::npos ||
                message.find("giờ rồi") != std::string::npos ||
                lower_message.find("gio roi") != std::string::npos ||
                message.find("bây giờ") != std::string::npos ||
                lower_message.find("bay gio") != std::string::npos ||
                lower_message.find("what time") != std::string::npos ||
                lower_message.find("show clock") != std::string::npos ||
                message.find("xem đồng hồ") != std::string::npos) {
                ESP_LOGI(TAG, "⏰ Detected clock display command: '%s'", message.c_str());
                
                // Try to cast display to OttoEmojiDisplay and show clock
                #if CONFIG_BOARD_TYPE_KIKI
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    Schedule([otto_display]() {
                        otto_display->ShowClock(10000);  // Show clock for 10 seconds
                    });
                    ESP_LOGI(TAG, "⏰ Clock display triggered via voice command");
                } else {
                    ESP_LOGW(TAG, "❌ Display is not OttoEmojiDisplay, cannot show clock");
                }
                #endif
                
                // Still send to AI server so it can respond with the time
                // Don't return here - let the message continue to AI
            }
            
            // Check for music control commands (bài trước, bài tiếp, tạm dừng, tiếp tục, dừng nhạc)
            // Detect keywords and send music_control signal to server immediately
            std::string music_action = "";
            
            // Next track commands
            if (lower_message.find("bài tiếp") != std::string::npos ||
                lower_message.find("bai tiep") != std::string::npos ||
                lower_message.find("bài tiếp theo") != std::string::npos ||
                lower_message.find("bai tiep theo") != std::string::npos ||
                lower_message.find("bài kế") != std::string::npos ||
                lower_message.find("bai ke") != std::string::npos ||
                lower_message.find("bài sau") != std::string::npos ||
                lower_message.find("bai sau") != std::string::npos ||
                lower_message.find("next song") != std::string::npos ||
                lower_message.find("next track") != std::string::npos ||
                lower_message.find("skip") != std::string::npos) {
                music_action = "next";
            }
            // Previous track commands
            else if (lower_message.find("bài trước") != std::string::npos ||
                     lower_message.find("bai truoc") != std::string::npos ||
                     lower_message.find("bài trước đó") != std::string::npos ||
                     lower_message.find("bai truoc do") != std::string::npos ||
                     lower_message.find("quay lại bài") != std::string::npos ||
                     lower_message.find("quay lai bai") != std::string::npos ||
                     lower_message.find("previous song") != std::string::npos ||
                     lower_message.find("previous track") != std::string::npos) {
                music_action = "previous";
            }
            // Pause commands
            else if (lower_message.find("tạm dừng") != std::string::npos ||
                     lower_message.find("tam dung") != std::string::npos ||
                     lower_message.find("dừng nhạc") != std::string::npos ||
                     lower_message.find("dung nhac") != std::string::npos ||
                     lower_message.find("tắt nhạc") != std::string::npos ||
                     lower_message.find("tat nhac") != std::string::npos ||
                     lower_message.find("pause") != std::string::npos ||
                     lower_message.find("stop music") != std::string::npos) {
                music_action = "pause";
            }
            // Resume/Play commands
            else if (lower_message.find("tiếp tục") != std::string::npos ||
                     lower_message.find("tiep tuc") != std::string::npos ||
                     lower_message.find("phát tiếp") != std::string::npos ||
                     lower_message.find("phat tiep") != std::string::npos ||
                     lower_message.find("mở nhạc") != std::string::npos ||
                     lower_message.find("mo nhac") != std::string::npos ||
                     lower_message.find("chơi nhạc") != std::string::npos ||
                     lower_message.find("choi nhac") != std::string::npos ||
                     lower_message.find("resume") != std::string::npos ||
                     lower_message.find("play music") != std::string::npos ||
                     lower_message.find("continue") != std::string::npos) {
                music_action = "play";
            }
            // Volume up commands
            else if ((lower_message.find("tăng") != std::string::npos || lower_message.find("tang") != std::string::npos) &&
                     (lower_message.find("âm lượng") != std::string::npos || lower_message.find("am luong") != std::string::npos ||
                      lower_message.find("volume") != std::string::npos)) {
                music_action = "volume_up";
            }
            // Volume down commands
            else if ((lower_message.find("giảm") != std::string::npos || lower_message.find("giam") != std::string::npos) &&
                     (lower_message.find("âm lượng") != std::string::npos || lower_message.find("am luong") != std::string::npos ||
                      lower_message.find("volume") != std::string::npos)) {
                music_action = "volume_down";
            }
            
            // ========== PLAY SPECIFIC SONG IMMEDIATELY ==========
            // Detect "bật bài X", "nghe bài X", "phát bài X", "play X" and play immediately
            std::string song_to_play = "";
            
            // Vietnamese patterns: bật bài, nghe bài, phát bài, mở bài, chơi bài
            // Also: bật, phát, nghe, mở (without "bài") for simpler commands
            std::vector<std::string> vn_patterns = {
                // Full patterns with "bài"
                "bật bài ", "bat bai ", "nghe bài ", "nghe bai ", 
                "phát bài ", "phat bai ", "mở bài ", "mo bai ",
                "chơi bài ", "choi bai ", "cho nghe ", "cho tui nghe ",
                "cho tôi nghe ", "bật nhạc ", "bat nhac ", "nghe nhạc ",
                "nghe nhac ", "phát nhạc ", "phat nhac ",
                // Short patterns without "bài" - for "phát sóng gió", "bật lạc trôi"
                "phát ", "phat ", "bật ", "bat ", "nghe ", "mở ", "mo "
            };
            
            // English patterns
            std::vector<std::string> en_patterns = {
                "play ", "play song ", "play the song "
            };
            
            // Check Vietnamese patterns
            for (const auto& pattern : vn_patterns) {
                size_t pos = lower_message.find(pattern);
                if (pos != std::string::npos) {
                    // Extract song name after the pattern
                    song_to_play = message.substr(pos + pattern.length());
                    // Trim whitespace
                    size_t start = song_to_play.find_first_not_of(" \t\n\r");
                    size_t end = song_to_play.find_last_not_of(" \t\n\r");
                    if (start != std::string::npos && end != std::string::npos) {
                        song_to_play = song_to_play.substr(start, end - start + 1);
                    }
                    break;
                }
            }
            
            // Check English patterns if no Vietnamese match
            if (song_to_play.empty()) {
                for (const auto& pattern : en_patterns) {
                    size_t pos = lower_message.find(pattern);
                    if (pos != std::string::npos) {
                        song_to_play = message.substr(pos + pattern.length());
                        size_t start = song_to_play.find_first_not_of(" \t\n\r");
                        size_t end = song_to_play.find_last_not_of(" \t\n\r");
                        if (start != std::string::npos && end != std::string::npos) {
                            song_to_play = song_to_play.substr(start, end - start + 1);
                        }
                        break;
                    }
                }
            }
            
            // If we found a song to play, play it immediately without asking LLM
            // Filter out control keywords that are not song names
            std::vector<std::string> control_keywords = {
                "nhạc", "nhac", "tiếp", "tiep", "lại", "lai", "dừng", "dung", 
                "tạm", "tam", "stop", "pause", "next", "previous", "skip",
                "âm lượng", "am luong", "volume", "tăng", "tang", "giảm", "giam"
            };
            bool is_control_keyword = false;
            std::string lower_song = song_to_play;
            std::transform(lower_song.begin(), lower_song.end(), lower_song.begin(), ::tolower);
            for (const auto& keyword : control_keywords) {
                if (lower_song == keyword || lower_song.find(keyword) == 0) {
                    is_control_keyword = true;
                    break;
                }
            }
            
            if (!song_to_play.empty() && song_to_play.length() > 1 && !is_control_keyword) {
                ESP_LOGI(TAG, "🎵 Direct play request detected: '%s'", song_to_play.c_str());
                
                // Play music directly using Board's Music
                auto music = Board::GetInstance().GetMusic();
                if (music) {
                    // IMPORTANT: Abort any TTS that might come from server
                    // Set flag to ignore incoming TTS responses for this session
                    AbortSpeaking(kAbortReasonNone);
                    
                    // Set device to IDLE state immediately when music starts
                    SetDeviceState(kDeviceStateIdle);
                    
                    // Show notification on display
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->SetChatMessage("assistant", ("🎵 Đang phát: " + song_to_play).c_str());
                    }
                    
                    // Play the song directly - no need to ask LLM  
                    // Note: Download() will automatically disable wake word to free SRAM
                    std::string song_copy = song_to_play;
                    Schedule([this, song_copy]() {
                        auto m = Board::GetInstance().GetMusic();
                        if (m) {
                            // 🎵 Kiểm tra xem nhạc đã đang tải/phát chưa
                            if (m->IsPreparing() || m->IsDownloading() || m->IsPlaying()) {
                                ESP_LOGI(TAG, "🎵 Music already %s, skipping direct play", 
                                         m->IsPlaying() ? "playing" : 
                                         m->IsDownloading() ? "downloading" : "preparing");
                                return;
                            }
                            // Abort speaking again in case TTS started
                            AbortSpeaking(kAbortReasonNone);
                            ESP_LOGI(TAG, "🎵 Starting direct playback: %s", song_copy.c_str());
                            m->Download(song_copy, "");
                        }
                    });
                    
                    // DO NOT send anything to server - play music locally only
                    // This prevents LLM from responding with TTS which conflicts with music
                    ESP_LOGI(TAG, "🎵 Skipping LLM - playing music directly");
                    
                    // Return immediately - root is const, don't delete it
                    return;
                }
            }
            // ========== END PLAY SPECIFIC SONG ==========
            
            // If music control command detected, send signal to server
            if (!music_action.empty()) {
                ESP_LOGI(TAG, "🎵 Detected music control command: '%s' -> action: %s", message.c_str(), music_action.c_str());
                
                // Create JSON properly to prevent injection
                cJSON* json_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(json_obj, "type", "music_control");
                cJSON_AddStringToObject(json_obj, "action", music_action.c_str());
                cJSON_AddStringToObject(json_obj, "text", message.c_str());
                char* json_str = cJSON_PrintUnformatted(json_obj);
                std::string music_control_json = json_str ? json_str : "{}";
                cJSON_free(json_str);
                cJSON_Delete(json_obj);
                ESP_LOGI(TAG, "🎵 Sending music_control to server: %s", music_control_json.c_str());
                
                Schedule([this, music_control_json]() {
                    if (protocol_ && protocol_->IsAudioChannelOpened()) {
                        protocol_->SendJsonText(music_control_json);
                        ESP_LOGI(TAG, "✅ Music control signal sent to server");
                    } else {
                        ESP_LOGW(TAG, "⚠️ Cannot send music control - channel not open");
                    }
                });
                
                // Show visual feedback
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    std::string emoji = "🎵";
                    if (music_action == "next") emoji = "⏭️";
                    else if (music_action == "previous") emoji = "⏮️";
                    else if (music_action == "pause") emoji = "⏸️";
                    else if (music_action == "play") emoji = "▶️";
                    else if (music_action == "volume_up") emoji = "🔊";
                    else if (music_action == "volume_down") emoji = "🔉";
                    
                    display->ShowNotification(emoji + " " + music_action, 2000);
                }
                
                // Still send to AI server so it can respond appropriately
                // Don't return here - let the message continue to AI for response
            }
#endif
            
            Schedule([this, display, message]() {
                display->SetChatMessage("user", message.c_str());
            });
        } else if (strcmp(type->valuestring, "llm") == 0) {
            // 🎵 LOGIC QUAN TRỌNG: Block LLM khi nhạc đang tải hoặc đang buffer
            //
            // Khi nhạc đang tải (IsPreparing) hoặc đang buffer (IsDownloading):
            //   - User ĐÃ CHỌN BÀI → không cần AI phản hồi thêm
            //   - LLM emotion (😊, 😔) THỪA THÃI → user đang chờ nhạc
            //   - SSL CẦN SRAM → LLM processing chiếm SRAM
            //   - Device NÊN IM LẶNG → về IDLE ngay để stream nhạc
            //   - Sau khi MCP trả kết quả, LLM có thể gửi thêm text/emotion
            //     nhưng user không cần → block luôn
            //
            // Khi nhạc ĐÃ PHÁT (IsPlaying) mà KHÔNG còn downloading:
            //   - CHO PHÉP LLM → nếu user hỏi gì thì vẫn trả lời
            //   - Music sẽ TỰ PAUSE trong PlayAudioStream loop
            auto music = Board::GetInstance().GetMusic();
            if (music && (music->IsPreparing() || music->IsDownloading())) {
                ESP_LOGI(TAG, "🎵 Music is %s - user đã chọn bài, LLM về chế độ chờ, bỏ qua LLM message",
                         music->IsPreparing() ? "preparing" : "downloading/buffering");
                // Đảm bảo device ở IDLE state
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                return;
            }
            
            ESP_LOGI(TAG, "Processing LLM message from server");
            // Extract and display text from LLM message
            auto text = cJSON_GetObjectItem(root, "text");
            if (text && cJSON_IsString(text)) {
                std::string text_value = text->valuestring;
                ESP_LOGI(TAG, "LLM message text: '%s' (length: %d)", text_value.c_str(), (int)text_value.length());
                
                // Check if text is meaningful (not just emoji or whitespace)
                // Filter out pure emoji messages (they should only set emotion, not display as text)
                bool is_meaningful_text = false;
                if (text_value.length() > 0) {
                    // Simple check: if text is very short (1-2 chars) and contains only emoji-like characters, skip it
                    // Otherwise, display it (even if it contains emoji mixed with text)
                    bool has_printable_ascii = false;
                    for (size_t i = 0; i < text_value.length() && i < 100; i++) {  // Check first 100 chars
                        unsigned char c = text_value[i];
                        // Check for printable ASCII (letters, numbers, punctuation)
                        if (c >= 0x20 && c < 0x7F && c != 0x7F) {
                            has_printable_ascii = true;
                            break;
                        }
                    }
                    // Display if: has ASCII text, or is longer than 2 characters (likely meaningful)
                    is_meaningful_text = has_printable_ascii || text_value.length() > 2;
                }
                
                if (is_meaningful_text) {
                    // Display LLM text as assistant message (this is the main response)
                    Schedule([this, display, message = std::string(text_value)]() {
                        display->SetChatMessage("assistant", message.c_str());
                        ESP_LOGI(TAG, "Displayed LLM message: %s", message.c_str());
                    });
                } else if (text_value.length() > 0) {
                    // Text is likely just emoji or placeholder, log but don't display as chat message
                    ESP_LOGD(TAG, "LLM message contains only emoji/whitespace, skipping text display: '%s'", text_value.c_str());
                } else {
                    ESP_LOGW(TAG, "LLM message has empty text field");
                }
            } else {
                ESP_LOGW(TAG, "LLM message missing or invalid 'text' field");
            }
            // Extract and set emotion (always set emotion if provided, even if text is not displayed)
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion && cJSON_IsString(emotion)) {
                std::string emotion_value = emotion->valuestring;
                ESP_LOGI(TAG, "LLM message emotion: %s", emotion_value.c_str());
                
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
                // Block LLM emotion if we're forcing "winking" emoji for QR code (30s)
                if (force_winking_emoji_.load()) {
                    ESP_LOGI(TAG, "🚫 Blocked LLM emotion '%s' - keeping 'winking' emoji for QR code", emotion_value.c_str());
                    // Keep "winking" emoji, don't change it
                    return;
                }
                
                // Block LLM emotion if we're forcing "silly" emoji for celebration
                if (force_silly_emoji_.load()) {
                    ESP_LOGI(TAG, "🚫 Blocked LLM emotion '%s' - keeping 'silly' emoji for celebration", emotion_value.c_str());
                    // Keep "silly" emoji, don't change it
                    return;
                }
                
                // Block LLM emotion if we're forcing "delicious" emoji for custom keyword
                if (force_delicious_emoji_.load()) {
                    ESP_LOGI(TAG, "🚫 Blocked LLM emotion '%s' - keeping 'delicious' emoji for custom keyword", emotion_value.c_str());
                    // Keep "delicious" emoji, don't change it
                    return;
                }
#endif
                
                Schedule([this, display, emotion_str = std::string(emotion_value)]() {
                    // Check if display is using Otto emoji mode
                    if (display->IsUsingOttoEmoji()) {
                        // In Otto emoji mode, use GIF emojis directly
                        display->SetEmotion(emotion_str.c_str());
                    } else {
                        // In Twemoji mode, use Twemoji Unicode characters
                        // This will be handled by LcdDisplay::SetEmotion which now prioritizes Unicode
                        display->SetEmotion(emotion_str.c_str());
                    }
                });
            } else {
                ESP_LOGD(TAG, "LLM message missing or invalid 'emotion' field");
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            ESP_LOGI(TAG, "Processing MCP message from server");
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (!payload || !cJSON_IsObject(payload)) {
                ESP_LOGW(TAG, "Invalid or missing 'payload' field in MCP message");
                char* json_str = cJSON_Print(root);
                if (json_str) {
                    ESP_LOGI(TAG, "Full JSON: %s", json_str);
                    free(json_str);
                }
                return;
            }
            ESP_LOGI(TAG, "Calling McpServer::ParseMessage()");
            McpServer::GetInstance().ParseMessage(payload);
            ESP_LOGI(TAG, "McpServer::ParseMessage() completed");
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        
#if defined(CONFIG_BOARD_TYPE_OTTO_ROBOT) || defined(CONFIG_BOARD_TYPE_KIKI)
        // Auto-start control panel after 5 seconds for 5 minutes (no IP display)
        ESP_LOGI(TAG, "⏰ Device ready, scheduling auto-start of control panel in 5 seconds");
        Schedule([this, display]() {
            vTaskDelay(pdMS_TO_TICKS(5000));  // Wait 5 seconds after device is ready
            
            ESP_LOGI(TAG, "🚀 Auto-starting control panel (will auto-stop after 5 minutes)");
            
            // Start webserver in background
            Schedule([this, display]() {
                try {
                    // Start webserver
                    esp_err_t webserver_result = otto_start_webserver();
                    if (webserver_result == ESP_OK) {
                        ESP_LOGI(TAG, "✅ Webserver auto-started successfully (will auto-stop after 5 min)");
                    } else {
                        ESP_LOGW(TAG, "⚠️ Webserver auto-start failed: %s", esp_err_to_name(webserver_result));
                    }
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "❌ Exception auto-starting webserver: %s", e.what());
                }
            });
            
            // Play notification sound for auto-start
            Schedule([this]() {
                ESP_LOGI(TAG, "🔔 Playing notification sound for auto-start web server");
                PlaySound("ding");  // Simple notification sound
            });
        });
#endif
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                try {
                    task();
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "❌ Exception in scheduled task: %s", e.what());
                } catch (...) {
                    ESP_LOGE(TAG, "❌ Unknown exception in scheduled task");
                }
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        // 🔊 Play activation sound IMMEDIATELY when wake word detected
        // This gives user instant feedback regardless of server connection status
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    // Don't go to Listening if music is playing - stay in IDLE
    auto music = Board::GetInstance().GetMusic();
    if (music && music->IsPlaying()) {
        ESP_LOGI(TAG, "🎵 Music is playing, ignoring SetListeningMode request");
        return;
    }
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    // 🎵 Học từ Maggotxy: Khi chuyển TỪ IDLE sang state khác → stop nhạc
    // Trừ khi đang bị suppress (music đang cố gắng force IDLE)
    if (previous_state == kDeviceStateIdle && state != kDeviceStateIdle) {
        if (!audio_stop_suppressed_.load()) {
            auto music = board.GetMusic();
            if (music && music->IsPlaying()) {
                ESP_LOGI(TAG, "🎵 Stopping music due to state change: IDLE -> %s", STATE_STRINGS[state]);
                music->StopStreaming(false);  // Don't send notification, just stop
            }
        } else {
            ESP_LOGI(TAG, "🎵 Music stop suppressed, not stopping music");
        }
    }
    
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running (unless we're sending text from web)
            if (!audio_service_.IsAudioProcessorRunning() && !skip_voice_processing_for_listening_.load()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            } else if (skip_voice_processing_for_listening_.load()) {
                // For web text input, send start listening but don't enable voice processing
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [this, display](int progress, size_t speed) {
        // Use Schedule instead of detached thread to prevent dangling pointer
        Schedule([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::ReloadCustomKeywords() {
    cached_keywords_.clear();
    cached_emoji_ = "delicious";
    cached_pose_ = "none";
    cached_action_slot_ = 0;
    keywords_loaded_ = true;
    
    nvs_handle_t nvs_handle;
    char kw_buf[128] = "";
    char emo_buf[32] = "delicious";
    char pose_buf[32] = "none";
    
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No custom keywords in NVS");
        return;
    }
    
    size_t kw_len = sizeof(kw_buf);
    size_t emo_len = sizeof(emo_buf);
    size_t pose_len = sizeof(pose_buf);
    nvs_get_str(nvs_handle, "delicious_kw", kw_buf, &kw_len);
    nvs_get_str(nvs_handle, "delicious_emo", emo_buf, &emo_len);
    nvs_get_str(nvs_handle, "delicious_pose", pose_buf, &pose_len);
    nvs_get_i8(nvs_handle, "kw_action_slot", &cached_action_slot_);
    nvs_close(nvs_handle);
    
    cached_emoji_ = emo_buf;
    cached_pose_ = pose_buf;
    
    if (strlen(kw_buf) == 0) {
        ESP_LOGI(TAG, "📋 No custom keywords configured");
        return;
    }
    
    // Pre-split keywords by comma/semicolon and trim whitespace
    // Store both original and lowercase (ASCII-only lowercase) for matching
    std::string kw_str = kw_buf;
    size_t pos = 0;
    while (pos < kw_str.length()) {
        size_t delim_pos = kw_str.find_first_of(",;", pos);
        if (delim_pos == std::string::npos) delim_pos = kw_str.length();
        
        std::string single_kw = kw_str.substr(pos, delim_pos - pos);
        
        // Trim whitespace
        while (!single_kw.empty() && single_kw.front() == ' ') single_kw.erase(0, 1);
        while (!single_kw.empty() && single_kw.back() == ' ') single_kw.pop_back();
        
        if (!single_kw.empty()) {
            // Store original keyword (Vietnamese UTF-8 works directly)
            cached_keywords_.push_back(single_kw);
            
            // Also store ASCII-lowercased version if different
            std::string kw_lower = single_kw;
            std::transform(kw_lower.begin(), kw_lower.end(), kw_lower.begin(), ::tolower);
            if (kw_lower != single_kw) {
                cached_keywords_.push_back(kw_lower);
            }
        }
        
        pos = delim_pos + 1;
    }
    
    ESP_LOGI(TAG, "📋 Loaded %d keyword variants, emoji='%s', pose='%s', action_slot=%d", 
             (int)cached_keywords_.size(), cached_emoji_.c_str(), cached_pose_.c_str(), cached_action_slot_);
    for (const auto& kw : cached_keywords_) {
        ESP_LOGI(TAG, "  🔑 Keyword: '%s'", kw.c_str());
    }
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

bool Application::SendRawText(const std::string& json_text) {
    ESP_LOGI(TAG, "SendRawText called, length: %d", (int)json_text.length());
    if (protocol_ == nullptr) {
        ESP_LOGW(TAG, "Protocol is null in SendRawText");
        return false;
    }
    // Ensure send on main loop thread
    bool ok = false;
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        ESP_LOGI(TAG, "On main loop thread, sending directly");
        ok = protocol_->SendJsonText(json_text);
        ESP_LOGI(TAG, "SendJsonText result: %s", ok ? "success" : "failed");
    } else {
        ESP_LOGI(TAG, "Not on main loop thread, scheduling");
        // Schedule the send operation on main thread
        // Note: We return true here assuming the task will be scheduled successfully
        // The actual send result will be logged but not returned
        Schedule([this, json_text]() {
            ESP_LOGI(TAG, "Scheduled SendJsonText executing");
            bool result = protocol_->SendJsonText(json_text);
            ESP_LOGI(TAG, "Scheduled SendJsonText result: %s", result ? "success" : "failed");
            if (!result) {
                ESP_LOGE(TAG, "Failed to send JSON text in scheduled task");
            }
        });
        // Return true to indicate task was scheduled
        // The actual send happens asynchronously
        ok = true;
    }
    return ok;
}

bool Application::SendSttMessage(const std::string& text) {
    ESP_LOGI(TAG, "SendSttMessage called with text: %s", text.c_str());
    
    if (protocol_ == nullptr) {
        ESP_LOGW(TAG, "Protocol is null, cannot send STT message");
        return false;
    }
    
    // Validate text length - MQTT has practical limits
    const size_t MAX_TEXT_LENGTH = 1500;
    std::string text_to_send = text;
    if (text.length() > MAX_TEXT_LENGTH) {
        ESP_LOGW(TAG, "Text too long (%d chars), truncating to %d chars", (int)text.length(), (int)MAX_TEXT_LENGTH);
        text_to_send = text.substr(0, MAX_TEXT_LENGTH);
        if (auto display = Board::GetInstance().GetDisplay()) {
            std::string truncated_msg = text_to_send + "...";
            display->SetChatMessage("user", truncated_msg.c_str());
        }
    }
    
    // Show the text on display immediately
    if (auto display = Board::GetInstance().GetDisplay()) {
        display->SetChatMessage("user", text_to_send.c_str());
    }
    
    // Ensure audio channel is opened (for WebSocket connection)
    if (!protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Audio channel not opened, opening now...");
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGE(TAG, "Failed to open audio channel for STT message");
            return false;
        }
    }
    
    // Escape JSON special characters in text
    auto json_escape = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[7];
                        snprintf(buf, sizeof(buf), "\\u%04X", c);
                        out += buf;
                    } else {
                        out.push_back((char)c);
                    }
            }
        }
        return out;
    };
    
    std::string escaped_text = json_escape(text_to_send);
    ESP_LOGI(TAG, "Text to send (escaped length: %d): %s", (int)escaped_text.length(), text_to_send.c_str());
    
    // For web text input, we need to:
    // 1. Stop any ongoing voice processing (so mic audio doesn't override our text)
    // 2. Send the text as a wake word detection message
    // 3. Send stop listening to tell server to process the text immediately
    // Unlike voice input, we DON'T want to enable audio processor or wait for more audio
    
    // Step 1: Disable voice processing to prevent mic audio from interfering
    ESP_LOGI(TAG, "Disabling voice processing for text input");
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    
    // Step 2: Send listen/detect message with user text
    // This is like SendWakeWordDetected but the text is the actual user message
    protocol_->SendWakeWordDetected(escaped_text);
    ESP_LOGI(TAG, "Sent listen/detect with text: %s", text_to_send.c_str());
    
    // Step 3: Send listen/stop to tell server to process the text immediately
    // Server will treat the text in detect message as the user's speech
    protocol_->SendStopListening();
    ESP_LOGI(TAG, "Sent listen/stop, server should now process the text");
    
    // Keep state as listening to receive TTS response
    // Server will send tts/start and then audio
    
    return true;
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

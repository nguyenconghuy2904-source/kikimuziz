#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <wifi_station.h>
#include <cstring>
#include <string>

#include "application.h"
#include "codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "device_state_event.h"
#include "display/lcd_display.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "otto_emoji_display.h"
#include "otto_webserver.h"
#include "drawing_display.h"
#include "udp_draw_service.h"
#include "power_manager.h"
#include "system_reset.h"
#include "wifi_board.h"
#include "boards/common/esp32_music.h"
#include "kiki_led_control.h"

#define TAG "OttoRobot"

extern void InitializeOttoController();

// C++ wrapper for web server functions
extern "C" {
    esp_err_t otto_start_webserver(void);
}

// Static timer for ASR error emotion reset (avoids blocking main event loop)
static TimerHandle_t asr_error_reset_timer = nullptr;
static void asr_error_reset_callback(TimerHandle_t xTimer) {
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetEmotion("neutral");
    }
    ESP_LOGI(TAG, "😐 ASR error emotion reset to neutral");
}

class OttoRobot : public WifiBoard {
private:
    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    Esp32Music* music_player_;  // HTTP audio streaming music player
    // TTP223 is active HIGH on touch; enable power-save mode
    // Now on GPIO 12 instead of GPIO 2
#ifdef TOUCH_TTP223_GPIO
    Button touch_button_{TOUCH_TTP223_GPIO, true, 0, 0, true};
    bool touch_sensor_enabled_ = true;  // Touch sensor can be disabled via web UI
#endif
    
    // Touch counting for IP display feature
    int touch_count_ = 0;                 // Count consecutive touches
    uint32_t last_touch_time_ = 0;        // Time of last touch (for timeout)
    
    // UDP Drawing Service
    std::unique_ptr<DrawingDisplay> drawing_display_;
    std::unique_ptr<UdpDrawService> udp_draw_service_;
    
    // Charging state tracking
    bool is_charging_mode_ = false;
    
    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_ADC_UNIT, POWER_ADC_CHANNEL);
        
        // Set callback for charging status changes
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging && !is_charging_mode_) {
                // Just started charging - go to home position and stay there
                is_charging_mode_ = true;
                ESP_LOGI(TAG, "🔌 Charging detected! Going to home position...");
                
                // Show relaxed emoji while charging
                if (display_) {
                    display_->SetEmotion("relaxed");
                }
                
                // Go to home position immediately
                otto_controller_queue_action(ACTION_HOME, 1, 500, 0, 0);
                
                ESP_LOGI(TAG, "🔋 Robot in charging mode - holding home position");
            } else if (!is_charging && is_charging_mode_) {
                // Stopped charging
                is_charging_mode_ = false;
                ESP_LOGI(TAG, "🔌 Charging stopped. Robot can move freely now.");
                
                // Show happy emoji when charging complete
                if (display_) {
                    display_->SetEmotion("happy");
                }
            }
        });
    }
    
    // Check if robot is in charging mode (blocks other movements)
    bool IsChargingMode() const {
        return is_charging_mode_;
    }
    
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        
        // Load saved rotation from NVS or use default
        int32_t rotation_angle = 0;
        bool mirror_x = DISPLAY_MIRROR_X;
        bool mirror_y = DISPLAY_MIRROR_Y;
        bool swap_xy = DISPLAY_SWAP_XY;
        
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("display", NVS_READONLY, &nvs_handle);
        if (err == ESP_OK) {
            err = nvs_get_i32(nvs_handle, "rotation", &rotation_angle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "📖 Loaded screen rotation from NVS: %ld°", rotation_angle);
                
                // Calculate mirror and swap settings based on saved rotation
                switch (rotation_angle) {
                    case 0:   // 0° - default orientation
                        mirror_x = DISPLAY_MIRROR_X;
                        mirror_y = DISPLAY_MIRROR_Y;
                        swap_xy = DISPLAY_SWAP_XY;
                        break;
                    case 90:  // 90° clockwise
                        mirror_x = !DISPLAY_MIRROR_Y;
                        mirror_y = DISPLAY_MIRROR_X;
                        swap_xy = !DISPLAY_SWAP_XY;
                        break;
                    case 180: // 180° 
                        mirror_x = !DISPLAY_MIRROR_X;
                        mirror_y = !DISPLAY_MIRROR_Y;
                        swap_xy = DISPLAY_SWAP_XY;
                        break;
                    case 270: // 270° clockwise (90° counter-clockwise)
                        mirror_x = DISPLAY_MIRROR_Y;
                        mirror_y = !DISPLAY_MIRROR_X;
                        swap_xy = !DISPLAY_SWAP_XY;
                        break;
                    default:
                        ESP_LOGW(TAG, "⚠️ Invalid rotation angle %ld, using default", rotation_angle);
                        rotation_angle = 0;
                        mirror_x = DISPLAY_MIRROR_X;
                        mirror_y = DISPLAY_MIRROR_Y;
                        swap_xy = DISPLAY_SWAP_XY;
                        break;
                }
            } else {
                ESP_LOGW(TAG, "⚠️ No rotation setting found in NVS: %s", esp_err_to_name(err));
            }
            nvs_close(nvs_handle);
        } else {
            ESP_LOGW(TAG, "⚠️ Failed to open NVS for rotation: %s", esp_err_to_name(err));
        }
        
        // Apply rotation settings
        esp_lcd_panel_swap_xy(panel, swap_xy);
        esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
        
        if (rotation_angle != 0) {
            ESP_LOGI(TAG, "🔄 Applied screen rotation: %ld° (swap_xy=%d, mirror_x=%d, mirror_y=%d)", 
                     rotation_angle, swap_xy, mirror_x, mirror_y);
        }

        display_ = new OttoEmojiDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        // Show happy emoji on boot (emoji mode is already loaded from NVS in constructor)
        if (display_) {
            display_->SetEmotion("happy");  // Welcoming expression
            auto otto_display = static_cast<OttoEmojiDisplay*>(display_);
            ESP_LOGI(TAG, "🤖 Emoji mode: %s (loaded from NVS)", otto_display->IsUsingOttoEmoji() ? "Otto GIF" : "Twemoji");
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting &&
                !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        // Boot button long press (3 seconds) -> Toggle between Otto emoji and Twemoji
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "🔄 Boot button long pressed (3s) -> Toggling emoji mode");
            
            // Cast display to OttoEmojiDisplay to access emoji functions
            auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display_);
            if (otto_display) {
                // Toggle between Otto GIF emoji and Twemoji text mode
                bool current_mode = otto_display->IsUsingOttoEmoji();
                bool new_mode = !current_mode;
                
                otto_display->SetEmojiMode(new_mode);
                
                if (new_mode) {
                    ESP_LOGI(TAG, "🤖 Switched to OTTO GIF emoji mode");
                    otto_display->SetChatMessage("system", "🤖 Otto GIF Mode");
                } else {
                    ESP_LOGI(TAG, "😊 Switched to Twemoji text mode");
                    otto_display->SetChatMessage("system", "😊 Twemoji Mode");
                }
            } else {
                ESP_LOGW(TAG, "❌ Display is not OttoEmojiDisplay, cannot toggle emoji mode");
            }
        });

#ifdef TOUCH_TTP223_GPIO
        // TTP223 touch -> random action and emotion (if enabled)
        touch_button_.OnClick([this]() {
            if (!touch_sensor_enabled_) {
                ESP_LOGI(TAG, "🖐️ TTP223 touch detected but sensor is disabled");
                return;
            }
            
            // Get current time for touch timeout handling
            uint32_t current_time = esp_timer_get_time() / 1000000; // Convert to seconds
            
            // Check if touch is within timeout window (5 seconds)
            if (current_time - last_touch_time_ > 5) {
                // Reset count if too much time has passed
                touch_count_ = 0;
            }
            
            // Increment touch count
            touch_count_++;
            last_touch_time_ = current_time;
            
            ESP_LOGI(TAG, "🖐️ Touch #%d detected", touch_count_);
            
            // Check if we've reached 5 touches
            if (touch_count_ >= 5) {
                ESP_LOGI(TAG, "🔗 5 touches detected! Displaying IP address...");
                DisplayStationIP();
                touch_count_ = 0; // Reset counter
                return; // Don't execute normal touch action
            }
            
            // Random number generator
            uint32_t random_val = esp_random();
            
            // Greet sequence function ID (will handle via queue)
            const int ACTION_GREET_SEQUENCE = 100;
            const int ACTION_CELEBRATE_SEQUENCE = 101;
            
            // Array of actions to choose from (6 specific actions with fixed emoji)
            
            struct ActionWithEmoji {
                int action_type;
                int param1;
                int param2;
                const char* name;
                bool is_sequence;
                const char* emoji;  // Fixed emoji for this action
            };
            
            const ActionWithEmoji actions[] = {
                {ACTION_GREET_SEQUENCE, 0, 0, "Greet (Chào Hỏi)", true, "happy"},          // 0: home → wave → bow
                {ACTION_CELEBRATE_SEQUENCE, 0, 0, "Celebrate (Ăn Mừng)", true, "happy"},   // 1: dance → wave → swing
                {ACTION_DOG_DANCE, 2, 200, "Dance (Nhảy Múa)", false, "happy"},            // 2
                {ACTION_DOG_SIT_DOWN, 1, 3000, "Sit (Ngồi)", false, "sleepy"},             // 3
                {ACTION_DOG_LIE_DOWN, 1, 1500, "Lie (Nằm)", false, "sleepy"},              // 4
                {ACTION_DOG_SCRATCH, 5, 50, "Scratch (Gãi Ngứa)", false, "neutral"}        // 5
            };
            const int num_actions = sizeof(actions) / sizeof(actions[0]);
            
            // Pick random action only (emoji is fixed per action)
            int action_idx = random_val % num_actions;
            
            const ActionWithEmoji& chosen_action = actions[action_idx];
            
            ESP_LOGI(TAG, "🖐️ TTP223 touch -> Random action: %s (emoji: %s)", 
                     chosen_action.name, chosen_action.emoji);
            
            // Show fixed emoji for this action
            auto display = GetDisplay();
            if (display) display->SetEmotion(chosen_action.emoji);
            
            // Queue random action (handle sequences specially)
            if (chosen_action.is_sequence) {
                if (chosen_action.action_type == 100) {
                    // Greet sequence: home → wave → bow
                    ESP_LOGI(TAG, "👋 Executing Greet sequence");
                    otto_controller_queue_action(ACTION_HOME, 1, 500, 0, 0);
                    otto_controller_queue_action(ACTION_DOG_WAVE_RIGHT_FOOT, 3, 150, 0, 0);
                    otto_controller_queue_action(ACTION_DOG_BOW, 2, 150, 0, 0);
                } else if (chosen_action.action_type == 101) {
                    // Celebrate sequence: dance → wave → swing
                    ESP_LOGI(TAG, "🎉 Executing Celebrate sequence");
                    otto_controller_queue_action(ACTION_DOG_DANCE, 2, 200, 0, 0);
                    otto_controller_queue_action(ACTION_DOG_WAVE_RIGHT_FOOT, 5, 100, 0, 0);
                    otto_controller_queue_action(ACTION_DOG_SWING, 3, 10, 0, 0);  // Changed from 150 to 10 for faster swing
                }
            } else {
                // Simple single action
                otto_controller_queue_action(chosen_action.action_type, 
                                            chosen_action.param1, 
                                            chosen_action.param2, 0, 0);
            }
        });
#endif // TOUCH_TTP223_GPIO
    }

    void InitializeOttoController() {
        ESP_LOGI(TAG, "初始化Otto机器人MCP控制器");
        ::InitializeOttoController();
    }
    
    void InitializeLedStrip() {
        ESP_LOGI(TAG, "🌈 Initializing LED strip...");
        kiki_led_init();
        ESP_LOGI(TAG, "✅ LED strip initialized");
    }

    void InitializeUdpDrawingService() {
        // UDP Drawing Service - DISABLED
        ESP_LOGI(TAG, "🎨 UDP Drawing Service: DISABLED");
        
        // Create DrawingDisplay (same size as main display)
        drawing_display_ = std::make_unique<DrawingDisplay>(display_->width(), display_->height());
        drawing_display_->StartDisplay();
        
        // Create UDP Drawing Service with drawing display and port 12345
        udp_draw_service_ = std::make_unique<UdpDrawService>(drawing_display_.get(), 12345);
        
        // Set pointers for web UI access
        otto_set_udp_draw_service(udp_draw_service_.get());
        otto_set_drawing_display(drawing_display_.get());
        
        ESP_LOGI(TAG, "✅ UDP Drawing Service initialized on port 12345");
        ESP_LOGI(TAG, "📱 Service will start when WiFi connects");
    }

    void InitializeWebServer() {
        ESP_LOGI(TAG, "Initializing Otto Web Controller");
        
        // Web server will NOT auto-start on boot
        // It will only start when user explicitly requests it (e.g., "mở trang điều khiển")
        ESP_LOGI(TAG, "🌐 Web server will NOT auto-start - manual start only");
        
        // Removed auto-start code - webserver will only start when explicitly requested
        // Users can start it by saying "mở trang điều khiển" or similar commands
    }

    void InitializeStateChangeCallback() {
        ESP_LOGI(TAG, "Registering device state change callback");
        
        // Create one-shot timer for ASR error emotion reset (3.5 seconds)
        if (asr_error_reset_timer == nullptr) {
            asr_error_reset_timer = xTimerCreate(
                "asr_err_timer",
                pdMS_TO_TICKS(3500),  // 3.5 seconds
                pdFALSE,              // One-shot, not auto-reload
                nullptr,
                asr_error_reset_callback
            );
        }
        
        auto& state_manager = DeviceStateEventManager::GetInstance();
        state_manager.RegisterStateChangeCallback(
            [this](DeviceState previous_state, DeviceState current_state) {
                ESP_LOGI(TAG, "🔄 State changed: %d -> %d", previous_state, current_state);
                
                // When ASR fails (listening -> idle without proper recognition)
                if (previous_state == kDeviceStateListening && 
                    current_state == kDeviceStateIdle) {
                    
                    ESP_LOGW(TAG, "❌ ASR error detected - Robot will lie down");
                    
                    // Display confused emotion
                    if (display_) {
                        display_->SetEmotion("confused");
                    }
                    
                    // Lie down with speed 3200 - queued, non-blocking
                    otto_controller_queue_action(ACTION_DOG_LIE_DOWN, 1, 3200, 0, 0);
                    ESP_LOGI(TAG, "🛏️ Queued lie down action (speed 3200)");
                    
                    // Use timer to reset emotion after 3.5 seconds (NON-BLOCKING!)
                    // This allows wake word detection to work during the delay
                    if (asr_error_reset_timer != nullptr) {
                        xTimerStop(asr_error_reset_timer, 0);
                        xTimerStart(asr_error_reset_timer, 0);
                    }
                }
                
                // When speaking starts
                else if (current_state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "🗣️ Speaking");
                    if (display_) {
                        display_->SetEmotion("happy");
                    }
                }
            }
        );
        
        ESP_LOGI(TAG, "✅ State change callback registered");
    }

public:
    // Boot button with 3 second long press time for toggling emoji mode (Otto GIF / Twemoji)
    // Parameters: gpio_num, active_high=false, long_press_time=3000ms, short_press_time=0
    OttoRobot() : boot_button_(BOOT_BUTTON_GPIO, false, 3000, 0, false) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeOttoController();
        InitializeUdpDrawingService();
        InitializeLedStrip();
        InitializeWebServer();
        InitializeStateChangeCallback();
        GetBacklight()->RestoreBrightness();
        
        // Initialize music player
        music_player_ = new Esp32Music();
        otto_set_music_player(music_player_);  // Set pointer for web UI
        ESP_LOGI(TAG, "🎵 Music player initialized");
    }
    
    ~OttoRobot() {
        // Music player cleanup
        if (music_player_) {
            music_player_->StopStreaming();
            delete music_player_;
            music_player_ = nullptr;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        
        // Load saved mic gain from NVS and apply it
        static bool mic_gain_loaded = false;
        if (!mic_gain_loaded) {
            nvs_handle_t nvs_handle;
            int32_t saved_gain = 30;  // Default value
            if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
                if (nvs_get_i32(nvs_handle, "mic_gain", &saved_gain) == ESP_OK) {
                    audio_codec.SetInputGain((float)saved_gain);
                    ESP_LOGI(TAG, "🎤 Loaded saved mic gain: %d", (int)saved_gain);
                }
                nvs_close(nvs_handle);
            }
            mic_gain_loaded = true;
        }
        
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual Music* GetMusic() override { 
        return music_player_;  // Return HTTP streaming music player
    }

    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        
        // Start UDP Drawing Service sau khi WiFi connected - DISABLED
        // ESP_LOGI(TAG, "🎨 Starting UDP Drawing Service...");
        // if (udp_draw_service_ && udp_draw_service_->Start()) {
        //     ESP_LOGI(TAG, "✅ UDP Drawing Service started on port 12345");
        //     ESP_LOGI(TAG, "🎨 Drawing web UI: http://[IP]/draw");
        // }
    }

public:
#ifdef TOUCH_TTP223_GPIO
    // Method to control touch sensor state
    void SetTouchSensorEnabled(bool enabled) {
        touch_sensor_enabled_ = enabled;
        ESP_LOGI(TAG, "🖐️ Touch sensor %s", enabled ? "ENABLED" : "DISABLED");
    }
    
    bool IsTouchSensorEnabled() const {
        return touch_sensor_enabled_;
    }
#endif // TOUCH_TTP223_GPIO
    
    // Display Station IP address on screen
    void DisplayStationIP() {
        ESP_LOGI(TAG, "📱 Displaying Station IP address...");
        
        // Get WiFi station IP
        auto& wifi_station = WifiStation::GetInstance();
        if (!wifi_station.IsConnected()) {
            ESP_LOGW(TAG, "❌ WiFi not connected, cannot display IP");
            if (display_) {
                display_->SetChatMessage("system", "WiFi chưa kết nối!");
            }
            return;
        }
        
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[64];
            snprintf(ip_str, sizeof(ip_str), "Station IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "\033[1;33m📱 Station IP: " IPSTR "\033[0m", IP2STR(&ip_info.ip));
            
            if (display_) {
                display_->SetChatMessage("system", ip_str);
            }
        } else {
            ESP_LOGE(TAG, "❌ Failed to get IP info");
            if (display_) {
                display_->SetChatMessage("system", "Không thể lấy IP!");
            }
        }
    }
};

DECLARE_BOARD(OttoRobot);

// C interface for touch sensor control
extern "C" {
#ifdef TOUCH_TTP223_GPIO
    void otto_set_touch_sensor_enabled(bool enabled) {
        auto& board = Board::GetInstance();
        auto otto_board = dynamic_cast<OttoRobot*>(&board);
        if (otto_board) {
            otto_board->SetTouchSensorEnabled(enabled);
        }
    }
    
    bool otto_is_touch_sensor_enabled() {
        auto& board = Board::GetInstance();
        auto otto_board = dynamic_cast<OttoRobot*>(&board);
        if (otto_board) {
            return otto_board->IsTouchSensorEnabled();
        }
        return false;
    }
#endif // TOUCH_TTP223_GPIO

    // Music player helper functions for web UI
    static Esp32Music* s_music_player = nullptr;
    
    void otto_set_music_player(void* player) {
        s_music_player = static_cast<Esp32Music*>(player);
        ESP_LOGI("OttoMusic", "🎵 Music player pointer set");
    }
    
    bool otto_music_download_and_play(const std::string& song) {
        if (!s_music_player) {
            ESP_LOGE("OttoMusic", "❌ Music player not initialized");
            return false;
        }
        
        ESP_LOGI("OttoMusic", "🎵 Searching and playing: %s", song.c_str());
        
        // Stop any current playback first (don't send notification since we're switching songs)
        s_music_player->StopStreaming(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Download and play the song
        bool result = s_music_player->Download(song, "");
        if (result) {
            ESP_LOGI("OttoMusic", "✅ Started playing: %s", song.c_str());
        } else {
            ESP_LOGE("OttoMusic", "❌ Failed to play: %s", song.c_str());
        }
        return result;
    }
    
    void otto_music_stop() {
        if (s_music_player) {
            ESP_LOGI("OttoMusic", "⏹️ Stopping music playback");
            s_music_player->StopStreaming();
        }
    }
    
    bool otto_music_get_status(bool* playing, size_t* buffer_size, char* song, int song_len, 
                               char* artist, int artist_len, char* thumbnail, int thumb_len) {
        if (!s_music_player) {
            if (playing) *playing = false;
            if (buffer_size) *buffer_size = 0;
            if (song && song_len > 0) song[0] = '\0';
            if (artist && artist_len > 0) artist[0] = '\0';
            if (thumbnail && thumb_len > 0) thumbnail[0] = '\0';
            return false;
        }
        
        if (playing) *playing = s_music_player->IsPlaying();
        if (buffer_size) *buffer_size = s_music_player->GetBufferSize();
        if (song && song_len > 0) {
            std::string song_name = s_music_player->GetCurrentSongName();
            strncpy(song, song_name.c_str(), song_len - 1);
            song[song_len - 1] = '\0';
        }
        if (artist && artist_len > 0) {
            std::string artist_name = s_music_player->GetCurrentArtist();
            strncpy(artist, artist_name.c_str(), artist_len - 1);
            artist[artist_len - 1] = '\0';
        }
        if (thumbnail && thumb_len > 0) {
            std::string thumb_url = s_music_player->GetCurrentThumbnail();
            strncpy(thumbnail, thumb_url.c_str(), thumb_len - 1);
            thumbnail[thumb_len - 1] = '\0';
        }
        return true;
    }
}

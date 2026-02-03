#include "otto_webserver.h"
#include "mcp_server.h"
#include "application.h"
#include "otto_emoji_display.h"
#include "board.h"
#include <cJSON.h>
#include <stdio.h>
#include <nvs_flash.h>

// TAG used by both C and C++ code
static const char *TAG = "OttoWeb";

extern "C" {

// Global variables
bool webserver_enabled = false;
static httpd_handle_t server = NULL;
static int s_retry_num = 0;

// Auto pose change variables
static bool auto_pose_enabled = false;
static TimerHandle_t auto_pose_timer = NULL;
static uint32_t auto_pose_interval_ms = 60000;  // Default 60 seconds
static char selected_poses[200] = "sit,wave,bow,stretch,swing,dance";  // Default all (removed jump)

// Auto emoji change variables
static bool auto_emoji_enabled = false;
static TimerHandle_t auto_emoji_timer = NULL;
static uint32_t auto_emoji_interval_ms = 10000;  // Default 10 seconds
static char selected_emojis[300] = "happy,laughing,winking,cool,love,surprised,excited,sleepy,sad,angry,confused,thinking,neutral,shocked";  // Default all

// Webserver auto-stop timer (30 minutes)
static TimerHandle_t webserver_auto_stop_timer = NULL;
static const uint32_t WEBSERVER_AUTO_STOP_DELAY_MS = 30 * 60 * 1000;  // 30 minutes

// Timer callback for webserver auto-stop
void webserver_auto_stop_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "⏱️ Webserver auto-stop timeout (30 min) - stopping webserver");
    otto_stop_webserver();
}

// Timer callback for auto pose change

// Timer callback for auto pose change
void auto_pose_timer_callback(TimerHandle_t xTimer) {
    if (!auto_pose_enabled) {
        return;
    }
    
    // Pose action structure
    struct PoseAction {
        const char* name;
        int action;
        int steps;
        int speed;
    };
    
    // All available poses
    const PoseAction all_poses[] = {
        {"sit", ACTION_DOG_SIT_DOWN, 1, 500},
        {"wave", ACTION_DOG_WAVE_RIGHT_FOOT, 3, 50},
        {"bow", ACTION_DOG_BOW, 1, 1500},
        {"stretch", ACTION_DOG_STRETCH, 2, 15},
        {"swing", ACTION_DOG_SWING, 3, 10},
        {"dance", ACTION_DOG_DANCE, 2, 200}
    };
    const int total_poses = sizeof(all_poses) / sizeof(all_poses[0]);
    
    // Build list of enabled poses
    PoseAction enabled_poses[6];  // Reduced from 7 to 6 (removed jump)
    int enabled_count = 0;
    
    for (int i = 0; i < total_poses; i++) {
        if (strstr(selected_poses, all_poses[i].name) != NULL) {
            enabled_poses[enabled_count++] = all_poses[i];
        }
    }
    
    if (enabled_count == 0) {
        ESP_LOGW(TAG, "⚠️ No poses selected for auto mode");
        return;
    }
    
    // Get next pose (cycle through enabled poses)
    static int pose_index = 0;
    if (pose_index >= enabled_count) pose_index = 0;
    
    const PoseAction& current = enabled_poses[pose_index];
    otto_controller_queue_action(current.action, current.steps, current.speed, 0, 0);
    
    ESP_LOGI(TAG, "🤖 Auto pose change [%d/%d]: %s (action=%d, steps=%d, speed=%d)", 
             pose_index + 1, enabled_count, current.name, current.action, current.steps, current.speed);
    
    // Move to next pose
    pose_index = (pose_index + 1) % enabled_count;
}

// Timer callback for auto emoji change - IMPROVED: Now selects random emoji from enabled list
void auto_emoji_timer_callback(TimerHandle_t xTimer) {
    if (!auto_emoji_enabled) {
        return;
    }

    // Static list to avoid stack allocation
    static const char* all_emojis[] = {
        "happy", "laughing", "winking", "cool", "love",
        "surprised", "excited", "sleepy", "sad", "angry",
        "confused", "thinking", "neutral", "shocked"
    };
    static const int total_emojis = 14;

    // Count enabled emojis and find random one
    int enabled_count = 0;
    const char* selected_emoji = nullptr;

    // First pass: count enabled emojis
    for (int i = 0; i < total_emojis; i++) {
        if (strstr(selected_emojis, all_emojis[i]) != nullptr) {
            enabled_count++;
        }
    }

    if (enabled_count == 0) {
        ESP_LOGW(TAG, "⚠️ No emojis selected for auto mode");
        return;
    }

    // Second pass: select random enabled emoji
    int target_index = esp_random() % enabled_count;
    int current_index = 0;

    for (int i = 0; i < total_emojis; i++) {
        if (strstr(selected_emojis, all_emojis[i]) != nullptr) {
            if (current_index == target_index) {
                selected_emoji = all_emojis[i];
                break;
            }
            current_index++;
        }
    }

    if (selected_emoji == nullptr) {
        ESP_LOGW(TAG, "⚠️ Failed to select random emoji");
        return;
    }

    // Turn on display and set emoji (SetEmotion will also turn on display)
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetEmotion(selected_emoji);
        ESP_LOGI(TAG, "😊 Auto emoji: %s (random from %d enabled)", selected_emoji, enabled_count);
    }
}

// WiFi event handler for monitoring system WiFi connection
void otto_system_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "System WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "🌐 WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Web server will NOT auto-start - manual start only
        // Users can start it by saying "mở trang điều khiển" or similar commands
        ESP_LOGI(TAG, "📱 Web server will NOT auto-start - manual start only");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "System WiFi disconnected, Otto Web Controller stopped");
    }
}

// Register to listen for system WiFi events
esp_err_t otto_register_wifi_listener(void) {
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_disconnected;
    
    esp_err_t ret = esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &otto_system_wifi_event_handler,
                                                       NULL,
                                                       &instance_got_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                             WIFI_EVENT_STA_DISCONNECTED,
                                             &otto_system_wifi_event_handler,
                                             NULL,
                                             &instance_disconnected);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Otto WiFi event listener registered");
    return ESP_OK;
}

// WiFi event handler function (inside extern "C" block but separate definition)  
void otto_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to WiFi AP");
        } else {
            ESP_LOGI(TAG, "Failed to connect to WiFi AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "\033[1;33m🌟 WifiStation: Got IP: " IPSTR "\033[0m", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        
        // Web server will NOT auto-start - manual start only
        ESP_LOGI(TAG, "📱 Web server will NOT auto-start - manual start only");
    }
}

// Start HTTP server automatically when WiFi is connected
esp_err_t otto_auto_start_webserver_if_wifi_connected(void) {
    // Check if WiFi is already connected (from main system)
    wifi_ap_record_t ap_info;
    esp_err_t wifi_status = esp_wifi_sta_get_ap_info(&ap_info);
    
    if (wifi_status == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already connected to: %s", ap_info.ssid);
        
        // Get current IP
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "\033[1;33m🌟 Current IP: " IPSTR "\033[0m", IP2STR(&ip_info.ip));
                ESP_LOGI(TAG, "Otto Web Controller will be available at: http://" IPSTR, IP2STR(&ip_info.ip));
                
                // Start web server immediately
                return otto_start_webserver();
            }
        }
    } else {
        ESP_LOGI(TAG, "WiFi not connected yet, Otto Web Controller will start when WiFi connects");
    }
    
    return ESP_OK;
}

// Original WiFi initialization (for standalone mode if needed)
esp_err_t otto_wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &otto_wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &otto_wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished");

    return ESP_OK;
}

// Send main control page HTML
void send_otto_control_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    
    // Modern responsive HTML with Otto Robot theme
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>");
    httpd_resp_sendstr_chunk(req, "<title>Kiki Control - miniZ</title>");
    // Twemoji CDN for better emoji rendering
    httpd_resp_sendstr_chunk(req, "<script src='https://twemoji.maxcdn.com/v/latest/twemoji.min.js' crossorigin='anonymous'></script>");
    
    // CSS Styling - Optimized for Mobile
    httpd_resp_sendstr_chunk(req, "<style>");
    httpd_resp_sendstr_chunk(req, "* { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }");
    httpd_resp_sendstr_chunk(req, "body { font-family: 'Segoe UI', 'Roboto', sans-serif; background: linear-gradient(135deg, #f8f8f8 0%, #ffffff 100%); min-height: 100vh; display: flex; justify-content: center; align-items: flex-start; color: #000000; padding: 8px; padding-top: 10px; }");
    httpd_resp_sendstr_chunk(req, ".container { max-width: 600px; width: 100%; background: #ffffff; border-radius: 15px; padding: 15px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); border: 2px solid #000000; } @media (min-width: 768px) { .container { max-width: 800px; padding: 25px; } }");
    httpd_resp_sendstr_chunk(req, ".header { text-align: center; margin-bottom: 15px; }");
    httpd_resp_sendstr_chunk(req, ".header h1 { font-size: 1.5em; margin-bottom: 5px; color: #000000; font-weight: bold; } @media (min-width: 768px) { .header h1 { font-size: 2.2em; } }");
    httpd_resp_sendstr_chunk(req, ".status { background: #f0f0f0; color: #000; padding: 10px; border-radius: 10px; margin-bottom: 15px; text-align: center; border: 2px solid #000000; font-weight: bold; font-size: 0.9em; }");
    
    // Compact button styling for mobile
    httpd_resp_sendstr_chunk(req, ".control-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(100px, 1fr)); gap: 8px; margin-bottom: 15px; } @media (min-width: 768px) { .control-grid { grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 12px; } }");
    httpd_resp_sendstr_chunk(req, ".btn { background: #ffffff; border: 2px solid #000000; color: #000000; padding: 10px 12px; border-radius: 10px; cursor: pointer; font-size: 13px; font-weight: bold; transition: all 0.15s; box-shadow: 0 2px 5px rgba(0,0,0,0.15); touch-action: manipulation; user-select: none; } @media (min-width: 768px) { .btn { padding: 14px 18px; font-size: 15px; } }");
    httpd_resp_sendstr_chunk(req, ".btn:active { transform: scale(0.95); box-shadow: 0 1px 3px rgba(0,0,0,0.2); background: #f0f0f0; }");
    httpd_resp_sendstr_chunk(req, ".paw-btn { font-size: 18px; }");
    
    // Compact sections for mobile
    httpd_resp_sendstr_chunk(req, ".movement-section { margin-bottom: 15px; }");
    httpd_resp_sendstr_chunk(req, ".section-title { font-size: 1.1em; margin-bottom: 10px; text-align: center; color: #000000; font-weight: bold; } @media (min-width: 768px) { .section-title { font-size: 1.4em; } }");
    httpd_resp_sendstr_chunk(req, ".direction-pad { display: grid; grid-template-columns: 1fr 1fr 1fr; grid-template-rows: 1fr 1fr 1fr; gap: 8px; max-width: 250px; margin: 0 auto; } @media (min-width: 768px) { .direction-pad { gap: 12px; max-width: 300px; } }");
    httpd_resp_sendstr_chunk(req, ".direction-pad .btn { padding: 15px; font-size: 14px; font-weight: 700; min-height: 50px; } @media (min-width: 768px) { .direction-pad .btn { padding: 20px; font-size: 16px; } }");
    httpd_resp_sendstr_chunk(req, ".btn-forward { grid-column: 2; grid-row: 1; }");
    httpd_resp_sendstr_chunk(req, ".btn-left { grid-column: 1; grid-row: 2; }");
    httpd_resp_sendstr_chunk(req, ".btn-stop { grid-column: 2; grid-row: 2; background: #ffeeee; border-color: #cc0000; color: #cc0000; }");
    httpd_resp_sendstr_chunk(req, ".btn-right { grid-column: 3; grid-row: 2; }");
    httpd_resp_sendstr_chunk(req, ".btn-backward { grid-column: 2; grid-row: 3; }");
    // Auto pose toggle styling
    httpd_resp_sendstr_chunk(req, ".auto-toggle { background: #e8f5e9; border: 2px solid #4caf50; padding: 12px; border-radius: 10px; margin: 15px 0; text-align: center; }");
    httpd_resp_sendstr_chunk(req, ".toggle-btn { background: #ffffff; border: 2px solid #000; padding: 10px 20px; border-radius: 8px; font-weight: bold; font-size: 14px; cursor: pointer; }");
    httpd_resp_sendstr_chunk(req, ".toggle-btn.active { background: #4caf50; color: white; border-color: #2e7d32; }");
    // Page navigation styling
    httpd_resp_sendstr_chunk(req, ".page { display: none; }");
    httpd_resp_sendstr_chunk(req, ".page.active { display: block; }");
    httpd_resp_sendstr_chunk(req, ".nav-tabs { display: flex; gap: 10px; margin-bottom: 20px; }");
    httpd_resp_sendstr_chunk(req, ".nav-tab { flex: 1; background: #f0f0f0; border: 2px solid #000; padding: 12px; border-radius: 10px; text-align: center; font-weight: bold; cursor: pointer; transition: all 0.2s; }");
    httpd_resp_sendstr_chunk(req, ".nav-tab.active { background: #4caf50; color: white; border-color: #2e7d32; }");
    // Auto pose config styling
    httpd_resp_sendstr_chunk(req, ".pose-config { background: #f8f8f8; border: 2px solid #000; border-radius: 10px; padding: 15px; margin: 10px 0; }");
    httpd_resp_sendstr_chunk(req, ".pose-item { display: flex; align-items: center; gap: 10px; margin: 8px 0; padding: 8px; background: white; border-radius: 8px; border: 1px solid #ddd; }");
    httpd_resp_sendstr_chunk(req, ".pose-item input[type='checkbox'] { width: 20px; height: 20px; cursor: pointer; }");
    httpd_resp_sendstr_chunk(req, ".pose-item label { flex: 1; cursor: pointer; font-weight: 500; }");
    httpd_resp_sendstr_chunk(req, ".time-input { width: 80px; padding: 5px; border: 2px solid #000; border-radius: 5px; font-weight: bold; text-align: center; }");
    
    // Compact fun actions grid
    httpd_resp_sendstr_chunk(req, ".fun-actions { margin-top: 15px; }");
    httpd_resp_sendstr_chunk(req, ".action-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; } @media (min-width: 768px) { .action-grid { grid-template-columns: repeat(4, 1fr); gap: 10px; } }");
    
    // Compact emoji sections
    httpd_resp_sendstr_chunk(req, ".emoji-section, .emoji-mode-section { margin-top: 15px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }");
    httpd_resp_sendstr_chunk(req, ".mode-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 12px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-btn { background: #fff8e1; border: 2px solid #ff6f00; color: #e65100; padding: 10px; font-size: 13px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-btn:hover { background: #ffecb3; border-color: #e65100; }");
    // Ensure emoji display properly with Twemoji
    httpd_resp_sendstr_chunk(req, ".emoji-btn img.emoji { width: 1.2em; height: 1.2em; vertical-align: middle; margin-right: 4px; display: inline-block; }");
    httpd_resp_sendstr_chunk(req, ".mode-btn { background: #e8f5e8; border: 2px solid #4caf50; color: #2e7d32; padding: 12px 16px; }");
    httpd_resp_sendstr_chunk(req, ".mode-btn:hover { background: #c8e6c9; }");
    httpd_resp_sendstr_chunk(req, ".mode-btn.active { background: #4caf50; color: white; }");
    
    // Compact response area
    httpd_resp_sendstr_chunk(req, ".response { margin-top: 15px; padding: 15px; background: #f8f8f8; border-radius: 12px; min-height: 60px; box-shadow: inset 2px 2px 4px rgba(0,0,0,0.1); border: 2px solid #000; font-family: 'Courier New', monospace; font-size: 13px; }");
    
    // Volume control styling
    httpd_resp_sendstr_chunk(req, ".volume-section { margin-top: 25px; }");
    httpd_resp_sendstr_chunk(req, "input[type='range'] { -webkit-appearance: none; width: 100%; height: 10px; border-radius: 5px; background: linear-gradient(145deg, #e0e0e0, #f0f0f0); outline: none; border: 1px solid #000; }");
    httpd_resp_sendstr_chunk(req, "input[type='range']::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 24px; height: 24px; border-radius: 50%; background: linear-gradient(145deg, #ffffff, #f0f0f0); border: 2px solid #000; cursor: pointer; box-shadow: 2px 2px 4px rgba(0,0,0,0.2); }");
    httpd_resp_sendstr_chunk(req, "input[type='range']::-moz-range-thumb { width: 24px; height: 24px; border-radius: 50%; background: linear-gradient(145deg, #ffffff, #f0f0f0); border: 2px solid #000; cursor: pointer; }");
    
    httpd_resp_sendstr_chunk(req, "</style>");
    
    httpd_resp_sendstr_chunk(req, "</head><body>");
    
    // HTML Content
    httpd_resp_sendstr_chunk(req, "<div class='container'>");
    httpd_resp_sendstr_chunk(req, "<div class='header'>");
    httpd_resp_sendstr_chunk(req, "<h1 style='margin: 0 0 10px 0;'>🐕 Kiki Control</h1>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size: 0.9em; color: #666; font-style: italic; margin-bottom: 15px;'>by miniZ</div>");
    httpd_resp_sendstr_chunk(req, "<div class='status' id='status'>🟢 Sẵn Sàng Điều Khiển</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Navigation Tabs
    httpd_resp_sendstr_chunk(req, "<div class='nav-tabs'>");
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab active' onclick='showPage(1)' id='tab1'>🎮 Điều Khiển</div>");
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab' onclick='showPage(2)' id='tab2'>😊 Cảm Xúc & Cài Đặt</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Page 1: Main Controls
    httpd_resp_sendstr_chunk(req, "<div class='page active' id='page1'>");
    
    // Movement Controls
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🎮 Điều Khiển Di Chuyển</div>");
    httpd_resp_sendstr_chunk(req, "<div class='direction-pad'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-forward paw-btn' onclick='sendAction(\"dog_walk\", 3, 150)'>🐾 Tiến</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-left paw-btn' onclick='sendAction(\"dog_turn_left\", 2, 150)'>🐾 Trái</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-stop' onclick='sendAction(\"dog_stop\", 0, 0)'>🛑 DỪNG</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-right paw-btn' onclick='sendAction(\"dog_turn_right\", 2, 150)'>🐾 Phải</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-backward paw-btn' onclick='sendAction(\"dog_walk_back\", 3, 150)'>🐾 Lùi</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Auto Pose Toggle Section
    httpd_resp_sendstr_chunk(req, "<div class='auto-pose-section' style='margin-top: 15px; text-align: center;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn toggle-btn' id='autoPoseBtn' onclick='toggleAutoPose()'>🔄 Tự Đổi Tư Thế (1 phút)</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Fun Actions
    httpd_resp_sendstr_chunk(req, "<div class='fun-actions'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🎪 Hành Động Vui</div>");
    httpd_resp_sendstr_chunk(req, "<div class='action-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_dance\", 3, 200)'>💃 Nhảy Múa</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_jump\", 1, 200)'>🦘 Nhảy Cao</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_bow\", 1, 2000)'>🙇 Cúi Chào</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_sit_down\", 1, 500)'>🪑 Ngồi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_lie_down\", 1, 1000)'>🛏️ Nằm</button>");
    // New Defend and Scratch buttons  
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_defend\", 1, 500)'>� Giả Chết</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn paw-btn' onclick='sendAction(\"dog_scratch\", 5, 50)'>🐾 Gãi Ngứa</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_wave_right_foot\", 5, 50)'>👋 Vẫy Tay</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_wag_tail\", 5, 100)'>🐕 Vẫy Đuôi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_swing\", 5, 10)'>🎯 Lắc Lư</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_stretch\", 2, 15)'>🧘 Thư Giản</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_home\", 1, 500)'>🏠 Về Nhà</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_dance_4_feet\", 3, 200)'>🕺 Nhảy 4 Chân</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_greet\", 1, 500)'>👋 Chào Hỏi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_attack\", 1, 500)'>⚔️ Tấn Công</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_celebrate\", 1, 500)'>🎉 Ăn Mừng</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_search\", 1, 500)'>🔍 Tìm Kiếm</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // New Poses Section (reduced - removed tools with >32 limit)
    httpd_resp_sendstr_chunk(req, "<div class='fun-actions'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🎭 Tư Thế Mới</div>");
    httpd_resp_sendstr_chunk(req, "<div class='action-grid'>");
    // Comment out removed tools: shake_paw, sidestep (đã xóa để giảm xuống <32 tools)
    // httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_shake_paw\", 3, 150)'>🤝 Bắt Tay</button>");
    // httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_sidestep_right\", 3, 80)'>➡️ Đi Ngang Phải</button>");
    // httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_sidestep_left\", 3, 80)'>⬅️ Đi Ngang Trái</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_pushup\", 3, 150)'>💪 Chống Đẩy</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_balance\", 2000, 150)'>🚽 Đi Vệ Sinh</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // New Special Actions section (HIDDEN)
    httpd_resp_sendstr_chunk(req, "<div class='fun-actions' style='display:none;'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🎪 Hành Động Đặc Biệt</div>");
    httpd_resp_sendstr_chunk(req, "<div class='action-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_roll_over\", 1, 200)'>🔄 Lăn Qua Lăn Lại</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_play_dead\", 5, 0)'>💀 Giả Chết</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // ALL EMOJI Section on Page 1
    httpd_resp_sendstr_chunk(req, "<div class='emoji-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>😊 TẤT CẢ EMOJI</div>");
    httpd_resp_sendstr_chunk(req, "<div class='emoji-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"happy\")'>😊 Vui</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"sad\")'>😢 Buồn</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"angry\")'>😠 Giận</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"surprised\")'>😮 Ngạc Nhiên</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"love\")'>😍 Yêu</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"sleepy\")'>😴 Buồn Ngủ</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"confused\")'>😕 Bối Rối</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"excited\")'>🤩 Phấn Khích</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"neutral\")'>😐 Bình Thường</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"thinking\")'>🤔 Suy Nghĩ</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"wink\")'>😉 Nháy Mắt</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"cool\")'>😎 Ngầu</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"laughing\")'>😂 Cười To</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"crying\")'>😭 Khóc</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"crazy\")'>🤪 Điên</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"shocked\")'>😱 Sốc</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"winking\")'>😜 Nháy Mắt Lém</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // AI Chat Section - MOVED TO PAGE 1
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>💬 Chat với AI</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #f0f4ff, #ffffff); border: 2px solid #1976d2; border-radius: 15px; padding: 20px; margin-bottom: 20px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "💬 Nhập văn bản để Otto nói chuyện với AI qua WebSocket!");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<textarea id='aiTextInput' placeholder='Nhập nội dung muốn gửi cho AI...' style='width: 100%; min-height: 100px; padding: 12px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px; font-family: inherit; resize: vertical;'></textarea>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendTextToAI()' style='margin-top: 10px; background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-weight: bold; padding: 12px 20px; width: 100%;'>📤 Gửi cho AI</button>");
    httpd_resp_sendstr_chunk(req, "<div id='aiChatStatus' style='margin-top: 10px; font-size: 14px; color: #666;'></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Response area for Page 1
    httpd_resp_sendstr_chunk(req, "<div class='response' id='response'>Ready for commands...</div>");
    httpd_resp_sendstr_chunk(req, "</div>"); // End Page 1
    
    // Page 2: Settings & Configuration
    httpd_resp_sendstr_chunk(req, "<div class='page' id='page2'>");

    // Volume Control Section
    httpd_resp_sendstr_chunk(req, "<div class='volume-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🔊 Điều Chỉnh Âm Lượng</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #f8f8f8, #ffffff); border: 2px solid #000000; border-radius: 15px; padding: 20px; margin-bottom: 20px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 15px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000; min-width: 80px;'>🔈 Âm lượng:</span>");
    httpd_resp_sendstr_chunk(req, "<input type='range' id='volumeSlider' min='0' max='100' value='50' style='flex: 1; min-width: 200px; height: 8px; background: linear-gradient(145deg, #e0e0e0, #f0f0f0); border-radius: 5px; outline: none; -webkit-appearance: none;'>");
    httpd_resp_sendstr_chunk(req, "<span id='volumeValue' style='font-weight: bold; color: #000; min-width: 50px;'>50%</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Touch Sensor Control Section - HIDDEN
    // httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    // httpd_resp_sendstr_chunk(req, "<div class='section-title'>🖐️ Cảm Biến Chạm TTP223</div>");
    // httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    // httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setTouchSensor(true)' id='touch-on' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 16px; font-weight: bold;'>🖐️ BẬT Cảm Biến Chạm</button>");
    // httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setTouchSensor(false)' id='touch-off' style='background: linear-gradient(145deg, #f44336, #e57373); color: white; border-color: #c62828; font-size: 16px; font-weight: bold;'>🚫 TẮT Cảm Biến Chạm</button>");
    // httpd_resp_sendstr_chunk(req, "</div>");
    // httpd_resp_sendstr_chunk(req, "<div style='text-align: center; margin-top: 10px; color: #666; font-size: 14px;'>");
    // httpd_resp_sendstr_chunk(req, "Khi BẬT: chạm vào cảm biến → robot nhảy + emoji cười<br>");
    // httpd_resp_sendstr_chunk(req, "Khi TẮT: chạm vào cảm biến không có phản ứng");
    // httpd_resp_sendstr_chunk(req, "</div>");
    // httpd_resp_sendstr_chunk(req, "</div>");
    
    // System Controls Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>⚙️ Điều Khiển Hệ Thống</div>");
    httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='powerSaveBtn' onclick='toggleScreen()' style='background: linear-gradient(145deg, #9e9e9e, #bdbdbd); color: white; border-color: #616161; font-size: 16px; font-weight: bold;'>📱 Tiết Kiệm: TẮT</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='micBtn' onclick='toggleMic()' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 16px; font-weight: bold;'>🎤 Mic: TẮT</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='forgetWiFi()' style='background: linear-gradient(145deg, #ff5722, #ff7043); color: white; border-color: #d84315; font-size: 16px; font-weight: bold;'>🔄 Quên WiFi & Tạo AP</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='text-align: center; margin-top: 10px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "<strong>Tiết Kiệm Năng Lượng:</strong> TẮT = bình thường, BẬT = giảm tiêu thụ WiFi<br>");
    httpd_resp_sendstr_chunk(req, "<strong>Mic:</strong> TẮT/BẬT microphone để lắng nghe giọng nói<br>");
    httpd_resp_sendstr_chunk(req, "<strong>Quên WiFi & Tạo AP:</strong> xóa WiFi hiện tại, robot sẽ tạo Access Point để cấu hình WiFi mới");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Auto Pose Advanced Configuration
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🔄 Cấu Hình Auto Pose</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-config'>");
    
    // Time interval setting
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 12px; background: #e3f2fd; border: 2px solid #2196f3; border-radius: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 8px; color: #000;'>⏱️ Thời gian giữa các tư thế (giây):</label>");
    httpd_resp_sendstr_chunk(req, "<input type='number' id='poseInterval' class='time-input' value='60' min='5' max='300' style='width: 100px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='updateInterval()' style='margin-left: 10px; padding: 8px 16px;'>✓ Áp Dụng</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Pose selection checkboxes
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #000;'>✅ Chọn các tư thế để Auto:</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_sit' checked><label for='pose_sit'>🪑 Ngồi (Sit Down)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_jump' checked><label for='pose_jump'>🦘 Nhảy (Jump)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_wave' checked><label for='pose_wave'>👋 Vẫy Tay (Wave)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_bow' checked><label for='pose_bow'>🙇 Cúi Chào (Bow)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_stretch' checked><label for='pose_stretch'>🧘 Thư Giản (Stretch)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_swing' checked><label for='pose_swing'>🎯 Lắc Lư (Swing)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_dance' checked><label for='pose_dance'>💃 Nhảy Múa (Dance)</label></div>");
    
    httpd_resp_sendstr_chunk(req, "<button class='btn toggle-btn' id='autoPoseBtn2' onclick='toggleAutoPose()' style='width: 100%; margin-top: 15px; font-size: 16px;'>🔄 Bật/Tắt Auto Pose</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Auto Emoji Advanced Configuration
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>😊 Cấu Hình Auto Emoji</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-config'>");
    
    // Time interval setting for emoji
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 12px; background: #fff3e0; border: 2px solid #ff9800; border-radius: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 8px; color: #000;'>⏱️ Thời gian giữa các emoji (giây):</label>");
    httpd_resp_sendstr_chunk(req, "<input type='number' id='emojiInterval' class='time-input' value='10' min='3' max='120' style='width: 100px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='updateEmojiInterval()' style='margin-left: 10px; padding: 8px 16px;'>✓ Áp Dụng</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Emoji selection checkboxes
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #000;'>✅ Chọn các emoji để Auto:</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_happy' checked><label for='emoji_happy'>😊 Vui (Happy)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_laughing' checked><label for='emoji_laughing'>😂 Cười To (Laughing)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_winking' checked><label for='emoji_winking'>😜 Nháy Mắt (Winking)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_cool' checked><label for='emoji_cool'>😎 Ngầu (Cool)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_love' checked><label for='emoji_love'>😍 Yêu (Love)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_surprised' checked><label for='emoji_surprised'>😮 Ngạc Nhiên (Surprised)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_excited' checked><label for='emoji_excited'>🤩 Phấn Khích (Excited)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_sleepy' checked><label for='emoji_sleepy'>😴 Buồn Ngủ (Sleepy)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_sad' checked><label for='emoji_sad'>😢 Buồn (Sad)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_angry' checked><label for='emoji_angry'>😠 Giận (Angry)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_confused' checked><label for='emoji_confused'>😕 Bối Rối (Confused)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_thinking' checked><label for='emoji_thinking'>🤔 Suy Nghĩ (Thinking)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_neutral' checked><label for='emoji_neutral'>😐 Bình Thường (Neutral)</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_shocked' checked><label for='emoji_shocked'>😱 Sốc (Shocked)</label></div>");
    
    httpd_resp_sendstr_chunk(req, "<button class='btn toggle-btn' id='autoEmojiBtn' onclick='toggleAutoEmoji()' style='width: 100%; margin-top: 15px; font-size: 16px; background: linear-gradient(145deg, #ff9800, #ffa726);'>😊 Bật/Tắt Auto Emoji</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Emoji Mode Selector Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🎨 Chế Độ Hiển Thị Emoji</div>");
    httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='otto-mode' onclick='setEmojiMode(true)' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: 3px solid #2e7d32; font-size: 18px; font-weight: bold; box-shadow: 0 4px 8px rgba(0,0,0,0.2);'>🤖 OTTO GIF MODE (ACTIVE)</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='default-mode' onclick='setEmojiMode(false)' style='font-size: 16px; font-weight: bold;'>😊 Twemoji Text Mode</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='text-align: center; margin-top: 10px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "<strong>🤖 OTTO GIF:</strong> Hiển thị emoji động GIF (Otto robot)<br>");
    httpd_resp_sendstr_chunk(req, "<strong>😊 Twemoji:</strong> Hiển thị emoji văn bản chuẩn Unicode");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");

    // Gemini API Key Configuration Section - REMOVED (MQTT/config section removed)
    // httpd_resp_sendstr_chunk(req, "<div class='movement-section' style='display:none;'>");
    // httpd_resp_sendstr_chunk(req, "<div class='section-title'>🤖 Cấu Hình Gemini AI</div>");
    // httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #f8f8f8, #ffffff); border: 2px solid #000000; border-radius: 15px; padding: 20px; margin-bottom: 20px;'>");
    // httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; color: #666; font-size: 14px;'>");
    // httpd_resp_sendstr_chunk(req, "⭐ Nhập Google Gemini API Key để Otto trở nên thông minh hơn!<br>");
    // httpd_resp_sendstr_chunk(req, "🔑 Lấy key miễn phí tại: <a href='https://aistudio.google.com/apikey' target='_blank' style='color: #1976d2;'>Google AI Studio</a>");
    // httpd_resp_sendstr_chunk(req, "</div>");
    // httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 10px; flex-wrap: wrap; align-items: center;'>");
    // httpd_resp_sendstr_chunk(req, "<input type='text' id='geminiApiKey' placeholder='Nhập Gemini API Key...' style='flex: 1; min-width: 250px; padding: 12px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'>");
    // httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='saveGeminiKey()' style='background: linear-gradient(145deg, #4285f4, #5a95f5); color: white; border-color: #1976d2; font-weight: bold; padding: 12px 20px;'>💾 Lưu Key</button>");
    // httpd_resp_sendstr_chunk(req, "</div>");
    // httpd_resp_sendstr_chunk(req, "<div id='geminiKeyStatus' style='margin-top: 10px; font-size: 14px;'></div>");
    // httpd_resp_sendstr_chunk(req, "</div>");
    // httpd_resp_sendstr_chunk(req, "</div>");

    // Response area for Page 2
    httpd_resp_sendstr_chunk(req, "<div class='response' id='response2'>Cấu hình sẵn sàng...</div>");
    httpd_resp_sendstr_chunk(req, "</div>"); // End Page 2
    
    httpd_resp_sendstr_chunk(req, "</div>"); // End container
    
    // JavaScript - Simple and clean
    httpd_resp_sendstr_chunk(req, "<script>");
    // Page navigation
    httpd_resp_sendstr_chunk(req, "function showPage(pageNum) {");
    httpd_resp_sendstr_chunk(req, "  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));");
    httpd_resp_sendstr_chunk(req, "  document.querySelectorAll('.nav-tab').forEach(t => t.classList.remove('active'));");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('page' + pageNum).classList.add('active');");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('tab' + pageNum).classList.add('active');");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function sendAction(action, param1, param2) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Action:', action);");
    httpd_resp_sendstr_chunk(req, "  var url = '/action?cmd=' + action + '&p1=' + param1 + '&p2=' + param2;");
    httpd_resp_sendstr_chunk(req, "  fetch(url).then(r => r.text()).then(d => console.log('Success:', d));");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function sendEmotion(emotion) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Emotion:', emotion);");
    httpd_resp_sendstr_chunk(req, "  fetch('/emotion?emotion=' + emotion)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Success:', d);");
    httpd_resp_sendstr_chunk(req, "      var responseEl = document.getElementById('response');");
    httpd_resp_sendstr_chunk(req, "      if (responseEl) responseEl.textContent = 'Emotion: ' + emotion + ' - ' + d;");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => {");
    httpd_resp_sendstr_chunk(req, "      console.error('Error:', e);");
    httpd_resp_sendstr_chunk(req, "      var responseEl = document.getElementById('response');");
    httpd_resp_sendstr_chunk(req, "      if (responseEl) responseEl.textContent = 'Error setting emotion: ' + e;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function setEmojiMode(useOttoEmoji) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Setting emoji mode:', useOttoEmoji ? 'OTTO GIF' : 'Twemoji Text');");
    // For compatibility, send 'gif' when Otto mode is selected (server also accepts 'otto')
    httpd_resp_sendstr_chunk(req, "  var mode = useOttoEmoji ? 'gif' : 'default';");
    httpd_resp_sendstr_chunk(req, "  fetch('/emoji_mode?mode=' + mode)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Mode response:', d);");
    // Update button styles
    httpd_resp_sendstr_chunk(req, "      var ottoBtn = document.getElementById('otto-mode');");
    httpd_resp_sendstr_chunk(req, "      var defaultBtn = document.getElementById('default-mode');");
    httpd_resp_sendstr_chunk(req, "      var responseEl = document.getElementById('response2');");
    httpd_resp_sendstr_chunk(req, "      if (useOttoEmoji) {");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.classList.add('active');");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.style.cssText = 'background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: 3px solid #2e7d32; font-size: 18px; font-weight: bold; box-shadow: 0 4px 8px rgba(0,0,0,0.2);';");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.innerHTML = '🤖 OTTO GIF MODE (ACTIVE)';");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.classList.remove('active');");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.style.cssText = 'font-size: 16px; font-weight: bold;';");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.innerHTML = '😊 Twemoji Text Mode';");
    httpd_resp_sendstr_chunk(req, "        if (responseEl) responseEl.textContent = '✅ Đã chuyển sang OTTO GIF MODE';");
    httpd_resp_sendstr_chunk(req, "      } else {");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.classList.add('active');");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.style.cssText = 'background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: 3px solid #2e7d32; font-size: 18px; font-weight: bold; box-shadow: 0 4px 8px rgba(0,0,0,0.2);';");
    httpd_resp_sendstr_chunk(req, "        defaultBtn.innerHTML = '😊 TWEMOJI TEXT MODE (ACTIVE)';");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.classList.remove('active');");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.style.cssText = 'font-size: 16px; font-weight: bold;';");
    httpd_resp_sendstr_chunk(req, "        ottoBtn.innerHTML = '🤖 OTTO GIF Mode';");
    httpd_resp_sendstr_chunk(req, "        if (responseEl) responseEl.textContent = '✅ Đã chuyển sang TWEMOJI TEXT MODE';");
    httpd_resp_sendstr_chunk(req, "      }");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => {");
    httpd_resp_sendstr_chunk(req, "      console.error('Error setting emoji mode:', e);");
    httpd_resp_sendstr_chunk(req, "      var responseEl = document.getElementById('response2');");
    httpd_resp_sendstr_chunk(req, "      if (responseEl) responseEl.textContent = '❌ Lỗi chuyển đổi chế độ: ' + e;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "}");
    // Touch sensor function - HIDDEN
    // httpd_resp_sendstr_chunk(req, "function setTouchSensor(enabled) {");
    // httpd_resp_sendstr_chunk(req, "  console.log('Touch sensor:', enabled);");
    // httpd_resp_sendstr_chunk(req, "  fetch('/touch_sensor?enabled=' + enabled).then(r => r.text()).then(d => {");
    // httpd_resp_sendstr_chunk(req, "    console.log('Touch sensor result:', d);");
    // httpd_resp_sendstr_chunk(req, "    document.getElementById('response').innerHTML = d;");
    // httpd_resp_sendstr_chunk(req, "  });");
    // httpd_resp_sendstr_chunk(req, "}");
    
    // Screen toggle JavaScript with state tracking
    httpd_resp_sendstr_chunk(req, "let powerSaveState = false;"); // Track state
    httpd_resp_sendstr_chunk(req, "function toggleScreen() {");
    httpd_resp_sendstr_chunk(req, "  console.log('Toggling screen...');");
    httpd_resp_sendstr_chunk(req, "  const btn = document.getElementById('powerSaveBtn');");
    httpd_resp_sendstr_chunk(req, "  fetch('/screen_toggle').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Screen toggle result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    powerSaveState = !powerSaveState;"); // Toggle state
    httpd_resp_sendstr_chunk(req, "    if (powerSaveState) {"); // ON - blue
    httpd_resp_sendstr_chunk(req, "      btn.style.background = 'linear-gradient(145deg, #2196f3, #42a5f5)';");
    httpd_resp_sendstr_chunk(req, "      btn.style.borderColor = '#1565c0';");
    httpd_resp_sendstr_chunk(req, "      btn.innerHTML = '📱 Tiết Kiệm: <strong>BẬT</strong>';");
    httpd_resp_sendstr_chunk(req, "    } else {"); // OFF - grey
    httpd_resp_sendstr_chunk(req, "      btn.style.background = 'linear-gradient(145deg, #9e9e9e, #bdbdbd)';");
    httpd_resp_sendstr_chunk(req, "      btn.style.borderColor = '#616161';");
    httpd_resp_sendstr_chunk(req, "      btn.innerHTML = '📱 Tiết Kiệm: <strong>TẮT</strong>';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Toggle microphone JavaScript - with state tracking
    httpd_resp_sendstr_chunk(req, "let micActive = false;");
    httpd_resp_sendstr_chunk(req, "function toggleMic() {");
    httpd_resp_sendstr_chunk(req, "  const micBtn = document.getElementById('micBtn');");
    httpd_resp_sendstr_chunk(req, "  if (micActive) {");
    httpd_resp_sendstr_chunk(req, "    console.log('Stopping microphone...');");
    httpd_resp_sendstr_chunk(req, "    fetch('/wake_mic?action=stop').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Mic stopped:', d);");
    httpd_resp_sendstr_chunk(req, "      micActive = false;");
    httpd_resp_sendstr_chunk(req, "      micBtn.innerHTML = '🎤 Mic: TẮT';");
    httpd_resp_sendstr_chunk(req, "      micBtn.style.background = 'linear-gradient(145deg, #9e9e9e, #bdbdbd)';");
    httpd_resp_sendstr_chunk(req, "      micBtn.style.borderColor = '#616161';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    console.log('Starting microphone...');");
    httpd_resp_sendstr_chunk(req, "    fetch('/wake_mic').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Mic started:', d);");
    httpd_resp_sendstr_chunk(req, "      micActive = true;");
    httpd_resp_sendstr_chunk(req, "      micBtn.innerHTML = '🎤 Mic: BẬT';");
    httpd_resp_sendstr_chunk(req, "      micBtn.style.background = 'linear-gradient(145deg, #4caf50, #66bb6a)';");
    httpd_resp_sendstr_chunk(req, "      micBtn.style.borderColor = '#2e7d32';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Forget WiFi JavaScript
    httpd_resp_sendstr_chunk(req, "function forgetWiFi() {");
    httpd_resp_sendstr_chunk(req, "  if (confirm('Quên WiFi hiện tại và tạo Access Point?\\n\\nRobot sẽ khởi động lại và tạo AP để bạn có thể:\\n1. Kết nối vào AP của robot\\n2. Cấu hình WiFi mới qua trình duyệt\\n\\nBạn có chắc không?')) {");
    httpd_resp_sendstr_chunk(req, "    console.log('Forgetting WiFi and entering AP mode...');");
    httpd_resp_sendstr_chunk(req, "    fetch('/forget_wifi').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Forget WiFi result:', d);");
    httpd_resp_sendstr_chunk(req, "      alert('WiFi đã được quên!\\nRobot sẽ khởi động lại và tạo Access Point.\\nHãy kết nối vào AP của robot để cấu hình WiFi mới.');");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Volume control JavaScript
    httpd_resp_sendstr_chunk(req, "function setVolume(volume) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Setting volume:', volume);");
    httpd_resp_sendstr_chunk(req, "  fetch('/volume?level=' + volume).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Volume result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response').innerHTML = 'Âm lượng: ' + volume + '%';");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Auto pose toggle JavaScript with pose selection
    httpd_resp_sendstr_chunk(req, "var autoPoseEnabled = false;");
    httpd_resp_sendstr_chunk(req, "var selectedPoses = ['sit','jump'  ,'wave','bow','stretch','swing','dance'];"); // Default all enabled
    httpd_resp_sendstr_chunk(req, "function toggleAutoPose() {");
    httpd_resp_sendstr_chunk(req, "  autoPoseEnabled = !autoPoseEnabled;");
    httpd_resp_sendstr_chunk(req, "  var btn = document.getElementById('autoPoseBtn');");
    httpd_resp_sendstr_chunk(req, "  var btn2 = document.getElementById('autoPoseBtn2');");
    httpd_resp_sendstr_chunk(req, "  if (autoPoseEnabled) {");
    httpd_resp_sendstr_chunk(req, "    if(btn) { btn.classList.add('active'); btn.style.background = '#4caf50'; btn.style.color = 'white'; }");
    httpd_resp_sendstr_chunk(req, "    if(btn2) { btn2.classList.add('active'); btn2.style.background = '#4caf50'; btn2.style.color = 'white'; }");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response').innerHTML = '✅ Tự động đổi tư thế BẬT';");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '✅ Tự động đổi tư thế BẬT';");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    if(btn) { btn.classList.remove('active'); btn.style.background = ''; btn.style.color = ''; }");
    httpd_resp_sendstr_chunk(req, "    if(btn2) { btn2.classList.remove('active'); btn2.style.background = ''; btn2.style.color = ''; }");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response').innerHTML = '⛔ Tự động đổi tư thế TẮT';");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '⛔ Tự động đổi tư thế TẮT';");
    httpd_resp_sendstr_chunk(req, "  }");
    // Get selected poses
    httpd_resp_sendstr_chunk(req, "  updateSelectedPoses();");
    httpd_resp_sendstr_chunk(req, "  var posesParam = selectedPoses.join(',');");
    httpd_resp_sendstr_chunk(req, "  fetch('/auto_pose?enabled=' + (autoPoseEnabled ? 'true' : 'false') + '&poses=' + posesParam).then(r => r.text()).then(d => console.log('Auto pose:', d));");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Update interval function
    httpd_resp_sendstr_chunk(req, "function updateInterval() {");
    httpd_resp_sendstr_chunk(req, "  var interval = document.getElementById('poseInterval').value;");
    httpd_resp_sendstr_chunk(req, "  fetch('/auto_pose_interval?seconds=' + interval).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '⏱️ Đã đặt thời gian: ' + interval + ' giây';");
    httpd_resp_sendstr_chunk(req, "    console.log('Interval updated:', d);");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Update selected poses
    httpd_resp_sendstr_chunk(req, "function updateSelectedPoses() {");
    httpd_resp_sendstr_chunk(req, "  selectedPoses = [];");
    httpd_resp_sendstr_chunk(req, "  ['sit','jump','wave','bow','stretch','swing','dance'].forEach(p => {");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('pose_' + p) && document.getElementById('pose_' + p).checked) selectedPoses.push(p);");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Auto emoji toggle JavaScript with emoji selection
    httpd_resp_sendstr_chunk(req, "var autoEmojiEnabled = false;");
    httpd_resp_sendstr_chunk(req, "var selectedEmojis = ['happy','laughing','winking','cool','love','surprised','excited','sleepy','sad','angry','confused','thinking','neutral','shocked'];"); // Default all enabled
    httpd_resp_sendstr_chunk(req, "function toggleAutoEmoji() {");
    httpd_resp_sendstr_chunk(req, "  autoEmojiEnabled = !autoEmojiEnabled;");
    httpd_resp_sendstr_chunk(req, "  var btn = document.getElementById('autoEmojiBtn');");
    httpd_resp_sendstr_chunk(req, "  if (autoEmojiEnabled) {");
    httpd_resp_sendstr_chunk(req, "    if(btn) { btn.classList.add('active'); btn.style.background = '#ff9800'; btn.style.color = 'white'; }");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '✅ Tự động đổi emoji BẬT';");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    if(btn) { btn.classList.remove('active'); btn.style.background = ''; btn.style.color = ''; }");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '⛔ Tự động đổi emoji TẮT';");
    httpd_resp_sendstr_chunk(req, "  }");
    // Get selected emojis
    httpd_resp_sendstr_chunk(req, "  updateSelectedEmojis();");
    httpd_resp_sendstr_chunk(req, "  var emojisParam = selectedEmojis.join(',');");
    httpd_resp_sendstr_chunk(req, "  fetch('/auto_emoji?enabled=' + (autoEmojiEnabled ? 'true' : 'false') + '&emojis=' + emojisParam).then(r => r.text()).then(d => console.log('Auto emoji:', d));");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Update emoji interval function
    httpd_resp_sendstr_chunk(req, "function updateEmojiInterval() {");
    httpd_resp_sendstr_chunk(req, "  var interval = document.getElementById('emojiInterval').value;");
    httpd_resp_sendstr_chunk(req, "  fetch('/auto_emoji_interval?seconds=' + interval).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '⏱️ Đã đặt thời gian emoji: ' + interval + ' giây';");
    httpd_resp_sendstr_chunk(req, "    console.log('Emoji interval updated:', d);");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Update selected emojis
    httpd_resp_sendstr_chunk(req, "function updateSelectedEmojis() {");
    httpd_resp_sendstr_chunk(req, "  selectedEmojis = [];");
    httpd_resp_sendstr_chunk(req, "  ['happy','laughing','winking','cool','love','surprised','excited','sleepy','sad','angry','confused','thinking','neutral','shocked'].forEach(e => {");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('emoji_' + e) && document.getElementById('emoji_' + e).checked) selectedEmojis.push(e);");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Gemini API Key functions - REMOVED (MQTT/config section removed)
    // httpd_resp_sendstr_chunk(req, "function saveGeminiKey() {");
    // httpd_resp_sendstr_chunk(req, "  var apiKey = document.getElementById('geminiApiKey').value;");
    // httpd_resp_sendstr_chunk(req, "  if (!apiKey || apiKey.trim() === '') {");
    // httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').innerHTML = '❌ Vui lòng nhập API key!';");
    // httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    // httpd_resp_sendstr_chunk(req, "    return;");
    // httpd_resp_sendstr_chunk(req, "  }");
    // httpd_resp_sendstr_chunk(req, "  document.getElementById('geminiKeyStatus').innerHTML = '⏳ Đang lưu...';");
    // httpd_resp_sendstr_chunk(req, "  document.getElementById('geminiKeyStatus').style.color = '#666';");
    // httpd_resp_sendstr_chunk(req, "  fetch('/gemini_api_key', {");
    // httpd_resp_sendstr_chunk(req, "    method: 'POST',");
    // httpd_resp_sendstr_chunk(req, "    headers: {'Content-Type': 'application/json'},");
    // httpd_resp_sendstr_chunk(req, "    body: JSON.stringify({api_key: apiKey})");
    // httpd_resp_sendstr_chunk(req, "  }).then(r => r.json()).then(data => {");
    // httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '✅ API key đã được lưu thành công!';");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#4caf50';");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiApiKey').value = '';");
    // httpd_resp_sendstr_chunk(req, "      loadGeminiKeyStatus();");
    // httpd_resp_sendstr_chunk(req, "    } else {");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '❌ Lỗi: ' + data.error;");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    // httpd_resp_sendstr_chunk(req, "    }");
    // httpd_resp_sendstr_chunk(req, "  }).catch(e => {");
    // httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').innerHTML = '❌ Lỗi kết nối: ' + e;");
    // httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    // httpd_resp_sendstr_chunk(req, "  });");
    // httpd_resp_sendstr_chunk(req, "}");
    // httpd_resp_sendstr_chunk(req, "function loadGeminiKeyStatus() {");
    // httpd_resp_sendstr_chunk(req, "  fetch('/gemini_api_key').then(r => r.json()).then(data => {");
    // httpd_resp_sendstr_chunk(req, "    if (data.configured) {");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '✅ API key đã cấu hình: ' + data.key_preview;");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#4caf50';");
    // httpd_resp_sendstr_chunk(req, "    } else {");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '⚠️ Chưa có API key. Nhập key để kích hoạt Gemini AI.';");
    // httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#ff9800';");
    // httpd_resp_sendstr_chunk(req, "    }");
    // httpd_resp_sendstr_chunk(req, "  });");
    // httpd_resp_sendstr_chunk(req, "}");
    
    // AI text chat function
    httpd_resp_sendstr_chunk(req, "function sendTextToAI() {");
    httpd_resp_sendstr_chunk(req, "  const textInput = document.getElementById('aiTextInput');");
    httpd_resp_sendstr_chunk(req, "  const statusDiv = document.getElementById('aiChatStatus');");
    httpd_resp_sendstr_chunk(req, "  const text = textInput.value.trim();");
    httpd_resp_sendstr_chunk(req, "  if (!text) {");
    httpd_resp_sendstr_chunk(req, "    statusDiv.innerHTML = '❌ Vui lòng nhập nội dung!';");
    httpd_resp_sendstr_chunk(req, "    statusDiv.style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  if (text.length > 1500) {");
    httpd_resp_sendstr_chunk(req, "    statusDiv.innerHTML = '❌ Văn bản quá dài! Tối đa 1500 ký tự.';");
    httpd_resp_sendstr_chunk(req, "    statusDiv.style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  statusDiv.innerHTML = '⏳ Đang gửi...';");
    httpd_resp_sendstr_chunk(req, "  statusDiv.style.color = '#666';");
    httpd_resp_sendstr_chunk(req, "  fetch('/api/ai/send', {");
    httpd_resp_sendstr_chunk(req, "    method: 'POST',");
    httpd_resp_sendstr_chunk(req, "    headers: {'Content-Type': 'application/json'},");
    httpd_resp_sendstr_chunk(req, "    body: JSON.stringify({text: text})");
    httpd_resp_sendstr_chunk(req, "  }).then(r => r.json()).then(data => {");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      statusDiv.innerHTML = '✅ Đã gửi thành công! Otto đang xử lý...';");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.color = '#4caf50';");
    httpd_resp_sendstr_chunk(req, "      textInput.value = '';");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      statusDiv.innerHTML = '❌ Lỗi: ' + data.message;");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  }).catch(e => {");
    httpd_resp_sendstr_chunk(req, "    statusDiv.innerHTML = '❌ Lỗi kết nối: ' + e;");
    httpd_resp_sendstr_chunk(req, "    statusDiv.style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "document.getElementById('aiTextInput').addEventListener('keypress', function(e) {");
    httpd_resp_sendstr_chunk(req, "  if (e.key === 'Enter' && !e.shiftKey) {");
    httpd_resp_sendstr_chunk(req, "    e.preventDefault();");
    httpd_resp_sendstr_chunk(req, "    sendTextToAI();");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "});");
    
    // Initialize volume slider
    httpd_resp_sendstr_chunk(req, "window.onload = function() {");
    // httpd_resp_sendstr_chunk(req, "  loadGeminiKeyStatus();"); // Removed - Gemini API Key config removed
    httpd_resp_sendstr_chunk(req, "  var slider = document.getElementById('volumeSlider');");
    httpd_resp_sendstr_chunk(req, "  var output = document.getElementById('volumeValue');");
    httpd_resp_sendstr_chunk(req, "  slider.oninput = function() {");
    httpd_resp_sendstr_chunk(req, "    output.innerHTML = this.value + '%';");
    httpd_resp_sendstr_chunk(req, "    setVolume(this.value);");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "};");
    
    // Initialize Twemoji for emoji rendering after page load
    httpd_resp_sendstr_chunk(req, "// Initialize Twemoji for emoji rendering");
    httpd_resp_sendstr_chunk(req, "if (typeof twemoji !== 'undefined') {");
    httpd_resp_sendstr_chunk(req, "  twemoji.parse(document.body, {");
    httpd_resp_sendstr_chunk(req, "    folder: 'svg',");
    httpd_resp_sendstr_chunk(req, "    ext: '.svg'");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "  console.log('Twemoji initialized');");
    httpd_resp_sendstr_chunk(req, "} else {");
    httpd_resp_sendstr_chunk(req, "  console.warn('Twemoji library not loaded');");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "</script>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    
    httpd_resp_sendstr_chunk(req, NULL); // End of chunks
}

// Root page handler
esp_err_t otto_root_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Root page requested");
    send_otto_control_page(req);
    return ESP_OK;
}

} // extern "C"

// C++ function to execute Otto actions (with real controller integration)
void otto_execute_web_action(const char* action, int param1, int param2) {
    ESP_LOGI(TAG, "🎮 Web Control: %s (param1:%d, param2:%d)", action, param1, param2);
    
    // Map web actions to controller actions (order matters - check specific first)
    esp_err_t ret = ESP_OK;
    
    if (strstr(action, "walk_back")) {
        ret = otto_controller_queue_action(ACTION_DOG_WALK_BACK, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Walking backward: %d steps, speed %d", param1, param2);
    } else if (strstr(action, "walk_forward") || strstr(action, "walk")) {
        ret = otto_controller_queue_action(ACTION_DOG_WALK, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Walking forward: %d steps, speed %d", param1, param2);
    } else if (strstr(action, "turn_left") || (strstr(action, "turn") && param1 < 0)) {
        ret = otto_controller_queue_action(ACTION_DOG_TURN_LEFT, abs(param1), param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Turning left: %d steps, speed %d", abs(param1), param2);
    } else if (strstr(action, "turn_right") || (strstr(action, "turn") && param1 > 0)) {
        ret = otto_controller_queue_action(ACTION_DOG_TURN_RIGHT, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Turning right: %d steps, speed %d", param1, param2);
    } else if (strstr(action, "turn")) {
        // Default turn right if no direction specified
        ret = otto_controller_queue_action(ACTION_DOG_TURN_RIGHT, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Turning right (default): %d steps, speed %d", param1, param2);
    } else if (strstr(action, "sit")) {
        ret = otto_controller_queue_action(ACTION_DOG_SIT_DOWN, 1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Sitting down with delay %d", param2);
    } else if (strstr(action, "lie")) {
        ret = otto_controller_queue_action(ACTION_DOG_LIE_DOWN, 1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Lying down with delay %d", param2);
    } else if (strstr(action, "bow")) {
        ret = otto_controller_queue_action(ACTION_DOG_BOW, 1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Bowing with delay %d", param2);
    } else if (strstr(action, "jump")) {
        // Angry emoji when jumping
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("angry");
        ret = otto_controller_queue_action(ACTION_DOG_JUMP, 1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Jumping with delay %d", param2);
    } else if (strstr(action, "dance")) {
        // Happy emoji when dancing
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        ret = otto_controller_queue_action(ACTION_DOG_DANCE, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Dancing: %d cycles, speed %d", param1, param2);
    } else if (strstr(action, "wave")) {
        ret = otto_controller_queue_action(ACTION_DOG_WAVE_RIGHT_FOOT, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Waving: %d times, speed %d", param1, param2);
    } else if (strstr(action, "swing")) {
        // Happy emoji when swinging
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        ret = otto_controller_queue_action(ACTION_DOG_SWING, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Swinging: %d cycles, speed %d", param1, param2);
    } else if (strstr(action, "stretch")) {
        // Sleepy emoji during stretch
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("sleepy");
        ret = otto_controller_queue_action(ACTION_DOG_STRETCH, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Stretching: %d cycles, speed %d", param1, param2);
    } else if (strstr(action, "scratch")) {
        ret = otto_controller_queue_action(ACTION_DOG_SCRATCH, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Scratching: %d times, speed %d", param1, param2);
    } else if (strstr(action, "wag_tail")) {
        // Happy emoji when wagging tail
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        ret = otto_controller_queue_action(ACTION_DOG_WAG_TAIL, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🐕 Wagging tail: %d wags, speed %d", param1, param2);
    } else if (strstr(action, "defend")) {
        // Shocked emoji when defending
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("shocked");
        // Defend sequence: walk back EXACTLY 1 journey -> sit (3000) -> lie (1500) -> delay(3000) -> home
        otto_controller_queue_action(ACTION_DOG_WALK_BACK, 1, 100, 0, 0);  // Changed: speed=100 for full 1 journey
        otto_controller_queue_action(ACTION_DOG_SIT_DOWN, 1, 3000, 0, 0);
        otto_controller_queue_action(ACTION_DOG_LIE_DOWN, 1, 1500, 0, 0);
        otto_controller_queue_action(ACTION_DELAY, 0, 3000, 0, 0);
        otto_controller_queue_action(ACTION_HOME, 1, 500, 0, 0);
        ret = ESP_OK;
        ESP_LOGI(TAG, "🛡️ Defend sequence queued: walk_back(1,100) -> sit(3000) -> lie_down(1500) -> delay(3000) -> home");
    } else if (strstr(action, "home")) {
        ret = otto_controller_queue_action(ACTION_HOME, 1, 500, 0, 0);
        ESP_LOGI(TAG, "🏠 Going to home position");
    } else if (strstr(action, "dance_4_feet")) {
        // Happy emoji when dancing with 4 feet
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        ret = otto_controller_queue_action(ACTION_DOG_DANCE_4_FEET, param1, param2, 0, 0);
        ESP_LOGI(TAG, "🕺 Dancing with 4 feet: %d cycles, speed %d", param1, param2);
    } else if (strstr(action, "greet")) {
        // Happy emoji when greeting
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        // Greet sequence: home → wave → bow
        otto_controller_queue_action(ACTION_HOME, 1, 500, 0, 0);
        otto_controller_queue_action(ACTION_DOG_WAVE_RIGHT_FOOT, 3, 150, 0, 0);
        otto_controller_queue_action(ACTION_DOG_BOW, 2, 150, 0, 0);
        ret = ESP_OK;
        ESP_LOGI(TAG, "👋 Greet sequence queued: home → wave → bow");
    } else if (strstr(action, "attack")) {
        // Angry emoji when attacking
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("angry");
        // Attack sequence: forward → jump → bow
        otto_controller_queue_action(ACTION_DOG_WALK, 2, 100, 0, 0);
        otto_controller_queue_action(ACTION_DOG_JUMP, 2, 200, 0, 0);
        otto_controller_queue_action(ACTION_DOG_BOW, 1, 150, 0, 0);
        ret = ESP_OK;
        ESP_LOGI(TAG, "⚔️ Attack sequence queued: forward → jump → bow");
    } else if (strstr(action, "celebrate")) {
        // Happy emoji when celebrating
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("happy");
        // Celebrate sequence: dance → wave → swing
        otto_controller_queue_action(ACTION_DOG_DANCE, 2, 200, 0, 0);
        otto_controller_queue_action(ACTION_DOG_WAVE_RIGHT_FOOT, 5, 100, 0, 0);
        otto_controller_queue_action(ACTION_DOG_SWING, 3, 10, 0, 0);  // Changed from 150 to 10 for faster swing
        ret = ESP_OK;
        ESP_LOGI(TAG, "🎉 Celebrate sequence queued: dance → wave → swing");
    } else if (strstr(action, "search")) {
        // Scared emoji when searching (cautious)
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("scared");
        // Search sequence: look left → look right → walk forward
        otto_controller_queue_action(ACTION_DOG_TURN_LEFT, 2, 150, 0, 0);
        otto_controller_queue_action(ACTION_DOG_TURN_RIGHT, 4, 150, 0, 0);
        otto_controller_queue_action(ACTION_DOG_TURN_LEFT, 2, 150, 0, 0);
        otto_controller_queue_action(ACTION_DOG_WALK, 3, 120, 0, 0);
        ret = ESP_OK;
        ESP_LOGI(TAG, "🔍 Search sequence queued: look around → walk forward");
    } else if (strstr(action, "roll_over")) {
        // Excited emoji when rolling over
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("excited");
        ret = otto_controller_queue_action(ACTION_DOG_ROLL_OVER, param1 > 0 ? param1 : 1, param2 > 0 ? param2 : 200, 0, 0);
        ESP_LOGI(TAG, "🐕 Rolling over: %d rolls, speed %d", param1 > 0 ? param1 : 1, param2 > 0 ? param2 : 200);
    } else if (strstr(action, "play_dead")) {
        // Shocked emoji when playing dead
        if (auto display = Board::GetInstance().GetDisplay()) display->SetEmotion("shocked");
        ret = otto_controller_queue_action(ACTION_DOG_PLAY_DEAD, 1, param1 > 0 ? param1 : 5, 0, 0);
        ESP_LOGI(TAG, "💀 Playing dead for %d seconds", param1 > 0 ? param1 : 5);
    } else if (strstr(action, "shake_paw")) {
        ret = otto_controller_queue_action(ACTION_DOG_SHAKE_PAW, param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 150, 0, 0);
        ESP_LOGI(TAG, "🤝 Shaking paw: %d shakes, speed %d", param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 150);
    // Removed sidestep actions (tools deleted to stay under 32 limit)
    /*
    } else if (strstr(action, "sidestep_right")) {
        ret = otto_controller_queue_action(ACTION_DOG_SIDESTEP, param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 80, 1, 0);  // direction = 1 (right)
        ESP_LOGI(TAG, "➡️ Sidestepping right: %d steps, speed %d", param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 80);
    } else if (strstr(action, "sidestep_left")) {
        ret = otto_controller_queue_action(ACTION_DOG_SIDESTEP, param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 80, -1, 0);  // direction = -1 (left)
        ESP_LOGI(TAG, "⬅️ Sidestepping left: %d steps, speed %d", param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 80);
    */
    } else if (strstr(action, "pushup")) {
        ret = otto_controller_queue_action(ACTION_DOG_PUSHUP, param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 150, 0, 0);
        ESP_LOGI(TAG, "💪 Doing pushups: %d pushups, speed %d", param1 > 0 ? param1 : 3, param2 > 0 ? param2 : 150);
    } else if (strstr(action, "balance")) {
        ret = otto_controller_queue_action(ACTION_DOG_BALANCE, param1 > 0 ? param1 : 2000, param2 > 0 ? param2 : 150, 0, 0);
        ESP_LOGI(TAG, "⚖️ Balancing: %d ms duration, speed %d", param1 > 0 ? param1 : 2000, param2 > 0 ? param2 : 150);
    } else if (strstr(action, "stop")) {
        // Stop action - clear queue and go to home position
        ret = otto_controller_stop_all();  // This will clear all queued actions
        ESP_LOGI(TAG, "🛑 STOP - all actions cancelled, robot at home");
    } else {
        ESP_LOGW(TAG, "❌ Unknown action: %s", action);
        return;
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ Action queued successfully");
    } else {
        ESP_LOGE(TAG, "❌ Failed to queue action: %s", esp_err_to_name(ret));
    }
}

extern "C" {

// Action handler
esp_err_t otto_action_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎯 ACTION HANDLER CALLED!"); // Debug logging
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    
    char query[200] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Query string: %s", query);
        
        char cmd[50] = {0};
        char p1_str[20] = {0};
        char p2_str[20] = {0};
        
        httpd_query_key_value(query, "cmd", cmd, sizeof(cmd));
        httpd_query_key_value(query, "p1", p1_str, sizeof(p1_str));
        httpd_query_key_value(query, "p2", p2_str, sizeof(p2_str));
        
        int param1 = atoi(p1_str);
        int param2 = atoi(p2_str);
        
        ESP_LOGI(TAG, "Action: %s, P1: %d, P2: %d", cmd, param1, param2);
        
        // Execute action
        otto_execute_web_action(cmd, param1, param2);
        
        // Send response
        httpd_resp_set_type(req, "text/plain");
        char response[200];
        snprintf(response, sizeof(response), "✅ Otto executed: %s (steps: %d, speed: %d)", cmd, param1, param2);
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing action parameters");
    }
    
    return ESP_OK;
}

// Status handler
esp_err_t otto_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    
    // Simple status - can be expanded with actual Otto status
    httpd_resp_sendstr(req, "ready");
    
    return ESP_OK;
}

// Emotion handler
esp_err_t otto_emotion_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "😊 EMOTION HANDLER CALLED!"); // Debug logging
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Emotion query: %s", query);
        
        char emotion[50] = {0};
        httpd_query_key_value(query, "emotion", emotion, sizeof(emotion));
        
        ESP_LOGI(TAG, "Setting emotion: %s", emotion);
        
        // Send emotion to display system with fallback
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            // Try Otto display first for GIF support
            auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
            if (otto_display) {
                otto_display->SetEmotion(emotion);
            } else {
                // Fallback to regular display for text emoji
                display->SetEmotion(emotion);
            }
            
            httpd_resp_set_type(req, "text/plain");
            char response[100];
            snprintf(response, sizeof(response), "✅ Emotion set to: %s", emotion);
            httpd_resp_sendstr(req, response);
        } else {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "❌ Display system not available");
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing emotion parameter");
    }
    
    return ESP_OK;
}

// Emoji mode handler
esp_err_t otto_emoji_mode_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎭 EMOJI MODE HANDLER CALLED!"); // Debug logging
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Emoji mode query: %s", query);
        
        char mode[20] = {0};
        httpd_query_key_value(query, "mode", mode, sizeof(mode));
        
    // Accept both 'gif' and 'otto' as Otto GIF mode keywords
    bool use_otto_emoji = (strcmp(mode, "gif") == 0) || (strcmp(mode, "otto") == 0);
        ESP_LOGI(TAG, "Setting emoji mode: %s (use_otto: %d)", mode, use_otto_emoji);
        
        // Send mode change to display system
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            if (use_otto_emoji) {
                // Try to cast to OttoEmojiDisplay for GIF mode
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    otto_display->SetEmojiMode(true);
                    // Ensure the GIF is visible immediately by setting neutral emotion
                    otto_display->SetEmotion("neutral");
                    httpd_resp_set_type(req, "text/plain");
                    httpd_resp_sendstr(req, "✅ Emoji mode set to: Otto GIF");
                } else {
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    httpd_resp_sendstr(req, "❌ Otto GIF display not available");
                }
            } else {
                // Use text emoji mode (Twemoji)
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    otto_display->SetEmojiMode(false); // Set to text emoji mode
                    otto_display->SetEmotion("happy"); // Set happy text emoji to show Unicode emoji
                    httpd_resp_set_type(req, "text/plain");
                    httpd_resp_sendstr(req, "✅ Emoji mode set to: Twemoji Text");
                } else {
                    display->SetEmotion("neutral"); // Fallback for non-Otto displays
                    httpd_resp_set_type(req, "text/plain");
                    httpd_resp_sendstr(req, "✅ Emoji mode set to: Default Text");
                }
            }
        } else {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "❌ Display system not available");
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing mode parameter");
    }
    
    return ESP_OK;
}

// Touch sensor control handler
esp_err_t otto_touch_sensor_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🖐️ TOUCH SENSOR HANDLER CALLED!"); // Debug logging
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Touch sensor query: %s", query);
        
        char enabled_str[10] = {0};
        httpd_query_key_value(query, "enabled", enabled_str, sizeof(enabled_str));
        
        bool enabled = (strcmp(enabled_str, "true") == 0);
        ESP_LOGI(TAG, "Setting touch sensor: %s", enabled ? "ENABLED" : "DISABLED");
        
        // Set touch sensor state
        otto_set_touch_sensor_enabled(enabled);
        
        httpd_resp_set_type(req, "text/plain");
        char response[100];
        snprintf(response, sizeof(response), "✅ Cảm biến chạm đã %s", enabled ? "BẬT" : "TẮT");
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing enabled parameter");
    }
    
    return ESP_OK;
}

// Volume control handler
esp_err_t otto_volume_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🔊 VOLUME HANDLER CALLED!");
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Volume query: %s", query);
        
        char level_str[10] = {0};
        httpd_query_key_value(query, "level", level_str, sizeof(level_str));
        
        int volume_level = atoi(level_str);
        if (volume_level < 0) volume_level = 0;
        if (volume_level > 100) volume_level = 100;
        
        ESP_LOGI(TAG, "🔊 Setting volume to: %d%%", volume_level);
        
        // Get AudioCodec instance and set volume
        Board& board = Board::GetInstance();
        if (board.GetAudioCodec()) {
            board.GetAudioCodec()->SetOutputVolume(volume_level);
            ESP_LOGI(TAG, "✅ Audio volume set successfully to %d%%", volume_level);
        } else {
            ESP_LOGW(TAG, "⚠️ AudioCodec not available");
        }
        
        // Also show volume change on display
        if (board.GetDisplay()) {
            char volume_msg[64];
            snprintf(volume_msg, sizeof(volume_msg), "Âm lượng: %d%%", volume_level);
            board.GetDisplay()->SetChatMessage("system", volume_msg);
        }
        
        httpd_resp_set_type(req, "text/plain");
        char response[100];
        snprintf(response, sizeof(response), "✅ Âm lượng đã đặt: %d%%", volume_level);
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing level parameter");
    }
    
    return ESP_OK;
}

// Auto pose handler with pose selection
esp_err_t otto_auto_pose_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🔄 AUTO POSE HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[300] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Auto pose query: %s", query);
        
        char enabled_str[10] = {0};
        char poses_str[200] = {0};
        httpd_query_key_value(query, "enabled", enabled_str, sizeof(enabled_str));
        httpd_query_key_value(query, "poses", poses_str, sizeof(poses_str));
        
        bool enabled = (strcmp(enabled_str, "true") == 0);
        
        // Update selected poses if provided
        if (strlen(poses_str) > 0) {
            strncpy(selected_poses, poses_str, sizeof(selected_poses) - 1);
            ESP_LOGI(TAG, "📝 Selected poses: %s", selected_poses);
        }
        
        ESP_LOGI(TAG, "Setting auto pose: %s", enabled ? "ENABLED" : "DISABLED");
        
        auto_pose_enabled = enabled;
        
        if (enabled) {
            // Create timer if not exists
            if (auto_pose_timer == NULL) {
                auto_pose_timer = xTimerCreate(
                    "AutoPoseTimer",
                    pdMS_TO_TICKS(auto_pose_interval_ms),
                    pdTRUE,                 // Auto-reload
                    NULL,
                    auto_pose_timer_callback
                );
            }
            
            // Update timer period and start
            if (auto_pose_timer != NULL) {
                xTimerChangePeriod(auto_pose_timer, pdMS_TO_TICKS(auto_pose_interval_ms), 0);
                xTimerStart(auto_pose_timer, 0);
                ESP_LOGI(TAG, "✅ Auto pose timer started with interval %lu ms", auto_pose_interval_ms);
            }
        } else {
            // Stop the timer
            if (auto_pose_timer != NULL) {
                xTimerStop(auto_pose_timer, 0);
                ESP_LOGI(TAG, "⏹️ Auto pose timer stopped");
            }
        }
        
        httpd_resp_set_type(req, "text/plain");
        char response[100];
        snprintf(response, sizeof(response), "✅ Tự động đổi tư thế đã %s", enabled ? "BẬT" : "TẮT");
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing enabled parameter");
    }
    
    return ESP_OK;
}

// Auto pose interval handler
esp_err_t otto_auto_pose_interval_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char seconds_str[10] = {0};
        httpd_query_key_value(query, "seconds", seconds_str, sizeof(seconds_str));
        
        int seconds = atoi(seconds_str);
        if (seconds >= 5 && seconds <= 300) {
            auto_pose_interval_ms = seconds * 1000;
            ESP_LOGI(TAG, "⏱️ Auto pose interval set to %d seconds", seconds);
            
            // Update timer if running
            if (auto_pose_enabled && auto_pose_timer != NULL) {
                xTimerChangePeriod(auto_pose_timer, pdMS_TO_TICKS(auto_pose_interval_ms), 0);
            }
            
            httpd_resp_set_type(req, "text/plain");
            char response[100];
            snprintf(response, sizeof(response), "✅ Đã đặt thời gian: %d giây", seconds);
            httpd_resp_sendstr(req, response);
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "❌ Thời gian phải từ 5-300 giây");
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing seconds parameter");
    }
    
    return ESP_OK;
}

// Auto emoji handler with emoji selection
esp_err_t otto_auto_emoji_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "😊 AUTO EMOJI HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[400] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "📥 Auto emoji query: %s", query);
        
        char enabled_str[10] = {0};
        char emojis_str[300] = {0};
        httpd_query_key_value(query, "enabled", enabled_str, sizeof(enabled_str));
        httpd_query_key_value(query, "emojis", emojis_str, sizeof(emojis_str));
        
        bool enabled = (strcmp(enabled_str, "true") == 0);
        
        // Update selected emojis if provided
        if (strlen(emojis_str) > 0) {
            strncpy(selected_emojis, emojis_str, sizeof(selected_emojis) - 1);
            ESP_LOGI(TAG, "📝 Selected emojis: %s", selected_emojis);
        }
        
        ESP_LOGI(TAG, "Setting auto emoji: %s", enabled ? "ENABLED" : "DISABLED");
        
        auto_emoji_enabled = enabled;
        
        if (enabled) {
            // Create timer if not exists
            if (auto_emoji_timer == NULL) {
                auto_emoji_timer = xTimerCreate(
                    "AutoEmojiTimer",
                    pdMS_TO_TICKS(auto_emoji_interval_ms),
                    pdTRUE,                 // Auto-reload
                    NULL,
                    auto_emoji_timer_callback
                );
            }
            
            // Update timer period and start
            if (auto_emoji_timer != NULL) {
                xTimerChangePeriod(auto_emoji_timer, pdMS_TO_TICKS(auto_emoji_interval_ms), 0);
                xTimerStart(auto_emoji_timer, 0);
                ESP_LOGI(TAG, "✅ Auto emoji timer started with interval %lu ms", auto_emoji_interval_ms);
            }
        } else {
            // Stop the timer
            if (auto_emoji_timer != NULL) {
                xTimerStop(auto_emoji_timer, 0);
                ESP_LOGI(TAG, "⏹️ Auto emoji timer stopped");
            }
        }
        
        httpd_resp_set_type(req, "text/plain");
        char response[100];
        snprintf(response, sizeof(response), "✅ Tự động đổi cảm xúc đã %s", enabled ? "BẬT" : "TẮT");
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing enabled parameter");
    }
    
    return ESP_OK;
}

// Auto emoji interval handler
esp_err_t otto_auto_emoji_interval_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char seconds_str[10] = {0};
        httpd_query_key_value(query, "seconds", seconds_str, sizeof(seconds_str));
        
        int seconds = atoi(seconds_str);
        if (seconds >= 3 && seconds <= 300) {
            auto_emoji_interval_ms = seconds * 1000;
            ESP_LOGI(TAG, "⏱️ Auto emoji interval set to %d seconds", seconds);
            
            // Update timer if running
            if (auto_emoji_enabled && auto_emoji_timer != NULL) {
                xTimerChangePeriod(auto_emoji_timer, pdMS_TO_TICKS(auto_emoji_interval_ms), 0);
            }
            
            httpd_resp_set_type(req, "text/plain");
            char response[100];
            snprintf(response, sizeof(response), "✅ Đã đặt thời gian: %d giây", seconds);
            httpd_resp_sendstr(req, response);
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "❌ Thời gian phải từ 3-300 giây");
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "❌ Missing seconds parameter");
    }
    
    return ESP_OK;
}

// Screen toggle handler - now with auto-off control
esp_err_t otto_screen_toggle_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "📱 SCREEN TOGGLE HANDLER CALLED!");

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Parse query parameters
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param_value[32] = {0};
        
        // Check for auto_off parameter
        if (httpd_query_key_value(query, "auto_off", param_value, sizeof(param_value)) == ESP_OK) {
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                // Cast to OttoEmojiDisplay to access auto-off methods
                OttoEmojiDisplay* otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    bool enable = (strcmp(param_value, "true") == 0);
                    otto_display->SetAutoOffEnabled(enable);
                    
                    httpd_resp_set_type(req, "text/plain");
                    char response[100];
                    snprintf(response, sizeof(response), "✅ Auto-off (5 min): %s", enable ? "BẬT" : "TẮT");
                    httpd_resp_sendstr(req, response);
                    return ESP_OK;
                }
            }
        }
    }

    // Get display instance and toggle power save mode (legacy behavior)
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        // Use power save mode to simulate screen toggle
        static bool power_save_mode = false; // Track current state
        power_save_mode = !power_save_mode;

        display->SetPowerSaveMode(power_save_mode);

        ESP_LOGI(TAG, "📱 Power save mode toggled: %s", power_save_mode ? "ON" : "OFF");

        httpd_resp_set_type(req, "text/plain");
        char response[100];
        snprintf(response, sizeof(response), "✅ Chế độ tiết kiệm năng lượng: %s", power_save_mode ? "BẬT" : "TẮT");
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "❌ Display system not available");
    }

    return ESP_OK;
}

// Send text to AI handler - Web UI chat feature
// Architecture: Frontend → HTTP POST → ESP32 → WebSocket → AI Server
esp_err_t otto_send_text_to_ai_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "💬 SEND TEXT TO AI HANDLER CALLED!");

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    // Read POST data with size limit
    const size_t MAX_CONTENT_SIZE = 2048;
    char content[MAX_CONTENT_SIZE] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        } else {
        const char* error_resp = "{\"success\":false,\"message\":\"Failed to receive data\"}";
        httpd_resp_sendstr(req, error_resp);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "📥 Received POST data: %s", content);

    // Parse JSON
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        ESP_LOGW(TAG, "❌ Failed to parse JSON");
        const char* error_resp = "{\"success\":false,\"message\":\"Invalid JSON format\"}";
        httpd_resp_sendstr(req, error_resp);
        return ESP_FAIL;
    }

    // Extract and validate text field
    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_item) || text_item->valuestring == NULL) {
        ESP_LOGW(TAG, "❌ Missing or invalid 'text' field");
        const char* error_resp = "{\"success\":false,\"message\":\"Missing 'text' field\"}";
        httpd_resp_sendstr(req, error_resp);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    std::string text = text_item->valuestring;
    cJSON_Delete(root);

    // Text Validation
    if (text.empty()) {
        ESP_LOGW(TAG, "❌ Empty text");
        const char* error_resp = "{\"success\":false,\"message\":\"Text cannot be empty\"}";
        httpd_resp_sendstr(req, error_resp);
        return ESP_FAIL;
    }

    // Length validation (max 1500 characters)
    if (text.length() > 1500) {
        ESP_LOGW(TAG, "❌ Text too long: %zu characters", text.length());
        const char* error_resp = "{\"success\":false,\"message\":\"Text too long (max 1500 characters)\"}";
        httpd_resp_sendstr(req, error_resp);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ Text validated: %zu characters", text.length());

    // Display Integration: Show user message on display immediately
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display != nullptr) {
        display->SetChatMessage("user", text.c_str());
    }

    // Async Processing: Schedule SendSttMessage on main task
    // This sends text as STT message via WebSocket to AI server
    // HTTP response returns immediately, AI processing happens async
    Application::GetInstance().Schedule([text]() {
        bool success = Application::GetInstance().SendSttMessage(text);
        if (!success) {
            ESP_LOGW("OttoWebServer", "Failed to send STT message to server");
        }
    });

    // Send success response immediately (non-blocking)
    const char* success_resp = "{\"success\":true,\"message\":\"Text sent to AI successfully\"}";
    httpd_resp_sendstr(req, success_resp);

    ESP_LOGI(TAG, "✅ Response sent, processing async");
    return ESP_OK;
}

// Wake microphone handler - Toggle listening mode on/off
esp_err_t otto_wake_mic_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎤 WAKE MICROPHONE HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Check for action parameter (start or stop)
    char action_buf[10];
    if (httpd_req_get_url_query_str(req, action_buf, sizeof(action_buf)) == ESP_OK) {
        char action[10];
        if (httpd_query_key_value(action_buf, "action", action, sizeof(action)) == ESP_OK) {
            if (strcmp(action, "stop") == 0) {
                // Stop listening mode - use ToggleChatState like boot button
                Application::GetInstance().ToggleChatState();
                ESP_LOGI(TAG, "Microphone toggled off");
                
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "✅ Microphone đã tắt! �");
                return ESP_OK;
            }
        }
    }
    
    // Default: Toggle listening mode - use ToggleChatState like boot button
    Application::GetInstance().ToggleChatState();
    ESP_LOGI(TAG, "Microphone toggled on");
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "✅ Microphone đang lắng nghe! 🎤");
    
    return ESP_OK;
}

// Forget WiFi handler - Reset WiFi and enter AP mode for configuration
esp_err_t otto_forget_wifi_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🔄 FORGET WIFI HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Stop WiFi completely to prevent auto-reconnection
    esp_wifi_stop();
    ESP_LOGI(TAG, "🔄 WiFi stopped");
    
    bool success = false;
    
    // Erase WiFi credentials from wifi_config namespace
    nvs_handle_t wifi_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &wifi_handle);
    if (err == ESP_OK) {
        // Erase WiFi SSID and password
        nvs_erase_key(wifi_handle, "ssid");
        nvs_erase_key(wifi_handle, "password");
        nvs_commit(wifi_handle);
        nvs_close(wifi_handle);
        ESP_LOGI(TAG, "✅ WiFi credentials erased from wifi_config namespace");
        success = true;
    } else {
        ESP_LOGE(TAG, "⚠️ Failed to open wifi_config NVS: %s", esp_err_to_name(err));
    }
    
    // Set force_ap flag in wifi namespace to enter AP mode
    nvs_handle_t settings_handle;
    err = nvs_open("wifi", NVS_READWRITE, &settings_handle);
    if (err == ESP_OK) {
        nvs_set_i32(settings_handle, "force_ap", 1);
        nvs_commit(settings_handle);
        nvs_close(settings_handle);
        ESP_LOGI(TAG, "✅ force_ap flag set to 1 in wifi namespace");
        success = true;
    } else {
        ESP_LOGE(TAG, "⚠️ Failed to open wifi NVS: %s", esp_err_to_name(err));
    }
    
    if (success) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "✅ Đã quên WiFi. Robot sẽ khởi động lại và tạo AP để cấu hình WiFi mới...");
        
        ESP_LOGI(TAG, "🔄 Restarting to enter AP mode for WiFi configuration");
        
        // Restart the device after a short delay
        vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds
        esp_restart();
    } else {
        ESP_LOGE(TAG, "❌ Failed to forget WiFi");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "❌ Không thể xóa thông tin WiFi");
    }
    
    return ESP_OK;
}

// Gemini API Key handler - Save API key to NVS
esp_err_t otto_gemini_api_key_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Read POST body (API key)
    char buf[200];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid request body\"}");
        return ESP_OK;
    }
    buf[ret] = '\0';
    
    // Parse JSON: {"api_key": "AIza..."}
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }
    
    cJSON *api_key_json = cJSON_GetObjectItem(root, "api_key");
    if (!cJSON_IsString(api_key_json)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Missing api_key field\"}");
        return ESP_OK;
    }
    
    const char* api_key = api_key_json->valuestring;
    ESP_LOGI(TAG, "🔑 Saving Gemini API key: %s...", api_key[0] ? "AIza***" : "(empty)");
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "gemini_key", api_key);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "✅ Gemini API key saved to NVS");
            
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"API key saved successfully\"}");
        } else {
            ESP_LOGE(TAG, "❌ Failed to save API key: %d", err);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"Failed to save API key\"}");
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "❌ Failed to open NVS: %d", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"Failed to open storage\"}");
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

// Get Gemini API Key handler - Check if key is configured
esp_err_t otto_gemini_get_key_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    // Read API key from NVS (masked)
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        char api_key[200];
        size_t required_size = sizeof(api_key);
        err = nvs_get_str(nvs_handle, "gemini_key", api_key, &required_size);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK && strlen(api_key) > 0) {
            // Mask the key (show first 8 chars only)
            char masked[210];
            if (strlen(api_key) > 8) {
                snprintf(masked, sizeof(masked), "%.8s***", api_key);
            } else {
                snprintf(masked, sizeof(masked), "%s", api_key);
            }
            
            char response[256];
            snprintf(response, sizeof(response), 
                    "{\"configured\":true,\"key_preview\":\"%s\"}", masked);
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
    }
    
    // No key configured
    httpd_resp_sendstr(req, "{\"configured\":false}");
    return ESP_OK;
}

// Start HTTP server
esp_err_t otto_start_webserver(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 19;  // Reduced after removing UDP Drawing handlers
    config.max_resp_headers = 8;
    config.stack_size = 8192;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = otto_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t action_uri = {
            .uri = "/action",
            .method = HTTP_GET,
            .handler = otto_action_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &action_uri);
        
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = otto_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);
        
        // New emotion control handlers
        httpd_uri_t emotion_uri = {
            .uri = "/emotion",
            .method = HTTP_GET,
            .handler = otto_emotion_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &emotion_uri);
        
        httpd_uri_t emoji_mode_uri = {
            .uri = "/emoji_mode",
            .method = HTTP_GET,
            .handler = otto_emoji_mode_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &emoji_mode_uri);
        
        // Touch sensor handler - HIDDEN
        // httpd_uri_t touch_sensor_uri = {
        //     .uri = "/touch_sensor",
        //     .method = HTTP_GET,
        //     .handler = otto_touch_sensor_handler,
        //     .user_ctx = NULL
        // };
        // httpd_register_uri_handler(server, &touch_sensor_uri);
        
        // Volume control handler registration
        httpd_uri_t volume_uri = {
            .uri = "/volume",
            .method = HTTP_GET,
            .handler = otto_volume_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &volume_uri);
        
        // Auto pose handler registration
        httpd_uri_t auto_pose_uri = {
            .uri = "/auto_pose",
            .method = HTTP_GET,
            .handler = otto_auto_pose_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &auto_pose_uri);
        
        // Auto pose interval handler registration
        httpd_uri_t auto_pose_interval_uri = {
            .uri = "/auto_pose_interval",
            .method = HTTP_GET,
            .handler = otto_auto_pose_interval_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &auto_pose_interval_uri);
        
        // Auto emoji handler registration
        httpd_uri_t auto_emoji_uri = {
            .uri = "/auto_emoji",
            .method = HTTP_GET,
            .handler = otto_auto_emoji_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &auto_emoji_uri);
        
        // Auto emoji interval handler registration
        httpd_uri_t auto_emoji_interval_uri = {
            .uri = "/auto_emoji_interval",
            .method = HTTP_GET,
            .handler = otto_auto_emoji_interval_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &auto_emoji_interval_uri);
        
        // Screen toggle handler registration
        httpd_uri_t screen_toggle_uri = {
            .uri = "/screen_toggle",
            .method = HTTP_GET,
            .handler = otto_screen_toggle_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &screen_toggle_uri);
        
        // Forget WiFi handler registration
        httpd_uri_t forget_wifi_uri = {
            .uri = "/forget_wifi",
            .method = HTTP_GET,
            .handler = otto_forget_wifi_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &forget_wifi_uri);
        
        // Wake microphone handler registration (supports start/stop)
        httpd_uri_t wake_mic_uri = {
            .uri = "/wake_mic",
            .method = HTTP_GET,
            .handler = otto_wake_mic_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wake_mic_uri);
        
        // Gemini API Key handler
        httpd_uri_t gemini_api_key_uri = {
            .uri = "/gemini_api_key",
            .method = HTTP_POST,
            .handler = otto_gemini_api_key_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &gemini_api_key_uri);
        
        // Get Gemini API Key handler
        httpd_uri_t gemini_get_key_uri = {
            .uri = "/gemini_api_key",
            .method = HTTP_GET,
            .handler = otto_gemini_get_key_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &gemini_get_key_uri);
        
        // Send text to AI handler (Web UI chat feature)
        httpd_uri_t send_text_to_ai_uri = {
            .uri = "/api/ai/send",
            .method = HTTP_POST,
            .handler = otto_send_text_to_ai_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &send_text_to_ai_uri);
        
        ESP_LOGI(TAG, "HTTP server started successfully (with UDP Drawing + Gemini API support)");
        webserver_enabled = true;
        
        // Create and start auto-stop timer (30 minutes)
        if (webserver_auto_stop_timer == NULL) {
            webserver_auto_stop_timer = xTimerCreate(
                "WebServerAutoStop",
                pdMS_TO_TICKS(WEBSERVER_AUTO_STOP_DELAY_MS),
                pdFALSE,  // One-shot timer
                NULL,
                webserver_auto_stop_callback
            );
        }
        
        if (webserver_auto_stop_timer != NULL) {
            xTimerStart(webserver_auto_stop_timer, 0);
            ESP_LOGI(TAG, "⏱️ Webserver will auto-stop in 30 minutes");
        }
        
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

// Stop HTTP server
esp_err_t otto_stop_webserver(void) {
    if (server == NULL) {
        ESP_LOGW(TAG, "Server not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping HTTP server...");
    
    // Stop auto-stop timer
    if (webserver_auto_stop_timer != NULL) {
        xTimerStop(webserver_auto_stop_timer, 0);
        ESP_LOGI(TAG, "⏱️ Webserver auto-stop timer stopped");
    }
    
    // Stop the server
    esp_err_t err = httpd_stop(server);
    if (err == ESP_OK) {
        server = NULL;
        webserver_enabled = false;
        ESP_LOGI(TAG, "HTTP server stopped successfully");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(err));
        return err;
    }
}

// UDP Drawing Service integration
static UdpDrawService* g_udp_draw_service = nullptr;
static DrawingDisplay* g_drawing_display = nullptr;

// Set UDP Drawing Service pointer (called from otto_robot.cc)
void otto_set_udp_draw_service(UdpDrawService* service) {
    g_udp_draw_service = service;
    ESP_LOGI(TAG, "UDP Drawing Service pointer set for web UI");
}

// Set Drawing Display pointer (called from otto_robot.cc)
void otto_set_drawing_display(DrawingDisplay* display) {
    g_drawing_display = display;
    ESP_LOGI(TAG, "Drawing Display pointer set for web UI");
}



} // extern "C"
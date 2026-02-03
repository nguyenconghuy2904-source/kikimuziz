#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <wifi_station.h>

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

#define TAG "OttoRobot"

extern void InitializeOttoController();

// C++ wrapper for web server functions
extern "C" {
    esp_err_t otto_start_webserver(void);
}

static esp_err_t StartOttoWebServer() {
    return otto_start_webserver();
}

class OttoRobot : public WifiBoard {
private:
    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    Esp32Music* music_player_;  // HTTP audio streaming music player
    // TTP223 is active HIGH on touch; enable power-save mode
    // Now on GPIO 12 instead of GPIO 2
    Button touch_button_{TOUCH_TTP223_GPIO, true, 0, 0, true};
    bool touch_sensor_enabled_ = true;  // Touch sensor can be disabled via web UI
    
    // Touch counting for IP display feature
    int touch_count_ = 0;                 // Count consecutive touches
    uint32_t last_touch_time_ = 0;        // Time of last touch (for timeout)
    
    // UDP Drawing Service
    std::unique_ptr<DrawingDisplay> drawing_display_;
    std::unique_ptr<UdpDrawService> udp_draw_service_;
    
    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_ADC_UNIT, POWER_ADC_CHANNEL);
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
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new OttoEmojiDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        // Start with Otto GIF mode, show happy emoji by default
        if (display_) {
            static_cast<OttoEmojiDisplay*>(display_)->SetEmojiMode(true);
            display_->SetEmotion("happy");  // Welcoming expression
            ESP_LOGI(TAG, "🤖 Otto GIF mode enabled with happy emoji");
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
    }

    void InitializeOttoController() {
        ESP_LOGI(TAG, "初始化Otto机器人MCP控制器");
        ::InitializeOttoController();
    }

    void InitializeUdpDrawingService() {
        ESP_LOGI(TAG, "🎨 Initializing UDP Drawing Service...");
        
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
        
        auto& state_manager = DeviceStateEventManager::GetInstance();
        state_manager.RegisterStateChangeCallback(
            [this](DeviceState previous_state, DeviceState current_state) {
                ESP_LOGI(TAG, "🔄 State changed: %d -> %d", previous_state, current_state);
                
                // When ASR fails (listening -> idle without proper recognition)
                if (previous_state == kDeviceStateListening && 
                    current_state == kDeviceStateIdle) {
                    
                    // Check if it's an ASR error (no text recognized)
                    // If we transitioned from listening to idle quickly, it's likely an error
                    ESP_LOGW(TAG, "❌ ASR error detected - Robot will lie down");
                    
                    // Display confused emotion
                    if (display_) {
                        display_->SetEmotion("confused");
                    }
                    
                    // Lie down with speed 3200
                    otto_controller_queue_action(ACTION_DOG_LIE_DOWN, 1, 3200, 0, 0);
                    ESP_LOGI(TAG, "🛏️ Queued lie down action (speed 3200)");
                    
                    vTaskDelay(pdMS_TO_TICKS(3500));  // Wait for lie down to complete
                    
                    // Return to neutral emotion
                    if (display_) {
                        display_->SetEmotion("neutral");
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
    OttoRobot() : boot_button_(BOOT_BUTTON_GPIO), music_player_(nullptr) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeOttoController();
        InitializeUdpDrawingService();
        InitializeWebServer();
        InitializeStateChangeCallback();
        GetBacklight()->RestoreBrightness();
        
        // Initialize music player
        music_player_ = new Esp32Music();
        ESP_LOGI(TAG, "🎵 Music player initialized");
    }
    
    ~OttoRobot() {
        if (music_player_) {
            delete music_player_;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
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

    MusicPlayer* GetMusicPlayer() { 
        return music_player_;  // Return HTTP streaming music player
    }

    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        
        // Start UDP Drawing Service sau khi WiFi connected
        ESP_LOGI(TAG, "🎨 Starting UDP Drawing Service...");
        if (udp_draw_service_ && udp_draw_service_->Start()) {
            ESP_LOGI(TAG, "✅ UDP Drawing Service started on port 12345");
            ESP_LOGI(TAG, "🎨 Drawing web UI: http://[IP]/draw");
        }
    }

public:
    // Method to control touch sensor state
    void SetTouchSensorEnabled(bool enabled) {
        touch_sensor_enabled_ = enabled;
        ESP_LOGI(TAG, "🖐️ Touch sensor %s", enabled ? "ENABLED" : "DISABLED");
    }
    
    bool IsTouchSensorEnabled() const {
        return touch_sensor_enabled_;
    }
    
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
}

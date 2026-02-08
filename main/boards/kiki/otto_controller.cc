/*
    Otto机器人控制器 - MCP协议版本
*/

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs.h>

#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "config.h"
#include "mcp_server.h"
#include "otto_movements.h"
#include "otto_emoji_display.h"
#include "sdkconfig.h"
#include "settings.h"
#include "kiki_led_control.h"

// Forward declarations for web server control
extern "C" {
    esp_err_t otto_start_webserver(void);
    esp_err_t otto_stop_webserver(void);
    // Alarm functions
    bool set_alarm_from_mcp(int seconds_from_now, const char* mode, const char* message);
    bool cancel_alarm_from_mcp();
    int get_alarm_remaining_seconds();
}

#define TAG "OttoController"
#define ACTION_DOG_WAG_TAIL 22

// Static timer for QR display reset (avoids creating new tasks)
static TimerHandle_t qr_reset_timer = nullptr;
static void qr_reset_timer_callback(TimerHandle_t xTimer) {
    auto disp = Board::GetInstance().GetDisplay();
    if (disp) {
        disp->SetEmotion("neutral");
    }
    ESP_LOGI(TAG, "🔓 QR display ended, emotion reset to neutral");
}

class OttoController {
private:
    Otto otto_;
    TaskHandle_t action_task_handle_ = nullptr;
    QueueHandle_t action_queue_;
    bool is_action_in_progress_ = false;
    // Idle management
    // Accumulated idle time in milliseconds (we increment by LOOP_IDLE_INCREMENT_MS each idle cycle)
    int idle_no_action_ticks_ = 0;    // milliseconds without actions
    int64_t idle_timeout_ms_ = 3600000; // Default: 1 hour = 60 * 60 * 1000 ms (now configurable)
    static constexpr int LOOP_IDLE_INCREMENT_MS = 20;   // Each idle loop adds 20 ms (vTaskDelay(20ms))
    bool idle_mode_ = false;          // true when idle behavior is active

    struct OttoActionParams {
        int action_type;
        int steps;
        int speed;
        int direction;
        int amount;
    };

    enum ActionType {
        // Dog-style movement actions (new)
        ACTION_DOG_WALK = 1,
        ACTION_DOG_WALK_BACK = 2,
        ACTION_DOG_TURN_LEFT = 3,
        ACTION_DOG_TURN_RIGHT = 4,
        ACTION_DOG_SIT_DOWN = 5,
        ACTION_DOG_LIE_DOWN = 6,
        ACTION_DOG_JUMP = 7,
        ACTION_DOG_BOW = 8,
        ACTION_DOG_DANCE = 9,
        ACTION_DOG_WAVE_RIGHT_FOOT = 10,
        ACTION_DOG_DANCE_4_FEET = 11,
        ACTION_DOG_SWING = 12,
        ACTION_DOG_STRETCH = 13,
        ACTION_DOG_SCRATCH = 14,  // New: Sit + BR leg scratch (gãi ngứa)
        
        // Legacy actions (adapted for 4 servos)
        ACTION_WALK = 15,
        ACTION_TURN = 16,
        ACTION_JUMP = 17,
        ACTION_BEND = 18,
        ACTION_HOME = 19,
        // Utility actions
        ACTION_DELAY = 20,  // Delay in milliseconds, use 'speed' as delay duration
        ACTION_DOG_JUMP_HAPPY = 21,  // Special: Jump with happy emoji (for touch sensor)
        ACTION_DOG_ROLL_OVER = 23,  // New: Roll over movement
        ACTION_DOG_PLAY_DEAD = 24,  // New: Play dead movement
        
        // New poses (Priority 1 + 2)
        ACTION_DOG_SHAKE_PAW = 25,  // New: Shake paw (bắt tay)
        ACTION_DOG_SIDESTEP = 26,  // New: Sidestep (đi ngang)
    ACTION_DOG_PUSHUP = 27,  // New: Pushup exercise
    ACTION_DOG_BALANCE = 28,  // New: Balance on hind legs
    ACTION_DOG_TOILET = 29,  // New: Toilet squat pose
    ACTION_DOG_STAND_UP = 30  // New: Stand up from sitting/lying
    };

    static void ActionTask(void* arg) {
        OttoController* controller = static_cast<OttoController*>(arg);
        OttoActionParams params;
        
        ESP_LOGI(TAG, "🚀 ActionTask started! Attaching servos...");
        controller->otto_.AttachServos();
        ESP_LOGI(TAG, "✅ Servos attached successfully");

        while (true) {
            // Use shorter timeout (100ms) for faster response to new actions
            if (xQueueReceive(controller->action_queue_, &params, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "⚡ Executing action: type=%d, steps=%d, speed=%d", 
                         params.action_type, params.steps, params.speed);
                controller->is_action_in_progress_ = true;
                controller->idle_no_action_ticks_ = 0; // reset idle timer on new action
                
                // Exit idle mode and re-attach servos if needed
                if (controller->idle_mode_) {
                    ESP_LOGI(TAG, "🔌 Waking up from idle - re-attaching servos and turning on display");
                    
                    // Turn on display first
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->SetPowerSaveMode(false);
                    }
                    auto backlight = Board::GetInstance().GetBacklight();
                    if (backlight) {
                        backlight->RestoreBrightness(); // Restore previous brightness
                    }
                    
                    // Restart web server
                    ESP_LOGI(TAG, "🌐 Restarting web server...");
                    otto_start_webserver();
                    
                    // Re-attach servos
                    controller->otto_.AttachServos();
                    vTaskDelay(pdMS_TO_TICKS(50));  // Give servos time to stabilize
                }
                controller->idle_mode_ = false;

                switch (params.action_type) {
                    // Dog-style movement actions
                    case ACTION_DOG_WALK:
                        controller->otto_.DogWalk(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after walk
                        break;
                    case ACTION_DOG_WALK_BACK:
                        ESP_LOGI(TAG, "🐕 DogWalkBack: steps=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogWalkBack(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after walk back
                        break;
                    case ACTION_DOG_TURN_LEFT:
                        ESP_LOGI(TAG, "🐕 DogTurnLeft: steps=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogTurnLeft(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after turn
                        break;
                    case ACTION_DOG_TURN_RIGHT:
                        ESP_LOGI(TAG, "🐕 DogTurnRight: steps=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogTurnRight(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after turn
                        break;
                    case ACTION_DOG_SIT_DOWN:
                        ESP_LOGI(TAG, "🐕 DogSitDown: speed=%d", params.speed);
                        controller->otto_.DogSitDown(params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after sit
                        break;
                    case ACTION_DOG_LIE_DOWN:
                        ESP_LOGI(TAG, "🐕 DogLieDown: speed=%d (no tail wag)", params.speed);
                        controller->otto_.DogLieDown(params.speed);
                        // NO tail wag for lie down
                        break;
                    case ACTION_DOG_JUMP:
                        {
                            // Show angry emoji on jump and keep until complete
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("angry");
                            controller->otto_.DogJump(params.speed);
                            controller->otto_.WagTail(3, 100); // Wag tail after jump
                            // Reset to neutral after jump completes
                            if (display) display->SetEmotion("neutral");
                        }
                        break;
                    case ACTION_DOG_JUMP_HAPPY:
                        {
                            // Touch-triggered happy jump
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("happy");
                            controller->otto_.DogJump(params.speed);
                            controller->otto_.WagTail(3, 100); // Wag tail after happy jump
                            // Reset to neutral after jump completes
                            if (display) display->SetEmotion("neutral");
                        }
                        break;
                    case ACTION_DOG_BOW:
                        controller->otto_.DogBow(params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after bow
                        break;
                    case ACTION_DOG_DANCE:
                        controller->otto_.DogDance(params.steps, params.speed);
                        controller->otto_.WagTail(5, 80); // More energetic tail wag after dance
                        break;
                    case ACTION_DOG_WAVE_RIGHT_FOOT:
                        controller->otto_.DogWaveRightFoot(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after wave
                        break;
                    case ACTION_DOG_DANCE_4_FEET:
                        controller->otto_.DogDance4Feet(params.steps, params.speed);
                        controller->otto_.WagTail(5, 80); // More energetic tail wag after dance
                        break;
                    case ACTION_DOG_SWING:
                        controller->otto_.DogSwing(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after swing
                        break;
                    case ACTION_DOG_STRETCH:
                        {
                            // Always show sleepy emoji during stretch and keep until complete
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("sleepy");
                            controller->otto_.DogStretch(params.steps, params.speed);
                            // NO tail wag for stretch (too sleepy!)
                            // Reset to neutral after stretch completes
                            if (display) display->SetEmotion("neutral");
                        }
                        break;
                    case ACTION_DOG_SCRATCH:
                        ESP_LOGI(TAG, "🐕 DogScratch: scratches=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogScratch(params.steps, params.speed);
                        controller->otto_.WagTail(3, 100); // Wag tail after scratch
                        break;
                    case ACTION_DOG_WAG_TAIL:
                        ESP_LOGI(TAG, "🐕 WagTail: wags=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.WagTail(params.steps, params.speed);
                        break;
                    
                    case ACTION_DOG_ROLL_OVER:
                        {
                            ESP_LOGI(TAG, "🔄 DogRollOver: rolls=%d, speed=%d", params.steps, params.speed);
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("excited");
                            // Roll over sequence: lie down → swing side to side → lie down opposite → back to home
                            controller->otto_.DogLieDown(1000);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            controller->otto_.DogSwing(3, 10);  // Swing to simulate rolling
                            vTaskDelay(pdMS_TO_TICKS(500));
                            controller->otto_.DogLieDown(1000);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            controller->otto_.Home();
                            controller->otto_.WagTail(5, 100); // Happy tail wag after roll
                            if (display) display->SetEmotion("happy");
                        }
                        break;
                    
                    case ACTION_DOG_PLAY_DEAD:
                        {
                            ESP_LOGI(TAG, "💀 DogPlayDead: duration=%d seconds", params.speed);
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("neutral");
                            // Play dead: lie down and stay still for specified seconds
                            controller->otto_.DogLieDown(1000);
                            vTaskDelay(pdMS_TO_TICKS(params.speed * 1000));  // Stay dead for speed seconds
                            // Wake up slowly
                            controller->otto_.DogSitDown(800);
                            vTaskDelay(pdMS_TO_TICKS(500));
                            controller->otto_.Home();
                            if (display) display->SetEmotion("happy");
                        }
                        break;
                    
                    // New poses (Priority 1 + 2)
                    case ACTION_DOG_SHAKE_PAW:
                        ESP_LOGI(TAG, "🤝 DogShakePaw: shakes=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogShakePaw(params.steps, params.speed);
                        break;
                    
                    case ACTION_DOG_SIDESTEP:
                        ESP_LOGI(TAG, "⬅️➡️ DogSidestep: steps=%d, speed=%d, direction=%d", 
                                 params.steps, params.speed, params.direction);
                        controller->otto_.DogSidestep(params.steps, params.speed, params.direction);
                        break;
                    
                    case ACTION_DOG_PUSHUP:
                        {
                            ESP_LOGI(TAG, "💪 DogPushup: pushups=%d, speed=%d", params.steps, params.speed);
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("confused");
                            // Execute pushup movement
                            controller->otto_.DogPushup(params.steps, params.speed);
                            // Keep confused emotion until pose completes (blocking LLM emoji changes)
                            vTaskDelay(pdMS_TO_TICKS(500));
                            if (display) display->SetEmotion("happy");
                        }
                        break;
                    
                    case ACTION_DOG_BALANCE:
                        ESP_LOGI(TAG, "⚖️ DogBalance: duration=%d ms, speed=%d", params.steps, params.speed);
                        controller->otto_.DogBalance(params.steps, params.speed);
                        break;
                    case ACTION_DOG_TOILET:
                        ESP_LOGI(TAG, "🚽 DogToilet: hold=%d ms, speed=%d", params.steps, params.speed);
                        controller->otto_.DogToilet(params.steps, params.speed);
                        break;
                    
                    case ACTION_DOG_STAND_UP:
                        ESP_LOGI(TAG, "🧍 DogStandUp: Standing up to rest position");
                        controller->otto_.StandUp();
                        break;
                        
                    // Legacy actions (adapted for 4 servos)
                    case ACTION_WALK:
                        controller->otto_.Walk(params.steps, params.speed, params.direction);
                        controller->otto_.WagTail(3, 100); // Wag tail after walk
                        break;
                    case ACTION_TURN:
                        controller->otto_.Turn(params.steps, params.speed, params.direction);
                        controller->otto_.WagTail(3, 100); // Wag tail after turn
                        break;
                    case ACTION_JUMP:
                        {
                            auto display = Board::GetInstance().GetDisplay();
                            if (display) display->SetEmotion("angry");
                            controller->otto_.Jump(params.steps, params.speed);
                            controller->otto_.WagTail(3, 100); // Wag tail after jump
                            // Reset to neutral after jump completes
                            if (display) display->SetEmotion("neutral");
                        }
                        break;
                    case ACTION_BEND:
                        controller->otto_.Bend(params.steps, params.speed, params.direction);
                        controller->otto_.WagTail(3, 100); // Wag tail after bend
                        break;
                    case ACTION_HOME:
                        ESP_LOGI(TAG, "🏠 Going Home");
                        controller->otto_.Home();
                        break;
                    case ACTION_DELAY:
                        ESP_LOGI(TAG, "⏱️ Delay: %d ms", params.speed);
                        vTaskDelay(pdMS_TO_TICKS(params.speed));
                        break;
                    default:
                        ESP_LOGW(TAG, "⚠️ Unknown action type: %d", params.action_type);
                        break;
                }
                
                // Note: Removed auto-return-to-home logic to allow action sequences
                // If you need to return home, queue ACTION_HOME explicitly
                
                controller->is_action_in_progress_ = false;
                ESP_LOGI(TAG, "✅ Action completed");
                vTaskDelay(pdMS_TO_TICKS(20));
            } else {
                // No action received within the polling timeout -> accumulate idle time
                controller->idle_no_action_ticks_ += LOOP_IDLE_INCREMENT_MS;

                // Periodic progress log every 5 minutes (300000 ms)
                if (!controller->idle_mode_ && (controller->idle_no_action_ticks_ % 300000) == 0) {
                    int minutes = controller->idle_no_action_ticks_ / 60000;
                    int timeout_minutes = controller->idle_timeout_ms_ / 60000;
                    float percent = (controller->idle_no_action_ticks_ * 100.0f) / controller->idle_timeout_ms_;
                    ESP_LOGI(TAG, "⌛ Idle for %d min (%.1f%% of %d min timeout)", minutes, percent, timeout_minutes);
                }

                // Enter idle (power save) mode after configured timeout without actions
                if (!controller->idle_mode_ && controller->idle_no_action_ticks_ >= controller->idle_timeout_ms_) {
                    int timeout_minutes = controller->idle_timeout_ms_ / 60000;
                    ESP_LOGI(TAG, "🛌 Idle timeout reached (%d min). Entering power save: lying down, turning off display, stopping web server.", timeout_minutes);
                    controller->idle_mode_ = true;

                    // Move to lie down posture at a gentle pace (single call)
                    controller->otto_.DogLieDown(1500);
                    // Wait for movement to complete
                    vTaskDelay(pdMS_TO_TICKS(500));
                    
                    // Turn off display (power save mode + brightness 0)
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->SetPowerSaveMode(true);
                    }
                    auto backlight = Board::GetInstance().GetBacklight();
                    if (backlight) {
                        backlight->SetBrightness(0);
                    }
                    
                    // Lie down posture already set above; no second DogLieDown needed
                    ESP_LOGI(TAG, "🛌 Position settled, proceeding with servo detach and web server stop");

                    // Stop web server to save power
                    ESP_LOGI(TAG, "🌐 Stopping web server to save power...");
                    otto_stop_webserver();
                    
                    // Detach servos to stop PWM - saves power and prevents servo jitter
                    controller->otto_.DetachServos();
                    ESP_LOGI(TAG, "💤 Servos detached - power saving mode activated (lie down position)");
                }
            }
        }
    }

    void StartActionTaskIfNeeded() {
        if (action_task_handle_ == nullptr) {
            ESP_LOGI(TAG, "🚀 Creating ActionTask...");
            BaseType_t result = xTaskCreate(ActionTask, "otto_action", 1024 * 3, this, 
                                           configMAX_PRIORITIES - 1, &action_task_handle_);
            if (result == pdPASS) {
                ESP_LOGI(TAG, "✅ ActionTask created successfully with handle: %p", action_task_handle_);
            } else {
                ESP_LOGE(TAG, "❌ Failed to create ActionTask!");
                action_task_handle_ = nullptr;
            }
        } else {
            ESP_LOGD(TAG, "ActionTask already running");
        }
    }

    void QueueAction(int action_type, int steps, int speed, int direction, int amount) {
        ESP_LOGI(TAG, "🎯 QueueAction called: type=%d, steps=%d, speed=%d, direction=%d, amount=%d", 
                 action_type, steps, speed, direction, amount);

        if (action_queue_ == nullptr) {
            ESP_LOGE(TAG, "❌ Action queue is NULL! Cannot queue action.");
            return;
        }

        OttoActionParams params = {action_type, steps, speed, direction, amount};
        
        // Use short timeout (100ms) instead of portMAX_DELAY to avoid blocking
        BaseType_t result = xQueueSend(action_queue_, &params, pdMS_TO_TICKS(100));
        if (result == pdTRUE) {
            ESP_LOGI(TAG, "✅ Action queued successfully. Queue space remaining: %d", 
                     uxQueueSpacesAvailable(action_queue_));
            StartActionTaskIfNeeded();
        } else {
            // Queue full - drop oldest action by receiving and discarding it, then retry
            ESP_LOGW(TAG, "⚠️ Queue full, dropping oldest action...");
            OttoActionParams dummy;
            if (xQueueReceive(action_queue_, &dummy, 0) == pdTRUE) {
                ESP_LOGW(TAG, "🗑️ Dropped action type %d to make room", dummy.action_type);
                result = xQueueSend(action_queue_, &params, pdMS_TO_TICKS(100));
                if (result == pdTRUE) {
                    ESP_LOGI(TAG, "✅ Action queued after dropping oldest");
                    StartActionTaskIfNeeded();
                } else {
                    ESP_LOGE(TAG, "❌ Failed to queue action even after dropping!");
                }
            } else {
                ESP_LOGE(TAG, "❌ Queue full but failed to receive - inconsistent state");
            }
        }
    }

    void LoadTrimsFromNVS() {
        Settings settings("otto_trims", false);

        int left_front = settings.GetInt("left_front", 0);
        int right_front = settings.GetInt("right_front", 0);
        int left_back = settings.GetInt("left_back", 0);
        int right_back = settings.GetInt("right_back", 0);

        ESP_LOGI(TAG, "从NVS加载微调设置: 左前=%d, 右前=%d, 左后=%d, 右后=%d",
                 left_front, right_front, left_back, right_back);

        otto_.SetTrims(left_front, right_front, left_back, right_back);
    }
    
    void LoadServoHomeFromNVS() {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "ℹ️ No servo calibration found in NVS, using defaults (90°)");
            return;
        }
        
        // Ensure nvs_close is called even if errors occur
        int32_t lf = 90, rf = 90, lb = 90, rb = 90;
        
        esp_err_t err_lf = nvs_get_i32(nvs_handle, "servo_lf", &lf);
        esp_err_t err_rf = nvs_get_i32(nvs_handle, "servo_rf", &rf);
        esp_err_t err_lb = nvs_get_i32(nvs_handle, "servo_lb", &lb);
        esp_err_t err_rb = nvs_get_i32(nvs_handle, "servo_rb", &rb);
        
        nvs_close(nvs_handle);  // Always close
        
        // Only apply if at least one value was found
        if (err_lf == ESP_OK || err_rf == ESP_OK || err_lb == ESP_OK || err_rb == ESP_OK) {
            ESP_LOGI(TAG, "📐 Loading servo home positions from NVS: LF=%d RF=%d LB=%d RB=%d", 
                (int)lf, (int)rf, (int)lb, (int)rb);
            
            // Set servos to calibrated home position
            otto_.ServoInit(lf, rf, lb, rb, 1000);
            
            ESP_LOGI(TAG, "✅ Servo home positions applied");
        } else {
            ESP_LOGI(TAG, "ℹ️ No servo calibration found in NVS, using defaults (90°)");
        }
    }

    void LoadIdleTimeoutFromNVS() {
        nvs_handle_t nvs_handle;
        esp_err_t open_err = nvs_open("otto", NVS_READONLY, &nvs_handle);
        if (open_err != ESP_OK) {
            ESP_LOGI(TAG, "ℹ️ No idle timeout found in NVS, using default (60 minutes)");
            return;
        }
        
        uint32_t timeout_minutes = 60;  // Default 60 minutes
        esp_err_t err = nvs_get_u32(nvs_handle, "idle_timeout", &timeout_minutes);
        nvs_close(nvs_handle);  // Always close
            
        if (err == ESP_OK) {
            idle_timeout_ms_ = (int64_t)timeout_minutes * 60 * 1000;
            ESP_LOGI(TAG, "⏰ Loaded idle timeout from NVS: %lu minutes (%lld ms)", timeout_minutes, idle_timeout_ms_);
        } else {
            ESP_LOGI(TAG, "ℹ️ No idle timeout found in NVS, using default (60 minutes)");
        }
    }

public:
    ~OttoController() {
        // Cleanup resources
        if (action_queue_ != nullptr) {
            vQueueDelete(action_queue_);
            action_queue_ = nullptr;
        }
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        otto_.DetachServos();
        ESP_LOGI(TAG, "🧹 OttoController resources cleaned up");
    }

    OttoController() {
        // Debug servo pins before initialization
        ESP_LOGI(TAG, "🤖 Initializing OttoController...");
        ESP_LOGI(TAG, "Servo pins configuration:");
        ESP_LOGI(TAG, "  LEFT_LEG_PIN (Left Front): GPIO %d", LEFT_LEG_PIN);
        ESP_LOGI(TAG, "  RIGHT_LEG_PIN (Right Front): GPIO %d", RIGHT_LEG_PIN);
        ESP_LOGI(TAG, "  LEFT_FOOT_PIN (Left Back): GPIO %d", LEFT_FOOT_PIN);
        ESP_LOGI(TAG, "  RIGHT_FOOT_PIN (Right Back): GPIO %d", RIGHT_FOOT_PIN);
        ESP_LOGI(TAG, "  DOG_TAIL_PIN (Tail): GPIO %d", DOG_TAIL_PIN);
        
        // Initialize Otto with 5 servo pins (4 legs + tail)
        otto_.Init(LEFT_LEG_PIN, RIGHT_LEG_PIN, LEFT_FOOT_PIN, RIGHT_FOOT_PIN, DOG_TAIL_PIN);

        ESP_LOGI(TAG, "✅ Kiki Dog Robot initialized with 5 servos (4 legs + tail)");

        LoadTrimsFromNVS();
        LoadServoHomeFromNVS();  // Load calibrated home positions
        LoadIdleTimeoutFromNVS();  // Load idle timeout setting

        ESP_LOGI(TAG, "📦 Creating action queue (size=10)...");
        action_queue_ = xQueueCreate(10, sizeof(OttoActionParams));
        
        if (action_queue_ == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Failed to create action queue!");
        } else {
            ESP_LOGI(TAG, "✅ Action queue created successfully");
        }

        ESP_LOGI(TAG, "🏠 Queuing initial HOME action...");
        QueueAction(ACTION_HOME, 1, 1000, 0, 0);  // Initialize to home position

        RegisterMcpTools();
        ESP_LOGI(TAG, "🎉 KikiController initialization complete!");
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        ESP_LOGI(TAG, "🐕 Registering Kiki the Adorable Dog Robot MCP Tools...");

        // NOTE: Trimmed tool set to respect 32-tool limit (system tools + motion tools = 32).
        // Removed legacy otto.* tools and advanced/sequenced dog.* tools (defend, attack, celebrate, scratch,
        // search, pushup, balance, test_servo, home) to reduce count.
        // If future expansion needed, consider a single multiplexing tool (self.motion.run).

        // IMPORTANT: I am Kiki, a cute 4-legged dog robot! 🐶
        // I can walk, run, sit, lie down, jump, dance, wave, and do tricks like a real puppy!
        // ALL dog actions consolidated into ONE tool to reduce MCP tool count

        // 🔧 CONSOLIDATED DOG CONTROL TOOL (replaces 17 separate tools)
        mcp_server.AddTool("self.dog",
            "🐕 Robot dog control. ALL movements in ONE tool.\n"
            "Actions: walk_forward, walk_backward, turn_left, turn_right, sit, lie, stand, jump, bow, dance, wave, dance4, swing, stretch, pushup, wag, toilet, stop, home\n"
            "Args:\n"
            "  action: Movement action name\n"
            "  steps: Steps/cycles (1-10, default 3)\n"
            "  speed: Speed in ms (50-500, default 150)",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("steps", kPropertyTypeInteger, 3, 1, 10),
                Property("speed", kPropertyTypeInteger, 150, 50, 500)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                
                auto display = Board::GetInstance().GetDisplay();
                
                if (action == "walk_forward" || action == "forward" || action == "walk") {
                    ESP_LOGI(TAG, "🐾 Walking forward %d steps", steps);
                    otto_.DogWalk(steps, speed);
                    otto_.WagTail(3, 100);
                    return true;
                }
                else if (action == "walk_backward" || action == "backward" || action == "back") {
                    ESP_LOGI(TAG, "🐾 Walking backward %d steps", steps);
                    otto_.DogWalkBack(steps, speed);
                    otto_.WagTail(3, 100);
                    return true;
                }
                else if (action == "turn_left" || action == "left") {
                    ESP_LOGI(TAG, "🐾 Turning left %d steps", steps);
                    otto_.DogTurnLeft(steps, speed);
                    otto_.WagTail(3, 100);
                    return true;
                }
                else if (action == "turn_right" || action == "right") {
                    ESP_LOGI(TAG, "🐾 Turning right %d steps", steps);
                    otto_.DogTurnRight(steps, speed);
                    otto_.WagTail(3, 100);
                    return true;
                }
                else if (action == "sit" || action == "sit_down") {
                    ESP_LOGI(TAG, "🐾 Sitting down");
                    otto_.DogSitDown(speed * 3);
                    return true;
                }
                else if (action == "lie" || action == "lie_down") {
                    ESP_LOGI(TAG, "🐾 Lying down");
                    otto_.DogLieDown(speed * 6);
                    return true;
                }
                else if (action == "stand" || action == "stand_up") {
                    ESP_LOGI(TAG, "🐾 Standing up");
                    otto_.StandUp();
                    return true;
                }
                else if (action == "jump") {
                    ESP_LOGI(TAG, "🐾 Jumping");
                    otto_.DogJump(speed);
                    return true;
                }
                else if (action == "bow") {
                    ESP_LOGI(TAG, "🐾 Bowing");
                    otto_.DogBow(speed * 10);
                    return true;
                }
                else if (action == "dance") {
                    ESP_LOGI(TAG, "🐾 Dancing %d cycles", steps);
                    if (display) display->SetEmotion("happy");
                    otto_.DogDance(steps, speed);
                    return true;
                }
                else if (action == "wave" || action == "wave_right_foot") {
                    ESP_LOGI(TAG, "🐾 Waving paw %d times", steps);
                    otto_.DogWaveRightFoot(steps, speed / 3);
                    return true;
                }
                else if (action == "dance4" || action == "dance_4_feet") {
                    ESP_LOGI(TAG, "🐾 Dancing with 4 feet");
                    if (display) display->SetEmotion("happy");
                    otto_.DogDance4Feet(steps, speed * 2);
                    return true;
                }
                else if (action == "swing") {
                    ESP_LOGI(TAG, "🐾 Swinging");
                    if (display) display->SetEmotion("happy");
                    otto_.DogSwing(steps, speed / 25);
                    return true;
                }
                else if (action == "stretch" || action == "relax") {
                    ESP_LOGI(TAG, "🐾 Stretching");
                    if (display) display->SetEmotion("sleepy");
                    otto_.DogStretch(steps, speed / 10);
                    return true;
                }
                else if (action == "pushup") {
                    ESP_LOGI(TAG, "💪 Doing pushups %d times", steps);
                    if (display) display->SetEmotion("confused");
                    otto_.DogPushup(steps, speed);
                    if (display) display->SetEmotion("happy");
                    return true;
                }
                else if (action == "wag" || action == "wag_tail") {
                    ESP_LOGI(TAG, "🐾 Wagging tail");
                    if (display) display->SetEmotion("happy");
                    otto_.WagTail(steps, speed / 1.5);
                    return true;
                }
                else if (action == "toilet") {
                    ESP_LOGI(TAG, "🚽 Toilet pose");
                    if (display) display->SetEmotion("embarrassed");
                    otto_.DogToilet(speed * 20, speed);
                    if (display) display->SetEmotion("neutral");
                    return true;
                }
                else if (action == "stop") {
                    ESP_LOGI(TAG, "🛑 Stopping all actions");
                    if (action_task_handle_ != nullptr) {
                        vTaskDelete(action_task_handle_);
                        action_task_handle_ = nullptr;
                    }
                    is_action_in_progress_ = false;
                    xQueueReset(action_queue_);
                    otto_.Home();
                    return true;
                }
                else if (action == "home") {
                    ESP_LOGI(TAG, "🏠 Going to home position");
                    otto_.Home();
                    return true;
                }
                else {
                    return std::string("{\"success\": false, \"message\": \"Unknown action: ") + action + 
                           ". Use: walk_forward/backward, turn_left/right, sit, lie, stand, jump, bow, dance, wave, dance4, swing, stretch, pushup, wag, toilet, stop, home\"}";
                }
            });

        // 🔧 CONSOLIDATED UTILITY TOOL (replaces show_qr, show_ip, webserver.open)
        mcp_server.AddTool("self.utils",
            "🔧 Utility functions: QR code, IP address, webserver\n"
            "Args:\n"
            "  action: qr/ip/web\n"
            "    qr: Show winking emoji 30s for QR code display\n"
            "    ip: Display WiFi IP address on screen\n"
            "    web: Start webserver control panel",
            PropertyList({Property("action", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                auto display = Board::GetInstance().GetDisplay();
                
                if (action == "qr" || action == "show_qr") {
                    ESP_LOGI(TAG, "📱 Showing QR mode for 30s");
                    if (display) display->SetEmotion("winking");
                    if (qr_reset_timer == nullptr) {
                        qr_reset_timer = xTimerCreate("qr_reset", pdMS_TO_TICKS(30000), 
                                                       pdFALSE, nullptr, qr_reset_timer_callback);
                    }
                    if (qr_reset_timer) {
                        xTimerStop(qr_reset_timer, 0);
                        xTimerStart(qr_reset_timer, 0);
                    }
                    return true;
                }
                else if (action == "ip" || action == "show_ip") {
                    ESP_LOGI(TAG, "📱 Showing IP address");
                    if (display) display->SetEmotion("happy");
                    esp_netif_ip_info_t ip_info;
                    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        char ip_str[64];
                        snprintf(ip_str, sizeof(ip_str), "📱 IP: %d.%d.%d.%d", IP2STR(&ip_info.ip));
                        if (display) display->SetChatMessage("system", ip_str);
                        return std::string("{\"ip\": \"") + ip_str + "\"}";
                    }
                    return "{\"error\": \"WiFi not connected\"}";
                }
                else if (action == "web" || action == "webserver") {
                    ESP_LOGI(TAG, "🌐 Starting webserver");
                    extern bool webserver_enabled;
                    if (!webserver_enabled) {
                        if (otto_start_webserver() != ESP_OK) return false;
                    }
                    if (display) {
                        display->SetEmotion("happy");
                        esp_netif_ip_info_t ip_info;
                        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                            char ip_str[64];
                            snprintf(ip_str, sizeof(ip_str), "📱 IP: %d.%d.%d.%d", IP2STR(&ip_info.ip));
                            display->SetChatMessage("system", ip_str);
                        }
                    }
                    return true;
                }
                return std::string("{\"error\": \"Unknown action. Use: qr, ip, web\"}");
            });

        // 🔧 CONSOLIDATED EMOJI TOOL (replaces emoji.toggle, emoji.delicious)
        mcp_server.AddTool("self.emoji",
            "😊 Emoji display control\n"
            "Args:\n"
            "  action: toggle/delicious\n"
            "    toggle: Switch between Otto GIF and Twemoji styles\n"
            "    delicious: Show yummy face for food topics",
            PropertyList({Property("action", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                auto display = Board::GetInstance().GetDisplay();
                
                if (action == "toggle") {
                    auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                    if (otto_display) {
                        bool current = otto_display->IsUsingOttoEmoji();
                        otto_display->SetEmojiMode(!current);
                        otto_display->SetEmotion("happy");
                        return current ? "{\"mode\": \"twemoji\"}" : "{\"mode\": \"otto_gif\"}";
                    }
                    return false;
                }
                else if (action == "delicious") {
                    if (display) display->SetEmotion("delicious");
                    return true;
                }
                return "{\"error\": \"Unknown action. Use: toggle, delicious\"}";
            });

        // 🔧 CONSOLIDATED ALARM TOOL (replaces alarm.set, alarm.cancel)
        mcp_server.AddTool("self.alarm",
            "⏰ Alarm timer control\n"
            "Args:\n"
            "  action: set/cancel\n"
            "  minutes: (1-1440) Time in minutes for alarm (only for set)\n"
            "  message: (optional) Reminder message",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("minutes", kPropertyTypeInteger, 5, 1, 1440),
                Property("message", kPropertyTypeString, "")
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                auto display = Board::GetInstance().GetDisplay();
                
                if (action == "set") {
                    int minutes = properties["minutes"].value<int>();
                    std::string message = properties["message"].value<std::string>();
                    const char* mode = message.empty() ? "alarm" : "message";
                    bool success = set_alarm_from_mcp(minutes * 60, mode, message.c_str());
                    if (success && display) display->SetEmotion("happy");
                    return success;
                }
                else if (action == "cancel") {
                    bool success = cancel_alarm_from_mcp();
                    if (success && display) display->SetEmotion("neutral");
                    return success;
                }
                return "{\"error\": \"Use action: set or cancel\"}";
            });

        // Legacy movement functions - COMMENTED OUT (already in self.dog tool)
        /*
        mcp_server.AddTool("self.dog.stop", 
                           "🐕 I stop all my actions immediately like an obedient puppy! Make me stop whatever I'm doing!\n"
                           "Example: 'Otto, stop!' or 'Freeze!' or 'Stay!'", 
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               if (action_task_handle_ != nullptr) {
                                   vTaskDelete(action_task_handle_);
                                   action_task_handle_ = nullptr;
                               }
                               is_action_in_progress_ = false;
                               xQueueReset(action_queue_);

                               ESP_LOGI(TAG, "🐾 Kiki stopped! 🛑");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.Home();
                               return true;
                           });
        */


        // Comment out to reduce tool count below 32 limit
        /*
        mcp_server.AddTool("self.dog.greet",
                           "🐕 I greet people like a friendly puppy! I stand up, wave my paw, and bow politely!\n"
                           "This is a greeting sequence: stand → wave 5x → bow.\n"
                           "Example: 'Otto, say hello!' or 'Greet our guest!' or 'Say hi!'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "🐾 Kiki is greeting! 👋 (stand → wave → bow)");
                               // Set happy emoji and keep during greeting
                               auto display = Board::GetInstance().GetDisplay();
                               if (display) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute sequence immediately
                               otto_.Home();
                               otto_.DogWaveRightFoot(5, 50);
                               otto_.DogBow(2000);
                               // Reset to neutral after greeting
                               if (display) {
                                   display->SetEmotion("neutral");
                               }
                               return true;
                           });
        */

        // Comment out to reduce tool count below 32 limit
        /*
        mcp_server.AddTool("self.dog.retreat",
                           "🐕 I retreat like a cautious puppy escaping danger! I back up fast, turn around, and run away!\n"
                           "This is a retreat sequence: walk back 3 steps → turn right 4x → walk forward 2 steps.\n"
                           "Example: 'Otto, retreat!' or 'Get away!' or 'Run away!'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "🐾 Kiki is retreating! 🏃 (back → turn → run)");
                               // Set scared emoji and keep during retreat
                               auto display = Board::GetInstance().GetDisplay();
                               if (display) {
                                   display->SetEmotion("scared");
                               }
                               // FAST RESPONSE: Execute sequence immediately
                               otto_.DogWalkBack(3, 100);
                               otto_.DogTurnRight(4, 150);
                               otto_.DogWalk(2, 100);
                               // Reset to neutral after retreat
                               if (display) {
                                   display->SetEmotion("neutral");
                               }
                               return true;
                           });
        */


        // New poses (Priority 1 + 2)
        // Comment out shake_paw to reduce tool count
        /*
        mcp_server.AddTool("self.dog.shake_paw",
                           "🐕 I shake my paw like greeting a friend! I lift my right paw and shake it to say hello!\n"
                           "Args:\n"
                           "  shakes (1-5): How many times to shake paw\n"
                           "  speed (50-300ms): Shake speed\n"
                           "Example: 'Otto, shake paw!' or 'Give me your paw!' or 'Shake hands!'",
                           PropertyList({Property("shakes", kPropertyTypeInteger, 3, 1, 5),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 300)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int shakes = properties["shakes"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🤝 Kiki is shaking paw!");
                               otto_.DogShakePaw(shakes, speed);
                               return true;
                           });
        */

        // Comment out sidestep to reduce tool count
        /*
        mcp_server.AddTool("self.dog.sidestep",
                           "🐕 I sidestep like moving sideways! I can step left or right without turning!\n"
                           "Args:\n"
                           "  steps (1-10): How many sidesteps to take\n"
                           "  speed (50-300ms): Sidestep speed\n"
                           "  direction (1=right, -1=left): Which direction to sidestep\n"
                           "Example: 'Otto, step to the right!' or 'Move sideways left!'",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 300),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int direction = properties["direction"].value<int>();
                               ESP_LOGI(TAG, "⬅️➡️ Kiki is sidestepping!");
                               otto_.DogSidestep(steps, speed, direction);
                               return true;
                           });
        */

        // ========== CONSOLIDATED LED TOOL (replaces led.control + led.state) ==========
        mcp_server.AddTool(
            "self.led",
            "🎨 LED control (8x WS2812 RGB)\n"
            "Args:\n"
            "  action: set/status/save (default: set)\n"
            "  red/green/blue: 0-255 RGB color\n"
            "  mode: off/solid/rainbow/breathing/chase/blink\n"
            "  brightness: 0-255\n"
            "  speed: effect speed in ms\n"
            "Colors: red(255,0,0), green(0,255,0), blue(0,0,255), yellow(255,255,0), white, purple, pink, orange",
            PropertyList({
                Property("action", kPropertyTypeString, "set"),
                Property("red", kPropertyTypeInteger, 0, 0, 255),
                Property("green", kPropertyTypeInteger, 0, 0, 255),
                Property("blue", kPropertyTypeInteger, 0, 0, 255),
                Property("mode", kPropertyTypeString),
                Property("brightness", kPropertyTypeInteger, 0, 0, 255),
                Property("speed", kPropertyTypeInteger, 0, 0, 500)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                
                // STATUS action - return current LED state
                if (action == "status" || action == "state" || action == "info") {
                    const led_state_t* state = kiki_led_get_state();
                    const char* modes[] = {"Off", "Solid", "Rainbow", "Breathing", "Chase", "Blink"};
                    const char* mode_name = (state->mode < 6) ? modes[state->mode] : "Unknown";
                    return std::string("{\"r\":") + std::to_string(state->r) + ",\"g\":" + std::to_string(state->g) + 
                           ",\"b\":" + std::to_string(state->b) + ",\"brightness\":" + std::to_string(state->brightness) +
                           ",\"mode\":\"" + mode_name + "\",\"speed\":" + std::to_string(state->speed) + "}";
                }
                
                // SAVE action - save to NVS
                if (action == "save") {
                    kiki_led_save_to_nvs();
                    return "{\"success\":true,\"message\":\"LED settings saved\"}";
                }
                
                // SET action (default) - change LED settings
                std::string result = "LED: ";
                bool changed = false;
                
                int r = properties["red"].value<int>();
                int g = properties["green"].value<int>();
                int b = properties["blue"].value<int>();
                if (r > 0 || g > 0 || b > 0) {
                    kiki_led_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
                    kiki_led_set_mode(LED_MODE_SOLID);
                    result += "RGB(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ") ";
                    changed = true;
                }
                
                std::string mode_str = properties["mode"].value<std::string>();
                if (!mode_str.empty()) {
                    led_mode_t mode = LED_MODE_SOLID;
                    if (mode_str == "off") mode = LED_MODE_OFF;
                    else if (mode_str == "solid") mode = LED_MODE_SOLID;
                    else if (mode_str == "rainbow") mode = LED_MODE_RAINBOW;
                    else if (mode_str == "breathing") mode = LED_MODE_BREATHING;
                    else if (mode_str == "chase") mode = LED_MODE_CHASE;
                    else if (mode_str == "blink") mode = LED_MODE_BLINK;
                    kiki_led_set_mode(mode);
                    result += mode_str + " ";
                    changed = true;
                }
                
                int brightness = properties["brightness"].value<int>();
                if (brightness > 0) {
                    kiki_led_set_brightness((uint8_t)brightness);
                    result += "bright=" + std::to_string((brightness * 100) / 255) + "% ";
                    changed = true;
                }
                
                int speed = properties["speed"].value<int>();
                if (speed >= 10) {
                    kiki_led_set_speed((uint16_t)speed);
                    result += "speed=" + std::to_string(speed) + "ms ";
                    changed = true;
                }
                
                if (changed) {
                    kiki_led_update();
                    return result;
                }
                return "{\"error\":\"Provide color(r,g,b), mode, brightness, or speed\"}";
            });

        ESP_LOGI(TAG, "🐾 Dog Robot MCP tools registered (robot + LED control)! 🐶");
    }

    // Public method for web server to queue actions
    void ExecuteAction(int action_type, int steps, int speed, int direction, int amount) {
        QueueAction(action_type, steps, speed, direction, amount);
    }
    
    // Public method to stop all actions and clear queue
    void StopAll() {
        ESP_LOGI(TAG, "🛑 StopAll() called - clearing queue");
        
        // Reset the queue to clear all pending actions
        if (action_queue_ != nullptr) {
            xQueueReset(action_queue_);
            ESP_LOGI(TAG, "✅ Queue cleared");
        }
        
        // Set flag to stop current action
        is_action_in_progress_ = false;
        
        // Go to home position immediately
        otto_.Home();
        
        ESP_LOGI(TAG, "✅ Robot stopped and at home position");
    }

    // Public method to set servo angle (for servo calibration)
    void SetServoAngle(int servo_id, int angle) {
        otto_.ServoAngleSet(servo_id, angle, 0);
    }

    // Public method to get servo angle (for servo calibration)
    int GetServoAngle(int servo_id) {
        return otto_.GetServoAngle(servo_id);
    }

    // Public method to apply servo home position (for servo calibration)
    void ApplyServoHome(int lf, int rf, int lb, int rb) {
        ESP_LOGI(TAG, "🏠 Applying servo home: LF=%d RF=%d LB=%d RB=%d", lf, rf, lb, rb);
        otto_.ServoInit(lf, rf, lb, rb, 1000);
        ESP_LOGI(TAG, "✅ Servo home applied immediately");
    }

    // Public method to set idle timeout (configurable from web UI)
    void SetIdleTimeout(int64_t timeout_ms) {
        idle_timeout_ms_ = timeout_ms;
        int minutes = timeout_ms / 60000;
        ESP_LOGI(TAG, "⏰ Idle timeout set to %d minutes (%lld ms)", minutes, timeout_ms);
    }
};  // End of OttoController class

static OttoController* g_otto_controller = nullptr;

void InitializeOttoController() {
    if (g_otto_controller == nullptr) {
        g_otto_controller = new OttoController();
        ESP_LOGI(TAG, "Otto控制器已初始化并注册MCP工具");
    }
}

// C interface for webserver to access controller
extern "C" {
    esp_err_t otto_controller_queue_action(int action_type, int steps, int speed, int direction, int amount) {
        ESP_LOGI(TAG, "🌐 Web/Voice request: action=%d, steps=%d, speed=%d, dir=%d, amt=%d", 
                 action_type, steps, speed, direction, amount);
        
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return ESP_ERR_INVALID_STATE;
        }
        
        g_otto_controller->ExecuteAction(action_type, steps, speed, direction, amount);
        return ESP_OK;
    }
    
    // Stop and clear all queued actions
    esp_err_t otto_controller_stop_all() {
        ESP_LOGI(TAG, "🛑 STOP ALL requested from web/external");
        
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return ESP_ERR_INVALID_STATE;
        }
        
        // Call the public StopAll method
        g_otto_controller->StopAll();
        
        return ESP_OK;
    }
    
    void otto_controller_set_servo_angle(int servo_id, int angle) {
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return;
        }
        
        // Set servo angle directly via public method
        g_otto_controller->SetServoAngle(servo_id, angle);
        ESP_LOGI(TAG, "🎚️ Servo %d set to %d°", servo_id, angle);
    }
    
    void otto_controller_get_servo_angles(int* angles) {
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return;
        }
        
        // Get current servo angles via public method
        angles[0] = g_otto_controller->GetServoAngle(0);  // LF
        angles[1] = g_otto_controller->GetServoAngle(1);  // RF
        angles[2] = g_otto_controller->GetServoAngle(2);  // LB
        angles[3] = g_otto_controller->GetServoAngle(3);  // RB
        
        ESP_LOGI(TAG, "📐 Current servo angles: LF=%d RF=%d LB=%d RB=%d", 
            angles[0], angles[1], angles[2], angles[3]);
    }
    
    void otto_controller_apply_servo_home(int lf, int rf, int lb, int rb) {
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return;
        }
        
        g_otto_controller->ApplyServoHome(lf, rf, lb, rb);
    }

    void otto_controller_set_idle_timeout(uint32_t timeout_ms) {
        if (g_otto_controller == nullptr) {
            ESP_LOGE(TAG, "❌ FATAL: Kiki controller not initialized!");
            return;
        }
        
        g_otto_controller->SetIdleTimeout((int64_t)timeout_ms);
    }
}

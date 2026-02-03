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
        // Use these tools to control my movements and make me perform adorable actions.

        // Dog-style movement actions
        mcp_server.AddTool("self.dog.walk_forward",
                           "🐕 I walk forward like a cute puppy! Make me walk forward with my 4 legs.\n"
                           "Args:\n"
                           "  steps (1-10): How many steps I should walk forward\n"
                           "  speed (50-500ms): Movement speed - lower is faster, higher is slower\n"
                           "Example: 'Otto, walk forward 3 steps' or 'Move forward'",
                           PropertyList({Property("steps", kPropertyTypeInteger, 2, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "⚡ IMMEDIATE ACTION: Walking forward %d steps at speed %dms", steps, speed);
                               // FAST RESPONSE: Execute immediately like esp-hi, no queue delay
                               otto_.DogWalk(steps, speed);
                               otto_.WagTail(3, 100); // Happy tail wag!
                               ESP_LOGI(TAG, "✅ Walk forward completed with tail wag");
                               return true;
                           });

        mcp_server.AddTool("self.dog.walk_backward",
                           "🐕 I walk backward like a cautious puppy! Make me step back carefully.\n"
                           "Args:\n"
                           "  steps (1-10): How many steps I should walk backward\n"
                           "  speed (50-500ms): Movement speed - lower is faster\n"
                           "Example: 'Otto, step back' or 'Walk backward 2 steps'",
                           PropertyList({Property("steps", kPropertyTypeInteger, 2, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "⚡ IMMEDIATE ACTION: Walking backward %d steps at speed %dms", steps, speed);
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogWalkBack(steps, speed);
                               otto_.WagTail(3, 100); // Happy tail wag!
                               ESP_LOGI(TAG, "✅ Walk backward completed with tail wag");
                               return true;
                           });

        mcp_server.AddTool("self.dog.turn_left",
                           "🐕 I turn left like a playful puppy! Make me spin to the left.\n"
                           "Args:\n"
                           "  steps (1-10): How many turning movements\n"
                           "  speed (50-500ms): Turn speed\n"
                           "Example: 'Otto, turn left' or 'Spin to the left'",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "⚡ IMMEDIATE ACTION: Turning left %d steps at speed %dms", steps, speed);
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogTurnLeft(steps, speed);
                               otto_.WagTail(3, 100); // Happy tail wag!
                               ESP_LOGI(TAG, "✅ Turn left completed with tail wag");
                               return true;
                           });

        mcp_server.AddTool("self.dog.turn_right",
                           "🐕 I turn right like a curious puppy! Make me spin to the right.\n"
                           "Args:\n"
                           "  steps (1-10): How many turning movements\n"
                           "  speed (50-500ms): Turn speed\n"
                           "Example: 'Otto, turn right' or 'Look to the right'",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "⚡ IMMEDIATE ACTION: Turning right %d steps at speed %dms", steps, speed);
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogTurnRight(steps, speed);
                               otto_.WagTail(3, 100); // Happy tail wag!
                               ESP_LOGI(TAG, "✅ Turn right completed with tail wag");
                               return true;
                           });

        mcp_server.AddTool("self.dog.sit_down",
                           "🐕 I sit down like an obedient puppy! Make me sit nicely.\n"
                           "Args:\n"
                           "  delay (100-2000ms): How long the sitting motion takes\n"
                           "Example: 'Otto, sit!' or 'Sit down like a good boy'",
                           PropertyList({Property("delay", kPropertyTypeInteger, 500, 100, 2000)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int delay = properties["delay"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is sitting down like a good puppy!");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogSitDown(delay);
                               return true;
                           });

        mcp_server.AddTool("self.dog.lie_down",
                           "🐕 I lie down like a tired puppy ready for a nap! Make me lie down and rest.\n"
                           "Args:\n"
                           "  delay (500-3000ms): How long the lying motion takes\n"
                           "Example: 'Otto, lie down' or 'Take a rest' or 'Nap time!'",
                           PropertyList({Property("delay", kPropertyTypeInteger, 1000, 500, 3000)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int delay = properties["delay"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is lying down for a nap!");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogLieDown(delay);
                               return true;
                           });

        mcp_server.AddTool("self.dog.stand_up",
                           "🐕 I stand up like a good puppy! Make me stand up from sitting or lying position!\n"
                           "Use this when user says: 'đứng lên', 'đứng dậy', 'stand up', 'get up', 'dậy đi'\n"
                           "This will make me stand up straight and ready for action!\n"
                           "Example: 'Otto, đứng lên!' or 'Stand up!' or 'Get up!'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "🧍 Kiki is standing up!");
                               // FAST RESPONSE: Execute immediately
                               otto_.StandUp();
                               return true;
                           });

        mcp_server.AddTool("self.dog.jump",
                           "🐕 I jump and dance with excitement like a happy puppy! Make me dance and jump for joy!\n"
                           "Args:\n"
                           "  delay (100-1000ms): Jump and dance speed\n"
                           "Example: 'Otto, dance and jump!' or 'Jump up!' or 'Show me your moves!'",
                           PropertyList({Property("delay", kPropertyTypeInteger, 200, 100, 1000)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int delay = properties["delay"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is dancing and jumping! 💃🦘");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogJump(delay);
                               return true;
                           });

        mcp_server.AddTool("self.dog.bow",
                           "🐕 I bow like a polite puppy greeting you! Make me bow to show respect.\n"
                           "Args:\n"
                           "  delay (1000-5000ms): How long I hold the bow\n"
                           "Example: 'Otto, bow' or 'Greet me nicely' or 'Say hello with a bow'",
                           PropertyList({Property("delay", kPropertyTypeInteger, 2000, 1000, 5000)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int delay = properties["delay"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is bowing politely! 🙇");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogBow(delay);
                               return true;
                           });

        mcp_server.AddTool("self.dog.dance",
                           "🐕 I dance and perform like a joyful puppy celebrating! Make me dance with style and happiness!\n"
                           "Args:\n"
                           "  cycles (1-10): How many dance moves\n"
                           "  speed (100-500ms): Dance speed\n"
                           "Example: 'Otto, dance!' or 'Let's celebrate!' or 'Show me your dance moves!'",
                           PropertyList({Property("cycles", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 200, 100, 500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int cycles = properties["cycles"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is dancing with style! 💃✨");
                               // Set happy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogDance(cycles, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.wave_right_foot",
                           "🐕 I wave my right paw like a friendly puppy saying hi! Make me wave hello!\n"
                           "Args:\n"
                           "  waves (1-10): How many times to wave\n"
                           "  speed (20-200ms): Wave speed\n"
                           "Example: 'Otto, wave!' or 'Say hi!' or 'Wave your paw!'",
                           PropertyList({Property("waves", kPropertyTypeInteger, 5, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 50, 20, 200)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int waves = properties["waves"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is waving his paw! 👋");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogWaveRightFoot(waves, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.dance_4_feet",
                           "🐕 I dance with all 4 feet like an excited puppy! Make me dance with coordinated paw movements!\n"
                           "Args:\n"
                           "  cycles (1-10): How many dance cycles\n"
                           "  speed (200-800ms): Dance speed delay\n"
                           "Example: 'Otto, dance with all your feet!' or 'Do the 4-feet dance!'",
                           PropertyList({Property("cycles", kPropertyTypeInteger, 6, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 300, 200, 800)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int cycles = properties["cycles"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is dancing with all 4 feet! 🎵");
                               // Set happy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogDance4Feet(cycles, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.swing",
                           "🐕 I swing left and right like a happy puppy wagging my whole body! Make me sway with joy!\n"
                           "Args:\n"
                           "  cycles (1-20): How many swing cycles\n"
                           "  speed (5-50ms): Swing speed delay\n"
                           "Example: 'Otto, swing left and right!' or 'Wag your body!'",
                           PropertyList({Property("cycles", kPropertyTypeInteger, 8, 1, 20),
                                         Property("speed", kPropertyTypeInteger, 6, 5, 50)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int cycles = properties["cycles"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is swinging left and right! 🎶");
                               // Set happy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogSwing(cycles, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.stretch",
                           "🐕 I relax like a puppy taking it easy! Make me feel relaxed and comfortable!\n"
                           "Args:\n"
                           "  cycles (1-5): How many relaxation cycles\n"
                           "  speed (10-50ms): Relaxation speed delay\n"
                           "Example: 'Otto, relax!' or 'Take it easy!' or 'Chill out!'",
                           PropertyList({Property("cycles", kPropertyTypeInteger, 2, 1, 5),
                                         Property("speed", kPropertyTypeInteger, 15, 10, 50)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int cycles = properties["cycles"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is relaxing! 😌");
                               // Set sleepy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("sleepy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogStretch(cycles, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.pushup",
                           "🐕💪 I do pushup exercises like a strong puppy training! Make me do pushups to show my strength!\n"
                           "Args:\n"
                           "  pushups (1-10): How many pushup repetitions\n"
                           "  speed (50-300ms): Movement speed between pushups\n"
                           "Example: 'Otto, do pushups!' or 'Exercise time!' or 'Chống đẩy đi!' or 'Tập thể dục!' or 'Hít đất đi!'",
                           PropertyList({Property("pushups", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 300)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int pushups = properties["pushups"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "💪 Kiki is doing pushups! Strong puppy!");
                               
                               // Set confused emoji IMMEDIATELY to block LLM emoji changes
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("confused");
                               }
                               
                               // Response to LLM first before executing
                               std::string response = "Được rồi! Để tôi chống đẩy " + std::to_string(pushups) + " cái nhé! 💪";
                               
                               // Queue the action to execute after response
                               OttoActionParams params;
                               params.action_type = ACTION_DOG_PUSHUP;
                               params.steps = pushups;
                               params.speed = speed;
                               params.direction = 1;  // Default direction
                               params.amount = 0;     // Not used for pushup
                               if (xQueueSend(action_queue_, &params, 0) != pdTRUE) {
                                   ESP_LOGW(TAG, "⚠️ Action queue full, executing directly");
                                   // Already set confused above, now execute
                                   otto_.DogPushup(pushups, speed);
                                   // Set happy after completion
                                   if (auto display = Board::GetInstance().GetDisplay()) {
                                       display->SetEmotion("happy");
                                   }
                               }
                               
                               return response;
                           });

        mcp_server.AddTool("self.dog.wag_tail",
                           "🐕🐾 I wag my tail happily like an excited puppy! Make me express my joy by wagging my tail!\n"
                           "Args:\n"
                           "  wags (1-10): How many times to wag tail\n"
                           "  speed (50-200ms): Wagging speed\n"
                           "Example: 'Otto, wag your tail!' or 'Vẫy đuôi đi!' or 'Show me you're happy!'",
                           PropertyList({Property("wags", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 100, 50, 200)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int wags = properties["wags"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐾 Kiki is wagging tail happily! 🐕");
                               // Set happy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.WagTail(wags, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.toilet",
                           "🐕🚽 I squat down like a puppy doing bathroom business! Make me do toilet pose!\n"
                           "Args:\n"
                           "  hold_ms (1000-5000ms): How long to hold the squat position\n"
                           "  speed (50-300ms): Movement speed\n"
                           "Example: 'Otto, go to toilet!' or 'Đi vệ sinh đi!' or 'Bathroom time!'",
                           PropertyList({Property("hold_ms", kPropertyTypeInteger, 3000, 1000, 5000),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 300)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int hold_ms = properties["hold_ms"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🚽 Kiki is doing toilet pose!");
                               // Set embarrassed emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("embarrassed");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogToilet(hold_ms, speed);
                               // Reset emotion after
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("neutral");
                               }
                               return true;
                           });

        mcp_server.AddTool("self.show_qr",
                           "📱 I show a winking face for 30 seconds to display QR code! Use this when user asks to show QR code, activation code, or control panel access!\n"
                           "This will display a playful winking emoji for 30 seconds (no movement, no text).\n"
                           "Example: 'Show me the QR code' or 'Mở mã QR' or 'Display control panel' or 'Hiển thị mã kích hoạt'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "📱 MCP QR tool called: showing winking emoji for 30s");
                               auto display = Board::GetInstance().GetDisplay();
                               if (display) {
                                   display->SetEmotion("winking");
                                   ESP_LOGI(TAG, "😉 Winking emoji set");
                               }
                               // Use timer instead of creating new task to save memory
                               if (qr_reset_timer == nullptr) {
                                   qr_reset_timer = xTimerCreate("qr_reset", pdMS_TO_TICKS(30000), 
                                                                  pdFALSE, nullptr, qr_reset_timer_callback);
                               }
                               if (qr_reset_timer) {
                                   xTimerStop(qr_reset_timer, 0);  // Stop if running
                                   xTimerStart(qr_reset_timer, 0); // Restart for 30s
                               }
                               return true;
                           });

        mcp_server.AddTool("self.show_ip",
                           "📱 I display my WiFi IP address on screen until TTS ends! Use this when user asks for IP address, network info, or WiFi details!\n"
                           "This will show the device's current IP address with a happy emoji until TTS finishes.\n"
                           "Example: 'Show me your IP' or 'Địa chỉ IP là gì' or 'What's your IP address' or 'Hiển thị 192.168'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "📱 MCP show_ip tool called - will display IP until TTS ends");
                               auto display = Board::GetInstance().GetDisplay();
                               if (display) {
                                   display->SetEmotion("happy");
                               }
                               
                               // Get IP address
                               esp_netif_ip_info_t ip_info;
                               esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                               if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                                   char ip_str[64];
                                   snprintf(ip_str, sizeof(ip_str), "📱 IP: %d.%d.%d.%d", 
                                            IP2STR(&ip_info.ip));
                                   ESP_LOGI(TAG, "🌟 Station IP: " IPSTR, IP2STR(&ip_info.ip));
                                   if (display) {
                                       display->SetChatMessage("system", ip_str);
                                   }
                                   ESP_LOGI(TAG, "✅ IP will be displayed until TTS ends");
                               } else {
                                   ESP_LOGE(TAG, "❌ Failed to get IP info");
                                   if (display) {
                                       display->SetChatMessage("system", "WiFi chưa kết nối!");
                                   }
                               }
                               return true;
                           });

        mcp_server.AddTool("self.webserver.open",
                           "🌐 I start the web server control panel and display IP address until TTS ends! Use this when user wants to open control panel, web interface, or access robot controls!\n"
                           "This will start the HTTP server on port 80 (auto-stops after 30 minutes) and show IP on screen until TTS finishes.\n"
                           "Example: 'Open control panel' or 'Mở trang điều khiển' or 'Start web server' or 'Bật web interface'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "🌐 MCP webserver.open called - will display IP until TTS ends");
                               extern bool webserver_enabled;
                               auto display = Board::GetInstance().GetDisplay();
                               
                               if (!webserver_enabled) {
                                   ESP_LOGI(TAG, "🌐 Starting webserver...");
                                   esp_err_t err = otto_start_webserver();
                                   if (err != ESP_OK) {
                                       ESP_LOGE(TAG, "❌ Failed to start webserver");
                                       return false;
                                   }
                               } else {
                                   ESP_LOGI(TAG, "🌐 Webserver already running");
                               }
                               
                               // Display IP address with happy emoji until TTS ends
                               if (display) {
                                   display->SetEmotion("happy");
                                   
                                   // Get IP address
                                   esp_netif_ip_info_t ip_info;
                                   esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                                   if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                                       char ip_str[64];
                                       snprintf(ip_str, sizeof(ip_str), "📱 IP: %d.%d.%d.%d", 
                                                IP2STR(&ip_info.ip));
                                       ESP_LOGI(TAG, "🌟 Station IP: " IPSTR, IP2STR(&ip_info.ip));
                                       display->SetChatMessage("system", ip_str);
                                       ESP_LOGI(TAG, "✅ IP will be displayed until TTS ends");
                                   } else {
                                       ESP_LOGE(TAG, "❌ Failed to get IP info");
                                       display->SetChatMessage("system", "✅ Web server đã khởi động!");
                                   }
                               }
                               
                               return true;
                           });

        mcp_server.AddTool("self.emoji.toggle",
                           "😊 I switch between Otto GIF emoji and Twemoji! Toggle my emoji display style!\n"
                           "Use this when user says: 'Đổi biểu cảm', 'Thay biểu cảm', 'Change emoji', 'Switch emoji style'\n"
                           "Otto GIF: Animated robot expressions (happy.gif, sad.gif, etc.)\n"
                           "Twemoji: Standard Unicode emoji (😊, 😢, 😍, etc.)\n"
                           "Example: 'Otto, đổi biểu cảm' or 'Switch to Twemoji'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "😊 MCP emoji.toggle called");
                               auto display = Board::GetInstance().GetDisplay();
                               auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                               
                               if (otto_display) {
                                   // Toggle emoji mode
                                   bool current_mode = otto_display->IsUsingOttoEmoji();
                                   otto_display->SetEmojiMode(!current_mode);
                                   
                                   // Show confirmation message
                                   if (!current_mode) {
                                       // Switched to Otto GIF
                                       ESP_LOGI(TAG, "🤖 Switched to Otto GIF emoji mode");
                                       otto_display->SetEmotion("happy");
                                       otto_display->SetChatMessage("system", "Đã chuyển sang Otto GIF emoji 🤖");
                                   } else {
                                       // Switched to Twemoji
                                       ESP_LOGI(TAG, "😊 Switched to Twemoji mode");
                                       otto_display->SetEmotion("happy");
                                       otto_display->SetChatMessage("system", "Đã chuyển sang Twemoji 😊");
                                   }
                                   
                                   return true;
                               } else {
                                   ESP_LOGE(TAG, "❌ Display is not OttoEmojiDisplay");
                                   return false;
                               }
                           });

        // Get delicious keyword from NVS and create MCP tool
        {
            nvs_handle_t nvs_handle;
            char delicious_keyword[128] = "";
            if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
                size_t len = sizeof(delicious_keyword);
                nvs_get_str(nvs_handle, "delicious_kw", delicious_keyword, &len);
                nvs_close(nvs_handle);
            }
            
            // Create description with the saved keyword
            std::string keyword_hint = "";
            if (strlen(delicious_keyword) > 0) {
                keyword_hint = std::string("User's custom keyword: '") + delicious_keyword + "'\n";
            }
            
            std::string tool_desc = "🍕 I show DELICIOUS emoji (excited happy face) until TTS ends! Use this when talking about:\n"
                                    "- Food, eating, yummy, tasty things\n"
                                    "- Pizza, bánh mì, phở, cơm, etc.\n"
                                    "- Expressions of enjoyment or satisfaction\n" +
                                    keyword_hint +
                                    "Example: 'Món này ngon quá' or 'Pizza is delicious' or 'Ăn thôi'";
            
            mcp_server.AddTool("self.emoji.delicious",
                               tool_desc,
                               PropertyList(),
                               [this](const PropertyList& properties) -> ReturnValue {
                                   ESP_LOGI(TAG, "🍕 MCP emoji.delicious called - showing delicious emoji until TTS ends!");
                                   auto display = Board::GetInstance().GetDisplay();
                                   if (display) {
                                       display->SetEmotion("delicious");
                                       ESP_LOGI(TAG, "✅ Delicious emoji will be displayed until TTS ends");
                                   }
                                   return true;
                               });
        }

        // ==================== ALARM TOOLS ====================
        // Note: set_alarm_from_mcp, cancel_alarm_from_mcp declared at file scope
        
        mcp_server.AddTool("self.alarm.set",
                           "⏰ I set an alarm timer! Use this when user wants to set a reminder or alarm!\n"
                           "Args:\n"
                           "  minutes (1-1440): Time in MINUTES until alarm (max 24 hours)\n"
                           "  message (optional): Message to speak when alarm triggers\n"
                           "Examples:\n"
                           "  'Đặt báo thức 5 phút' → minutes=5\n"
                           "  'Nhắc tao 10 phút nữa uống nước' → minutes=10, message='uống nước'\n"
                           "  'Set alarm for 1 hour' → minutes=60\n"
                           "  '30 phút nữa nhắc tao' → minutes=30",
                           PropertyList({Property("minutes", kPropertyTypeInteger, 5, 1, 1440),
                                         Property("message", kPropertyTypeString, "")}),
                           [](const PropertyList& properties) -> ReturnValue {
                               int minutes = properties["minutes"].value<int>();
                               std::string message = properties["message"].value<std::string>();
                               int seconds = minutes * 60;
                               
                               ESP_LOGI(TAG, "⏰ MCP alarm.set: minutes=%d, message='%s'", minutes, message.c_str());
                               
                               // Determine mode based on message
                               const char* mode = message.empty() ? "alarm" : "message";
                               
                               bool success = set_alarm_from_mcp(seconds, mode, message.c_str());
                               
                               if (success) {
                                   auto display = Board::GetInstance().GetDisplay();
                                   if (display) {
                                       display->SetEmotion("happy");
                                   }
                               }
                               return success;
                           });
        
        mcp_server.AddTool("self.alarm.cancel",
                           "⏰ I cancel the current alarm! Use when user wants to stop/cancel alarm!\n"
                           "Examples: 'Hủy báo thức', 'Cancel alarm', 'Tắt nhắc nhở', 'Stop timer'",
                           PropertyList(),
                           [](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "⏰ MCP alarm.cancel called");
                               
                               bool success = cancel_alarm_from_mcp();
                               
                               if (success) {
                                   auto display = Board::GetInstance().GetDisplay();
                                   if (display) {
                                       display->SetEmotion("neutral");
                                   }
                               }
                               return success;
                           });

        // Legacy movement functions (for compatibility - prefer self.dog.* tools for newer features!)

        // System tools
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

        // ========== LED CONTROL TOOLS (MERGED) ==========
        
        // Tool: LED Control (merged: set_color, set_mode, set_brightness, set_speed, off, save)
        mcp_server.AddTool(
            "self.led.control",
            "[Kiki Robot] 🎨 ĐIỀU KHIỂN 8 ĐÈN LED RGB WS2812. "
            "CONTROL 8 WS2812 RGB LEDs - đổi màu, chế độ, độ sáng, tốc độ, tắt, lưu. "
            "GỌI KHI: người dùng nói 'đổi màu led', 'bật đèn đỏ/xanh/vàng/trắng/đen/tím/hồng', 'led cầu vồng', "
            "'đèn nhấp nháy', 'tắt đèn led', 'giảm độ sáng', 'lưu cài đặt led', 'chế độ thở'... "
            "WHEN TO CALL: 'change led color', 'red/green/blue/white/black light', 'rainbow led', 'turn off led'... "
            "TẤT CẢ THAM SỐ LÀ TÙY CHỌN - chỉ cung cấp những gì user muốn thay đổi. "
            "COLORS: đỏ(255,0,0), xanh lá(0,255,0), xanh dương(0,0,255), vàng(255,255,0), trắng(255,255,255), đen/tắt(0,0,0), tím(128,0,255), hồng(255,105,180), cam(255,165,0). "
            "MODES: off/tắt, solid/cố định, rainbow/cầu vồng, breathing/thở, chase/chạy, blink/nháy. "
            "If action='save', current settings will be saved to memory.",
            PropertyList({
                Property("red", kPropertyTypeInteger, 0, 0, 255),
                Property("green", kPropertyTypeInteger, 0, 0, 255),
                Property("blue", kPropertyTypeInteger, 0, 0, 255),
                Property("mode", kPropertyTypeString),
                Property("brightness", kPropertyTypeInteger, 0, 0, 255),
                Property("speed", kPropertyTypeInteger, 0, 0, 500),
                Property("action", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string result = "✅ LED changes: ";
                bool changed = false;
                
                // Get current state for comparison
                const led_state_t* current = kiki_led_get_state();
                
                // Set color if any RGB value is non-zero
                int r = properties["red"].value<int>();
                int g = properties["green"].value<int>();
                int b = properties["blue"].value<int>();
                if (r > 0 || g > 0 || b > 0) {
                    kiki_led_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
                    kiki_led_set_mode(LED_MODE_SOLID);
                    result += "Color=RGB(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ") ";
                    changed = true;
                }
                
                // Set mode if provided
                std::string mode_str = properties["mode"].value<std::string>();
                if (!mode_str.empty()) {
                    led_mode_t mode = LED_MODE_SOLID;
                    if (mode_str == "off" || mode_str == "tắt") {
                        mode = LED_MODE_OFF;
                    } else if (mode_str == "solid" || mode_str == "cố định") {
                        mode = LED_MODE_SOLID;
                    } else if (mode_str == "rainbow" || mode_str == "cầu vồng") {
                        mode = LED_MODE_RAINBOW;
                    } else if (mode_str == "breathing" || mode_str == "thở") {
                        mode = LED_MODE_BREATHING;
                    } else if (mode_str == "chase" || mode_str == "chạy") {
                        mode = LED_MODE_CHASE;
                    } else if (mode_str == "blink" || mode_str == "nháy") {
                        mode = LED_MODE_BLINK;
                    }
                    kiki_led_set_mode(mode);
                    result += "Mode=" + mode_str + " ";
                    changed = true;
                }
                
                // Set brightness if provided (non-zero)
                int brightness = properties["brightness"].value<int>();
                if (brightness > 0) {
                    kiki_led_set_brightness((uint8_t)brightness);
                    int percent = (brightness * 100) / 255;
                    result += "Brightness=" + std::to_string(brightness) + "(" + std::to_string(percent) + "%) ";
                    changed = true;
                }
                
                // Set speed if provided (non-zero, at least 10ms)
                int speed = properties["speed"].value<int>();
                if (speed >= 10) {
                    kiki_led_set_speed((uint16_t)speed);
                    result += "Speed=" + std::to_string(speed) + "ms ";
                    changed = true;
                }
                
                // Apply changes
                if (changed) {
                    kiki_led_update();
                }
                
                // Save if requested
                std::string action = properties["action"].value<std::string>();
                if (action == "save") {
                    kiki_led_save_to_nvs();
                    result += "+ Saved to memory!";
                }
                
                if (!changed && action != "save") {
                    return std::string("⚠️ No LED parameters provided. Use red/green/blue for color, mode for effect, brightness, speed, or action='save'");
                }
                
                return result;
            });
        
        // Tool: Get LED state
        mcp_server.AddTool(
            "self.led.state",
            "[Kiki Robot] ℹ️ KIỂM TRA TRẠNG THÁI LED / CHECK LED STATE. "
            "Trả về màu sắc, độ sáng, chế độ hiệu ứng của đèn LED. "
            "GỌI KHI: người dùng hỏi 'led màu gì', 'đèn đang bật không', 'trạng thái đèn led', 'kiểm tra led'... "
            "WHEN TO CALL: 'what color is led', 'check led status', 'led info', 'is led on'...",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                const led_state_t* state = kiki_led_get_state();
                std::string mode_name;
                
                switch (state->mode) {
                    case LED_MODE_OFF: mode_name = "Off"; break;
                    case LED_MODE_SOLID: mode_name = "Solid"; break;
                    case LED_MODE_RAINBOW: mode_name = "Rainbow"; break;
                    case LED_MODE_BREATHING: mode_name = "Breathing"; break;
                    case LED_MODE_CHASE: mode_name = "Chase"; break;
                    case LED_MODE_BLINK: mode_name = "Blink"; break;
                    default: mode_name = "Unknown"; break;
                }
                
                return std::string("{\"success\": true, \"color\": {\"r\": ") + std::to_string(state->r) + 
                       ", \"g\": " + std::to_string(state->g) + ", \"b\": " + std::to_string(state->b) +
                       "}, \"brightness\": " + std::to_string(state->brightness) +
                       ", \"mode\": \"" + mode_name + "\", \"speed\": " + std::to_string(state->speed) +
                       ", \"description\": \"LED is in " + mode_name + " mode with color RGB(" + 
                       std::to_string(state->r) + "," + std::to_string(state->g) + "," + std::to_string(state->b) +
                       "), brightness " + std::to_string((state->brightness * 100) / 255) + "%\"}";
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

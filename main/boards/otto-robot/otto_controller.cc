/*
    Otto机器人控制器 - MCP协议版本
*/

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "config.h"
#include "mcp_server.h"
#include "otto_movements.h"
#include "sdkconfig.h"
#include "settings.h"

// Forward declarations for web server control
extern "C" {
    esp_err_t otto_start_webserver(void);
    esp_err_t otto_stop_webserver(void);
}

#define TAG "OttoController"
#define ACTION_DOG_WAG_TAIL 22

class OttoController {
private:
    Otto otto_;
    TaskHandle_t action_task_handle_ = nullptr;
    QueueHandle_t action_queue_;
    bool is_action_in_progress_ = false;
    int current_action_type_ = 0;  // Track current action type for completion reporting
    // Idle management
    // Accumulated idle time in milliseconds (we increment by LOOP_IDLE_INCREMENT_MS each idle cycle)
    int idle_no_action_ticks_ = 0;    // milliseconds without actions
    static constexpr int64_t IDLE_TIMEOUT_MS = 3600000; // 1 hour = 60 * 60 * 1000 ms
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
    ACTION_DOG_TOILET = 29   // New: Toilet squat pose
    };

    static void ActionTask(void* arg) {
        OttoController* controller = static_cast<OttoController*>(arg);
        OttoActionParams params;
        
        ESP_LOGI(TAG, "🚀 ActionTask started! Attaching servos...");
        controller->otto_.AttachServos();
        ESP_LOGI(TAG, "✅ Servos attached successfully");

        while (true) {
            if (xQueueReceive(controller->action_queue_, &params, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ESP_LOGI(TAG, "⚡ Executing action: type=%d, steps=%d, speed=%d", 
                         params.action_type, params.steps, params.speed);
                controller->is_action_in_progress_ = true;
                controller->current_action_type_ = params.action_type;  // Track current action
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
                        ESP_LOGI(TAG, "💪 DogPushup: pushups=%d, speed=%d", params.steps, params.speed);
                        controller->otto_.DogPushup(params.steps, params.speed);
                        break;
                    
                    case ACTION_DOG_BALANCE:
                        ESP_LOGI(TAG, "⚖️ DogBalance: duration=%d ms, speed=%d", params.steps, params.speed);
                        controller->otto_.DogBalance(params.steps, params.speed);
                        break;
                    case ACTION_DOG_TOILET:
                        ESP_LOGI(TAG, "🚽 DogToilet: hold=%d ms, speed=%d", params.steps, params.speed);
                        controller->otto_.DogToilet(params.steps, params.speed);
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
                
                // Report action completion to MCP server
                controller->ReportActionCompletion(controller->current_action_type_, "completed");
                controller->current_action_type_ = 0;  // Reset
                
                vTaskDelay(pdMS_TO_TICKS(20));
            } else {
                // No action received within the polling timeout -> accumulate idle time
                controller->idle_no_action_ticks_ += LOOP_IDLE_INCREMENT_MS;

                // Periodic progress log every 5 minutes (300000 ms)
                if (!controller->idle_mode_ && (controller->idle_no_action_ticks_ % 300000) == 0) {
                    int minutes = controller->idle_no_action_ticks_ / 60000;
                    float percent = (controller->idle_no_action_ticks_ * 100.0f) / IDLE_TIMEOUT_MS;
                    ESP_LOGI(TAG, "⌛ Idle for %d min (%.1f%% of 60 min timeout)", minutes, percent);
                }

                // Enter idle (power save) mode after 1 hour without actions
                if (!controller->idle_mode_ && controller->idle_no_action_ticks_ >= IDLE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "🛌 Idle timeout reached (1h). Entering power save: lying down, turning off display, stopping web server.");
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
        
        BaseType_t result = xQueueSend(action_queue_, &params, portMAX_DELAY);
        if (result == pdTRUE) {
            ESP_LOGI(TAG, "✅ Action queued successfully. Queue space remaining: %d", 
                     uxQueueSpacesAvailable(action_queue_));
            StartActionTaskIfNeeded();
        } else {
            ESP_LOGE(TAG, "❌ Failed to queue action! Queue full or error.");
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

public:
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

    void ReportActionCompletion(int action_type, const std::string& status) {
        // Convert action type to readable string
        std::string action_name;
        switch (action_type) {
            case ACTION_DOG_WALK: action_name = "walk"; break;
            case ACTION_DOG_WALK_BACK: action_name = "walk_back"; break;
            case ACTION_DOG_TURN_LEFT: action_name = "turn_left"; break;
            case ACTION_DOG_TURN_RIGHT: action_name = "turn_right"; break;
            case ACTION_DOG_SIT_DOWN: action_name = "sit_down"; break;
            case ACTION_DOG_LIE_DOWN: action_name = "lie_down"; break;
            case ACTION_DOG_JUMP: action_name = "jump"; break;
            case ACTION_DOG_BOW: action_name = "bow"; break;
            case ACTION_DOG_DANCE: action_name = "dance"; break;
            case ACTION_DOG_WAVE_RIGHT_FOOT: action_name = "wave_right_foot"; break;
            case ACTION_DOG_DANCE_4_FEET: action_name = "dance_4_feet"; break;
            case ACTION_DOG_SWING: action_name = "swing"; break;
            case ACTION_DOG_STRETCH: action_name = "stretch"; break;
            case ACTION_DOG_SCRATCH: action_name = "scratch"; break;
            case ACTION_DOG_WAG_TAIL: action_name = "wag_tail"; break;
            case ACTION_DOG_ROLL_OVER: action_name = "roll_over"; break;
            case ACTION_DOG_PLAY_DEAD: action_name = "play_dead"; break;
            case ACTION_DOG_SHAKE_PAW: action_name = "shake_paw"; break;
            case ACTION_DOG_SIDESTEP: action_name = "sidestep"; break;
            case ACTION_DOG_PUSHUP: action_name = "pushup"; break;
            case ACTION_DOG_JUMP_HAPPY: action_name = "jump_happy"; break;
            case ACTION_DOG_STAND_UP: action_name = "stand_up"; break;
            case ACTION_HOME: action_name = "home"; break;
            case ACTION_DELAY: action_name = "delay"; break;
            default: action_name = "unknown"; break;
        }

        // Send MCP notification to server
        std::string payload = R"({
            "jsonrpc": "2.0",
            "method": "notifications/action_completed",
            "params": {
                "action_type": ")" + action_name + R"(",
                "status": ")" + status + R"("
            }
        })";
        
        // Send via Application (will queue if protocol not ready)
        Application::GetInstance().SendMcpMessage(payload);
        ESP_LOGI(TAG, "📢 Reported action completion: %s (%s)", action_name.c_str(), status.c_str());
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
                               try {
                                   int steps = properties["steps"].value<int>();
                                   int speed = properties["speed"].value<int>();
                                   ESP_LOGI(TAG, "🐕 MCP walk_backward called: steps=%d, speed=%d", steps, speed);
                                   
                                   // Execute movement
                                   otto_.DogWalkBack(steps, speed);
                                   otto_.WagTail(3, 100);
                                   
                                   ESP_LOGI(TAG, "✅ Walk backward completed successfully");
                                   return "Walked backward " + std::to_string(steps) + " steps at " + std::to_string(speed) + "ms speed";
                               } catch (const std::exception& e) {
                                   ESP_LOGE(TAG, "❌ Walk backward failed: %s", e.what());
                                   throw;
                               }
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
                           "Example: 'Otto, do pushups!' or 'Exercise time!' or 'Chống đẩy đi!' or 'Tập thể dục!'",
                           PropertyList({Property("pushups", kPropertyTypeInteger, 3, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 150, 50, 300)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int pushups = properties["pushups"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "💪 Kiki is doing pushups! Strong puppy!");
                               // Set happy emoji
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.DogPushup(pushups, speed);
                               return true;
                           });

        mcp_server.AddTool("self.dog.pushup_completed",
                           "🐕✅ Report that I have finished doing pushup exercises! Call this when pushup exercise is complete.\n"
                           "Use this tool to indicate that the pushup workout has finished and I can relax or do something else.\n"
                           "Example: 'Great job on pushups!' or 'Pushup exercise completed!'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "💪✅ Otto finished pushup exercises! Great workout!");
                               // Set happy emoji to celebrate completion
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
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

        mcp_server.AddTool("self.dog.wag_tail",
                           "🐕 I wag my tail like a happy puppy showing excitement! Make me wag my tail to show I'm happy!\n"
                           "Args:\n"
                           "  wags (1-20): How many times to wag my tail\n"
                           "  speed (50-300ms): Wag speed - lower is faster, higher is slower\n"
                           "Example: 'Otto, wag your tail!' or 'Vẫy đuôi đi!' or 'Show me you're happy!'",
                           PropertyList({Property("wags", kPropertyTypeInteger, 5, 1, 20),
                                         Property("speed", kPropertyTypeInteger, 100, 50, 300)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int wags = properties["wags"].value<int>();
                               int speed = properties["speed"].value<int>();
                               ESP_LOGI(TAG, "🐕 Kiki is wagging tail %d times at speed %dms!", wags, speed);
                               // Set happy emoji when wagging tail
                               if (auto display = Board::GetInstance().GetDisplay()) {
                                   display->SetEmotion("happy");
                               }
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.WagTail(wags, speed);
                               return true;
                           });

        mcp_server.AddTool("self.show_qr",
                           "📱 I show a winking face to display QR code! Use this when user asks to show QR code, activation code, or control panel access!\n"
                           "This will display a playful winking emoji and HIDE chat message until TTS ends.\n"
                           "The emoji and hidden chat will auto-restore when TTS finishes speaking.\n"
                           "Example: 'Show me the QR code' or 'Mở mã QR' or 'Display control panel' or 'Hiển thị mã kích hoạt'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "📱 MCP QR tool called: showing winking emoji + hiding chat message");
                               auto display = Board::GetInstance().GetDisplay();
                               auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                               
                               if (otto_display) {
                                   // Hide chat message immediately
                                   otto_display->SetChatMessageHidden(true);
                                   // Enable emoji overlay mode
                                   otto_display->SetEmojiOverlayMode(true);
                                   otto_display->SetEmotion("winking");
                                   ESP_LOGI(TAG, "😉 Winking emoji set + chat message hidden");
                               } else if (display) {
                                   display->SetEmotion("winking");
                                   ESP_LOGI(TAG, "😉 Winking emoji set (fallback)");
                               }
                               
                               // NOTE: Chat message and emoji will be restored by Application when TTS ends
                               // via Application::OnChatEnd() callback
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

        // Legacy movement functions (for compatibility - prefer self.dog.* tools for newer features!)

        // System tools
        mcp_server.AddTool("self.dog.home",
                           "🐕 I stand up and return to home position like a ready puppy! Make me stand up straight!\n"
                           "This is the default standing position. Use this when user says 'stand up', 'đứng lên', 'đứng dậy', 'stand', or 'home position'.\n"
                           "Example: 'Otto, stand up!' or 'Đứng lên!' or 'Đứng dậy!' or 'Go to home position!'",
                           PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ESP_LOGI(TAG, "🐾 Kiki is standing up to home position! 🏠");
                               // FAST RESPONSE: Execute immediately like esp-hi
                               otto_.Home();
                               return true;
                           });

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

        ESP_LOGI(TAG, "🐾 Dog Robot MCP tools registered (trimmed for 32-tool limit)! 🐶");
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

    ~OttoController() {
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        vQueueDelete(action_queue_);
    }
};

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
}

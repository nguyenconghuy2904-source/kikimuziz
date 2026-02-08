#include "otto_webserver.h"
#include "mcp_server.h"
#include "application.h"
#include "otto_emoji_display.h"
#include "board.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "boards/kiki/config.h"  // For DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY
#include "boards/kiki/kiki_led_control.h"  // For LED control
#include <cJSON.h>
#include <stdio.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>  // For heap_caps_malloc

// TAG used by both C and C++ code
static const char *TAG = "OttoWeb";

extern "C" {

// Global variables
bool webserver_enabled = false;
static httpd_handle_t server = NULL;
static int s_retry_num = 0;

// Action memory slots - 3 slots to save and replay action sequences
struct ActionSlot {
    char actions[512];  // Action sequence: "walk,3,150;sit,1,500;bow,1,200"
    char emotion[32];   // Associated emotion/GIF name
    bool used;          // Whether this slot has data
};
static ActionSlot memory_slots[3] = {
    {"", "", false},
    {"", "", false},
    {"", "", false}
};

// NVS functions for persistent storage of memory slots
static void save_memory_slots_to_nvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("otto_slots", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for memory slots: %s", esp_err_to_name(err));
        return;
    }
    
    for (int i = 0; i < 3; i++) {
        char key_actions[16], key_emotion[16], key_used[16];
        snprintf(key_actions, sizeof(key_actions), "slot%d_act", i);
        snprintf(key_emotion, sizeof(key_emotion), "slot%d_emo", i);
        snprintf(key_used, sizeof(key_used), "slot%d_used", i);
        
        nvs_set_str(nvs_handle, key_actions, memory_slots[i].actions);
        nvs_set_str(nvs_handle, key_emotion, memory_slots[i].emotion);
        nvs_set_u8(nvs_handle, key_used, memory_slots[i].used ? 1 : 0);
    }
    
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "💾 Saved memory slots to NVS");
}

static void load_memory_slots_from_nvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("otto_slots", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved memory slots in NVS (first boot?)");
        return;
    }
    
    for (int i = 0; i < 3; i++) {
        char key_actions[16], key_emotion[16], key_used[16];
        snprintf(key_actions, sizeof(key_actions), "slot%d_act", i);
        snprintf(key_emotion, sizeof(key_emotion), "slot%d_emo", i);
        snprintf(key_used, sizeof(key_used), "slot%d_used", i);
        
        size_t len_actions = sizeof(memory_slots[i].actions);
        size_t len_emotion = sizeof(memory_slots[i].emotion);
        uint8_t used = 0;
        
        if (nvs_get_str(nvs_handle, key_actions, memory_slots[i].actions, &len_actions) == ESP_OK &&
            nvs_get_str(nvs_handle, key_emotion, memory_slots[i].emotion, &len_emotion) == ESP_OK &&
            nvs_get_u8(nvs_handle, key_used, &used) == ESP_OK) {
            memory_slots[i].used = (used == 1);
            ESP_LOGI(TAG, "📂 Loaded slot %d from NVS: %s", i+1, memory_slots[i].used ? "has data" : "empty");
        }
    }
    
    nvs_close(nvs_handle);
}

// Static buffer pool cho draw handler - tránh heap fragmentation
// Buffer được allocate khi cần, không phải static để tiết kiệm RAM
static struct {
    uint8_t* buffer;  // Allocate on first use (PSRAM if available)
    bool in_use;
    bool initialized;
    SemaphoreHandle_t mutex;
} draw_buffer_pool = {
    .buffer = NULL,
    .in_use = false,
    .initialized = false,
    .mutex = NULL
};

// Initialize draw buffer on first use (lazy allocation to save RAM at startup)
static bool init_draw_buffer() {
    if (draw_buffer_pool.initialized) return true;
    if (draw_buffer_pool.mutex == NULL) {
        draw_buffer_pool.mutex = xSemaphoreCreateMutex();
    }
    // Allocate in PSRAM if available, otherwise internal
    draw_buffer_pool.buffer = (uint8_t*)heap_caps_malloc(120000, MALLOC_CAP_SPIRAM);
    if (!draw_buffer_pool.buffer) {
        draw_buffer_pool.buffer = (uint8_t*)heap_caps_malloc(120000, MALLOC_CAP_INTERNAL);
    }
    if (draw_buffer_pool.buffer) {
        draw_buffer_pool.initialized = true;
        ESP_LOGI(TAG, "🎨 Draw buffer allocated: 120KB");
        return true;
    }
    ESP_LOGE(TAG, "❌ Failed to allocate draw buffer");
    return false;
}

// Cleanup draw buffer when webserver stops
static void cleanup_draw_buffer() {
    if (draw_buffer_pool.mutex) {
        xSemaphoreTake(draw_buffer_pool.mutex, portMAX_DELAY);
    }
    if (draw_buffer_pool.buffer) {
        free(draw_buffer_pool.buffer);
        draw_buffer_pool.buffer = NULL;
        draw_buffer_pool.initialized = false;
        ESP_LOGI(TAG, "🧹 Draw buffer freed: 120KB");
    }
    if (draw_buffer_pool.mutex) {
        xSemaphoreGive(draw_buffer_pool.mutex);
    }
}

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

// Webserver auto-stop timer (5 minutes)
static TimerHandle_t webserver_auto_stop_timer = NULL;
static const uint32_t WEBSERVER_AUTO_STOP_DELAY_MS = 5 * 60 * 1000;  // 5 minutes
static bool webserver_manual_mode = false;  // Flag to prevent auto-start, only manual start

// Speed multiplier (50 = 50% = faster, 100 = normal, 200 = 200% = slower)
static int speed_multiplier = 100;  // Default 100%

// Schedule message timer - hẹn giờ gửi tin nhắn hoặc báo thức
static TimerHandle_t schedule_message_timer = NULL;
static bool schedule_active = false;
static char scheduled_message[512] = "";  // Tin nhắn đã hẹn giờ
static uint32_t schedule_remaining_seconds = 0;  // Số giây còn lại
static int64_t schedule_target_timestamp = 0;  // Unix timestamp khi đến giờ gửi
static char schedule_mode[16] = "alarm";  // "alarm" hoặc "message"
static int scheduled_action_slot = 0;  // 0=none, 1-3=memory slot to play when triggered
static TaskHandle_t alarm_task_handle = NULL;  // Prevent multiple alarm tasks

// Power save idle timeout (configurable from web UI)
static uint32_t idle_timeout_minutes = 60;  // Default 60 minutes (1 hour)

// Timer callback for schedule message - countdown every second
void schedule_countdown_callback(TimerHandle_t xTimer);

// Save schedule to NVS for persistence across reboots
void save_schedule_to_nvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open("schedule", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, "message", scheduled_message);
        nvs_set_i64(nvs_handle, "target_ts", schedule_target_timestamp);
        nvs_set_i8(nvs_handle, "active", schedule_active ? 1 : 0);
        nvs_set_str(nvs_handle, "mode", schedule_mode);
        nvs_set_i8(nvs_handle, "action_slot", scheduled_action_slot);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 Schedule saved to NVS: mode='%s', msg='%s', target=%lld, active=%d, slot=%d", 
                 schedule_mode, scheduled_message, schedule_target_timestamp, schedule_active, scheduled_action_slot);
    }
}

// Clear schedule from NVS
void clear_schedule_from_nvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open("schedule", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, "message");
        nvs_erase_key(nvs_handle, "target_ts");
        nvs_erase_key(nvs_handle, "active");
        nvs_erase_key(nvs_handle, "mode");
        nvs_erase_key(nvs_handle, "action_slot");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "🗑️ Schedule cleared from NVS");
    }
}

// Load schedule from NVS and restore timer if pending
void load_schedule_from_nvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open("schedule", NVS_READONLY, &nvs_handle) == ESP_OK) {
        int8_t active = 0;
        nvs_get_i8(nvs_handle, "active", &active);
        
        if (active == 1) {
            size_t msg_len = sizeof(scheduled_message);
            nvs_get_str(nvs_handle, "message", scheduled_message, &msg_len);
            nvs_get_i64(nvs_handle, "target_ts", &schedule_target_timestamp);
            
            // Load mode
            size_t mode_len = sizeof(schedule_mode);
            if (nvs_get_str(nvs_handle, "mode", schedule_mode, &mode_len) != ESP_OK) {
                strcpy(schedule_mode, "alarm");  // Default to alarm if not set
            }
            
            // Load action slot
            int8_t slot = 0;
            if (nvs_get_i8(nvs_handle, "action_slot", &slot) == ESP_OK) {
                scheduled_action_slot = slot;
            } else {
                scheduled_action_slot = 0;
            }
            
            // Calculate remaining seconds based on current time
            time_t now;
            time(&now);
            int64_t remaining = schedule_target_timestamp - (int64_t)now;
            
            ESP_LOGI(TAG, "📖 Loaded schedule from NVS: mode='%s', msg='%s', target=%lld, now=%lld, remaining=%lld", 
                     schedule_mode, scheduled_message, schedule_target_timestamp, (int64_t)now, remaining);
            
            if (remaining > 0) {
                // Schedule still pending - restore it
                schedule_remaining_seconds = (uint32_t)remaining;
                schedule_active = true;
                
                // Create and start timer
                if (schedule_message_timer == NULL) {
                    schedule_message_timer = xTimerCreate(
                        "schedule_msg_timer",
                        pdMS_TO_TICKS(1000),  // 1 second period
                        pdTRUE,              // Auto-reload
                        NULL,
                        schedule_countdown_callback
                    );
                }
                
                if (schedule_message_timer != NULL) {
                    xTimerStart(schedule_message_timer, 0);
                    ESP_LOGI(TAG, "⏰ Restored schedule timer: %lu seconds remaining", schedule_remaining_seconds);
                }
            } else {
                // Schedule expired while device was off - clear it
                ESP_LOGI(TAG, "⚠️ Schedule expired while device was off - clearing");
                clear_schedule_from_nvs();
                scheduled_message[0] = '\0';
                schedule_target_timestamp = 0;
            }
        }
        nvs_close(nvs_handle);
    }
}

// Set alarm from MCP voice command
// seconds_from_now: số giây từ bây giờ đến khi báo thức
// mode: "alarm" hoặc "message"
// message: tin nhắn (chỉ dùng cho mode "message")
extern "C" bool set_alarm_from_mcp(int seconds_from_now, const char* mode, const char* message) {
    ESP_LOGI(TAG, "⏰ MCP Set Alarm: seconds=%d, mode=%s, msg=%s", seconds_from_now, mode, message ? message : "(null)");
    
    if (seconds_from_now <= 0 || seconds_from_now > 86400) {  // Max 24 hours
        ESP_LOGE(TAG, "❌ Invalid seconds: %d (must be 1-86400)", seconds_from_now);
        return false;
    }
    
    // Set schedule parameters
    schedule_remaining_seconds = (uint32_t)seconds_from_now;
    
    // Calculate target timestamp
    time_t now;
    time(&now);
    schedule_target_timestamp = (int64_t)now + seconds_from_now;
    
    // Set mode
    if (mode && strlen(mode) > 0) {
        strncpy(schedule_mode, mode, sizeof(schedule_mode) - 1);
        schedule_mode[sizeof(schedule_mode) - 1] = '\0';
    } else {
        strcpy(schedule_mode, "alarm");
    }
    
    // Set message (only used in message mode)
    if (message && strlen(message) > 0) {
        size_t msg_len = strlen(message);
        if (msg_len >= sizeof(scheduled_message)) {
            ESP_LOGW(TAG, "⚠️ Message too long (%zu bytes), truncating to %zu", msg_len, sizeof(scheduled_message) - 1);
        }
        strncpy(scheduled_message, message, sizeof(scheduled_message) - 1);
        scheduled_message[sizeof(scheduled_message) - 1] = '\0';
    } else {
        scheduled_message[0] = '\0';
    }
    
    schedule_active = true;
    
    // Save to NVS for persistence
    save_schedule_to_nvs();
    
    // Create or start timer
    if (schedule_message_timer == NULL) {
        schedule_message_timer = xTimerCreate(
            "schedule_msg_timer",
            pdMS_TO_TICKS(1000),  // 1 second period
            pdTRUE,              // Auto-reload
            NULL,
            schedule_countdown_callback
        );
    }
    
    if (schedule_message_timer != NULL) {
        xTimerStop(schedule_message_timer, 0);
        xTimerStart(schedule_message_timer, 0);
        ESP_LOGI(TAG, "✅ Alarm set successfully! Will trigger in %d seconds", seconds_from_now);
        return true;
    }
    
    ESP_LOGE(TAG, "❌ Failed to create timer");
    return false;
}

// Cancel alarm from MCP voice command
extern "C" bool cancel_alarm_from_mcp() {
    ESP_LOGI(TAG, "⏰ MCP Cancel Alarm");
    
    if (!schedule_active) {
        ESP_LOGW(TAG, "⚠️ No active alarm to cancel");
        return false;
    }
    
    schedule_active = false;
    schedule_remaining_seconds = 0;
    schedule_target_timestamp = 0;
    scheduled_message[0] = '\0';
    scheduled_action_slot = 0;
    
    if (schedule_message_timer != NULL) {
        xTimerStop(schedule_message_timer, 0);
    }
    
    clear_schedule_from_nvs();
    ESP_LOGI(TAG, "✅ Alarm cancelled!");
    return true;
}

// Get remaining alarm time in seconds
extern "C" int get_alarm_remaining_seconds() {
    if (!schedule_active) return -1;
    return (int)schedule_remaining_seconds;
}

// Timer callback for webserver auto-stop
void webserver_auto_stop_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "⏱️ Webserver auto-stop timeout (5 min) - stopping webserver");
    otto_stop_webserver();
    webserver_manual_mode = false;  // Reset to allow future manual starts
}

// Helper function to reset auto-stop timer when webserver is used
void webserver_reset_auto_stop_timer() {
    if (webserver_auto_stop_timer != NULL && server != NULL) {
        // Reset the timer - restart 5-minute countdown
        xTimerReset(webserver_auto_stop_timer, 0);
    }
}

// Schedule message timer callback - đếm ngược và gửi tin nhắn khi hết giờ
void schedule_countdown_callback(TimerHandle_t xTimer) {
    if (!schedule_active) {
        return;
    }
    
    schedule_remaining_seconds--;
    
    // Only log every 10 seconds to reduce overhead
    if (schedule_remaining_seconds % 10 == 0) {
        ESP_LOGI(TAG, "⏰ Countdown: %lu sec", schedule_remaining_seconds);
    }
    
    if (schedule_remaining_seconds == 0) {
        // Hết giờ
        bool is_alarm_mode = (strcmp(schedule_mode, "alarm") == 0);
        ESP_LOGI(TAG, "⏰ Time reached! Alarm=%d", is_alarm_mode);
        schedule_active = false;
        schedule_target_timestamp = 0;
        
        // Clear from NVS since schedule is completed
        clear_schedule_from_nvs();
        
        // Stop the timer
        if (schedule_message_timer != NULL) {
            xTimerStop(schedule_message_timer, 0);
        }
        
        // Prevent multiple alarm tasks running simultaneously
        if (alarm_task_handle != NULL) {
            ESP_LOGW(TAG, "⚠️ Previous alarm task still running, deleting...");
            vTaskDelete(alarm_task_handle);
            alarm_task_handle = NULL;
        }
        
        // Allocate task parameters on heap to avoid race condition with static variables
        struct AlarmTaskParams {
            char msg[256];  // Reduced size - 256 chars should be enough
            bool is_alarm_mode;
            int action_slot;  // Memory slot to play
            char slot_actions[512];  // Copy actions here to avoid accessing memory_slots from task
            char slot_emotion[32];
            bool slot_valid;
        };
        AlarmTaskParams* params = new AlarmTaskParams();
        strncpy(params->msg, scheduled_message, sizeof(params->msg) - 1);
        params->msg[sizeof(params->msg) - 1] = '\0';
        params->is_alarm_mode = is_alarm_mode;
        params->action_slot = scheduled_action_slot;
        
        // Copy slot data to params if slot is selected (avoid accessing memory_slots from task)
        if (scheduled_action_slot >= 1 && scheduled_action_slot <= 3) {
            int idx = scheduled_action_slot - 1;
            if (memory_slots[idx].used && strlen(memory_slots[idx].actions) > 0) {
                strncpy(params->slot_actions, memory_slots[idx].actions, sizeof(params->slot_actions) - 1);
                params->slot_actions[sizeof(params->slot_actions) - 1] = '\0';
                strncpy(params->slot_emotion, memory_slots[idx].emotion, sizeof(params->slot_emotion) - 1);
                params->slot_emotion[sizeof(params->slot_emotion) - 1] = '\0';
                params->slot_valid = true;
            } else {
                params->slot_valid = false;
            }
        } else {
            params->slot_valid = false;
        }
        
        // Create alarm task with larger stack to prevent overflow
        BaseType_t task_result = xTaskCreate([](void* param) {
            AlarmTaskParams* p = static_cast<AlarmTaskParams*>(param);
            
            ESP_LOGI(TAG, "🔔 Alarm task started! Mode: %s, slot: %d", 
                     p->is_alarm_mode ? "ALARM" : "MESSAGE", p->action_slot);
            
            // Play alarm sound - simpler loop
            int ring_count = p->is_alarm_mode ? 3 : 1;  // Reduced ring count
            
            for (int i = 0; i < ring_count; i++) {
                ESP_LOGI(TAG, "🔔 Ring %d/%d", i + 1, ring_count);
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(600));
            }
            
            // If message mode, wake robot and send message
            if (!p->is_alarm_mode && strlen(p->msg) > 0) {
                ESP_LOGI(TAG, "💬 Message mode: Waking up robot...");
                
                Application::GetInstance().Schedule([]() {
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateIdle) {
                        app.ToggleChatState();
                    }
                });
                
                vTaskDelay(pdMS_TO_TICKS(2000));
                
                // Copy message to std::string for lambda capture
                std::string msg_str(p->msg);
                Application::GetInstance().Schedule([msg_str]() {
                    ESP_LOGI(TAG, "💬 Sending: %s", msg_str.c_str());
                    Application::GetInstance().SendSttMessage(msg_str);
                });
                
                // Wait for message to be sent before playing actions
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
            // Play action from memory slot if selected and valid
            if (p->action_slot >= 1 && p->action_slot <= 3 && p->slot_valid) {
                ESP_LOGI(TAG, "🎭 Playing action from slot %d", p->action_slot);
                
                // Use simple parsing without large stack buffers - work directly on params
                char* actions_ptr = p->slot_actions;
                char* saveptr = NULL;
                char* token = strtok_r(actions_ptr, ";", &saveptr);
                int action_count = 0;
                
                while (token != NULL && action_count < 20) {  // Limit max actions
                    char action[32] = {0};
                    char emoji[24] = {0};
                    int p1 = 0, p2 = 0;
                    
                    // Parse token - simplified
                    if (sscanf(token, "%31[^,],%d,%d,%23s", action, &p1, &p2, emoji) < 3) {
                        sscanf(token, "%31[^,],%d,%d", action, &p1, &p2);
                    }
                    
                    if (strlen(action) > 0) {
                        // Skip emoji-only actions
                        if (strcmp(action, "emoji") != 0) {
                            // Apply speed multiplier
                            int adjusted_speed = p2;
                            if (p2 > 0) {
                                adjusted_speed = (p2 * speed_multiplier) / 100;
                                if (adjusted_speed < 10) adjusted_speed = 10;
                            }
                            
                            ESP_LOGI(TAG, "▶️ Action: %s (p1:%d, p2:%d)", action, p1, adjusted_speed);
                            otto_execute_web_action(action, p1, adjusted_speed);
                            action_count++;
                            
                            // Delay between actions
                            vTaskDelay(pdMS_TO_TICKS(150));
                        } else if (strlen(emoji) > 0) {
                            // Set emoji via Schedule to avoid direct display access
                            std::string emo_str(emoji);
                            Application::GetInstance().Schedule([emo_str]() {
                                auto display = Board::GetInstance().GetDisplay();
                                if (display) {
                                    display->SetEmotion(emo_str.c_str());
                                }
                            });
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                    }
                    
                    token = strtok_r(NULL, ";", &saveptr);
                }
                
                ESP_LOGI(TAG, "✅ Completed %d actions from slot %d", action_count, p->action_slot);
            }
            
            // Cleanup and exit
            delete p;
            alarm_task_handle = NULL;
            vTaskDelete(NULL);
        }, "alarm_task", 4096, params, 5, &alarm_task_handle);  // Increased stack to 4096
        
        // Check if task creation succeeded, cleanup if failed
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "❌ Failed to create alarm task!");
            delete params;  // Prevent memory leak
            alarm_task_handle = NULL;
        }
        
        // Clear the scheduled message
        scheduled_message[0] = '\0';
    }
}

// Load speed multiplier from NVS
void load_speed_from_nvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        int32_t speed = 100;
        if (nvs_get_i32(nvs_handle, "speed_mult", &speed) == ESP_OK) {
            speed_multiplier = (int)speed;
            ESP_LOGI(TAG, "📐 Loaded speed multiplier from NVS: %d%%", speed_multiplier);
        }
        nvs_close(nvs_handle);
    }
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
        ESP_LOGI(TAG, "🌐 Otto Web Controller available at: http://" IPSTR, IP2STR(&event->ip_info.ip));
        
        // Start Otto web server automatically
        if (server == NULL) {
            otto_start_webserver();
        }
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

// WiFi event handler function - DISABLED, using esp-wifi-connect component instead
// This prevents duplicate WiFi initialization which causes ESP_ERR_NO_MEM
void otto_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    // Removed WiFi connection logic - handled by esp-wifi-connect component
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "\033[1;33m🌟 WifiStation: Got IP: " IPSTR "\033[0m", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        
        // DO NOT auto-start webserver - only start when user requests it
        ESP_LOGI(TAG, "📱 Web control panel available - say 'mở bảng điều khiển' to start");
    }
}

// Check WiFi connection but DO NOT auto-start webserver
// Webserver only starts when user explicitly requests it via voice command
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
                ESP_LOGI(TAG, "📱 Web control panel available at: http://" IPSTR, IP2STR(&ip_info.ip));
                ESP_LOGI(TAG, "💬 Say 'mở bảng điều khiển' to start the web server");
                
                // DO NOT auto-start - wait for user voice command
                return ESP_OK;
            }
        }
    } else {
        ESP_LOGI(TAG, "WiFi not connected yet, web control will be available after connection");
    }
    
    return ESP_OK;
}

// Original WiFi initialization (for standalone mode if needed)
// DISABLED: WiFi init now handled by esp-wifi-connect component
// This function is kept for compatibility but does nothing
esp_err_t otto_wifi_init_sta(void) {
    ESP_LOGI(TAG, "⚠️ otto_wifi_init_sta() called but DISABLED - WiFi managed by esp-wifi-connect component");
    return ESP_OK;
}

// Send main control page HTML
void send_otto_control_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    
    // Modern responsive HTML with Otto Robot theme
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>");
    httpd_resp_sendstr_chunk(req, "<title>Kiki Control - miniZ</title>");
    
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
    // Auto pose config styling - 3 columns for mobile
    httpd_resp_sendstr_chunk(req, ".pose-config { background: #f8f8f8; border: 2px solid #000; border-radius: 10px; padding: 12px; margin: 10px 0; }");
    httpd_resp_sendstr_chunk(req, ".pose-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; }");
    httpd_resp_sendstr_chunk(req, ".pose-item { display: flex; align-items: center; gap: 4px; padding: 6px; background: white; border-radius: 6px; border: 1px solid #ddd; font-size: 12px; }");
    httpd_resp_sendstr_chunk(req, ".pose-item input[type='checkbox'] { width: 16px; height: 16px; cursor: pointer; flex-shrink: 0; }");
    httpd_resp_sendstr_chunk(req, ".pose-item label { cursor: pointer; font-weight: 500; font-size: 11px; line-height: 1.2; }");
    httpd_resp_sendstr_chunk(req, ".time-input { width: 70px; padding: 5px; border: 2px solid #000; border-radius: 5px; font-weight: bold; text-align: center; }");
    
    // Compact fun actions grid
    httpd_resp_sendstr_chunk(req, ".fun-actions { margin-top: 15px; }");
    httpd_resp_sendstr_chunk(req, ".action-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; } @media (min-width: 768px) { .action-grid { grid-template-columns: repeat(4, 1fr); gap: 10px; } }");
    
    // Compact emoji sections
    httpd_resp_sendstr_chunk(req, ".emoji-section, .emoji-mode-section { margin-top: 15px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }");
    httpd_resp_sendstr_chunk(req, ".mode-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 12px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-btn { background: #fff8e1; border: 2px solid #ff6f00; color: #e65100; padding: 10px; font-size: 13px; }");
    httpd_resp_sendstr_chunk(req, ".emoji-btn:hover { background: #ffecb3; border-color: #e65100; }");
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
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab' onclick='showPage(2)' id='tab2'>😊 Cảm Xúc</div>");
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab' onclick='window.location.href=\"/music\"' id='tabMusic'>🎵 Nhạc</div>");
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab' onclick='showPage(4)' id='tab4'>🎨 Vẽ</div>");
    httpd_resp_sendstr_chunk(req, "<div class='nav-tab' onclick='window.location.href=\"/servo_calibration\"' id='tab3'>⚙️</div>");
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
    
    // Volume Control Section (moved to Page 1 for mobile)
    httpd_resp_sendstr_chunk(req, "<div class='volume-section' style='margin-top: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🔊 Âm Lượng</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #f8f8f8, #ffffff); border: 2px solid #000000; border-radius: 12px; padding: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000;'>🔈</span>");
    httpd_resp_sendstr_chunk(req, "<input type='range' id='volumeSlider' min='0' max='100' value='50' style='flex: 1; height: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<span id='volumeValue' style='font-weight: bold; color: #000; min-width: 40px;'>50%</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
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
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_lie_down\", 1, 4500)'>🛏️ Nằm</button>");
    // New Defend and Scratch buttons  
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_defend\", 1, 500)'>� Giả Chết</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn paw-btn' onclick='sendAction(\"dog_scratch\", 5, 50)'>🐾 Gãi Ngứa</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_wave_right_foot\", 5, 50)'>👋 Vẫy Tay</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_wag_tail\", 5, 100)'>🐕 Vẫy Đuôi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_swing\", 5, 10)'>🎯 Lắc Lư</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_stretch\", 2, 15)'>🧘 Thư Giản</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_home\", 1, 4500)'>🏠 Về Nhà</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_dance_4_feet\", 3, 200)'>🕺 Nhảy 4 Chân</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_greet\", 1, 500)'>👋 Chào Hỏi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_attack\", 1, 500)'>⚔️ Tấn Công</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_celebrate\", 1, 500)'>🎉 Ăn Mừng</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"dog_search\", 1, 500)'>🔍 Tìm Kiếm</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendAction(\"show_clock\", 10, 0)' style='background: linear-gradient(145deg, #2196f3, #42a5f5); color: white; border-color: #1976d2;'>⏰ Đồng Hồ</button>");
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
    
    // ALL EMOJI Section on Page 1 - Match server emoji list
    httpd_resp_sendstr_chunk(req, "<div class='emoji-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>😊 TẤT CẢ EMOJI</div>");
    httpd_resp_sendstr_chunk(req, "<div class='emoji-grid'>");
    // Row 1: Neutral, Happy, Laughing, Funny, Sad, Angry, Crying
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"neutral\")'>😐 Neutral</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"happy\")'>🤗 Happy</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"laughing\")'>🤣 Laughing</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"funny\")'>🥳 Funny</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"sad\")'>😔 Sad</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"angry\")'>😠 Angry</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"crying\")'>😭 Crying</button>");
    // Row 2: Loving, Embarrassed, Surprised, Shocked, Thinking, Winking, Cool
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"loving\")'>😍 Loving</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"embarrassed\")'>😳 Embarrassed</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"surprised\")'>😲 Surprised</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"shocked\")'>🤯 Shocked</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"thinking\")'>🤔 Thinking</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"winking\")'>😉 Winking</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"cool\")'>😎 Cool</button>");
    // Row 3: Relaxed, Delicious, Kiss, Confident, Sleepy, Silly, Confused
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"relaxed\")'>😌 Relaxed</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"delicious\")'>🤤 Delicious</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"kiss\")'>😘 Kiss</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"confident\")'>🤨 Confident</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"sleepy\")'>😴 Sleepy</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"silly\")'>🤪 Silly</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn emoji-btn' onclick='sendEmotion(\"confused\")'>😕 Confused</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Action Memory Slots Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section' style='margin-top: 20px;'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>💾 Lưu Và Phát Lại Hành Động</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #fff3e0, #ffffff); border: 2px solid #ff9800; border-radius: 15px; padding: 15px;'>");
    
    // Slot 1
    httpd_resp_sendstr_chunk(req, "<div style='background: #fff; border: 2px solid #4caf50; border-radius: 10px; padding: 12px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; color: #2e7d32;'>📍 Vị trí 1</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot1-status' style='font-size: 11px; color: #666;'>⚪ Chưa ghi</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 6px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='record1-btn' onclick='startRecording(1)' style='background: linear-gradient(145deg, #ff5722, #ff7043); color: white; border-color: #d84315; font-size: 12px; padding: 8px 12px;'>🔴 Bắt đầu ghi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='save1-btn' onclick='saveSlot(1)' disabled style='background: #e0e0e0; color: #999; font-size: 12px; padding: 8px 12px; cursor: not-allowed;'>💾 Lưu</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='playSlot(1)' style='background: linear-gradient(145deg, #2196f3, #42a5f5); color: white; border-color: #1565c0; font-size: 12px; padding: 8px 12px;'>▶️ Phát</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot1-info' style='margin-top: 8px; font-size: 11px; color: #666;'>📦 Chưa có dữ liệu</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Slot 2
    httpd_resp_sendstr_chunk(req, "<div style='background: #fff; border: 2px solid #2196f3; border-radius: 10px; padding: 12px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; color: #1565c0;'>📍 Vị trí 2</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot2-status' style='font-size: 11px; color: #666;'>⚪ Chưa ghi</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 6px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='record2-btn' onclick='startRecording(2)' style='background: linear-gradient(145deg, #ff5722, #ff7043); color: white; border-color: #d84315; font-size: 12px; padding: 8px 12px;'>🔴 Bắt đầu ghi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='save2-btn' onclick='saveSlot(2)' disabled style='background: #e0e0e0; color: #999; font-size: 12px; padding: 8px 12px; cursor: not-allowed;'>💾 Lưu</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='playSlot(2)' style='background: linear-gradient(145deg, #2196f3, #42a5f5); color: white; border-color: #1565c0; font-size: 12px; padding: 8px 12px;'>▶️ Phát</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot2-info' style='margin-top: 8px; font-size: 11px; color: #666;'>📦 Chưa có dữ liệu</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Slot 3
    httpd_resp_sendstr_chunk(req, "<div style='background: #fff; border: 2px solid #9c27b0; border-radius: 10px; padding: 12px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; color: #6a1b9a;'>📍 Vị trí 3</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot3-status' style='font-size: 11px; color: #666;'>⚪ Chưa ghi</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 6px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='record3-btn' onclick='startRecording(3)' style='background: linear-gradient(145deg, #ff5722, #ff7043); color: white; border-color: #d84315; font-size: 12px; padding: 8px 12px;'>🔴 Bắt đầu ghi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' id='save3-btn' onclick='saveSlot(3)' disabled style='background: #e0e0e0; color: #999; font-size: 12px; padding: 8px 12px; cursor: not-allowed;'>💾 Lưu</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='playSlot(3)' style='background: linear-gradient(145deg, #2196f3, #42a5f5); color: white; border-color: #1565c0; font-size: 12px; padding: 8px 12px;'>▶️ Phát</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div id='slot3-info' style='margin-top: 8px; font-size: 11px; color: #666;'>📦 Chưa có dữ liệu</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Instructions
    httpd_resp_sendstr_chunk(req, "<div style='margin-top: 12px; padding: 10px; background: #e3f2fd; border-radius: 8px; font-size: 12px; color: #1565c0; line-height: 1.6;'>");
    httpd_resp_sendstr_chunk(req, "💡 <strong>Hướng dẫn:</strong><br>");
    httpd_resp_sendstr_chunk(req, "1️⃣ Bấm <strong>🔴 Bắt đầu ghi</strong> → Nút chuyển thành 🟢 Đang ghi...<br>");
    httpd_resp_sendstr_chunk(req, "2️⃣ Thực hiện các hành động (đi, nhảy, ngồi...) và chọn emoji<br>");
    httpd_resp_sendstr_chunk(req, "3️⃣ Bấm <strong>💾 Lưu</strong> → Ghi vào vị trí và dừng recording<br>");
    httpd_resp_sendstr_chunk(req, "4️⃣ Bấm <strong>▶️ Phát</strong> → Robot thực hiện lại toàn bộ");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // AI Chat Section - MOVED TO PAGE 1
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>💬 Chat với Kiki AI</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #e3f2fd, #ffffff); border: 2px solid #1976d2; border-radius: 15px; padding: 15px; margin-bottom: 20px;'>");
    
    // Chat history area
    httpd_resp_sendstr_chunk(req, "<div id='chat-history' style='background: #fff; border: 1px solid #ddd; border-radius: 10px; padding: 10px; height: 150px; overflow-y: auto; margin-bottom: 10px; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='color: #999; text-align: center;'>💬 Lịch sử chat sẽ hiển thị ở đây...</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Input area
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 8px; margin-bottom: 5px;'>");
    httpd_resp_sendstr_chunk(req, "<textarea id='ai_text_input' placeholder='Nhập tin nhắn cho Kiki...' rows='2' maxlength='1500' style='flex:1; padding: 10px; border: 2px solid #1976d2; border-radius: 8px; font-size: 14px; resize: none;' oninput='updateCharCount()'></textarea>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendTextToAI()' style='background: linear-gradient(145deg, #1976d2, #42a5f5); color: white; border: none; padding: 10px 20px; font-size: 16px;'>📤 Gửi</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Character counter
    httpd_resp_sendstr_chunk(req, "<div id='char-counter' style='text-align: right; font-size: 11px; color: #666; margin-bottom: 8px;'>0 / 1500 ký tự</div>");
    
    // Quick buttons
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='quickSend(\"Xin chào Kiki\")' style='font-size: 12px; padding: 6px 12px; background: #e3f2fd; border-color: #1976d2; color: #1976d2;'>👋 Xin chào</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='quickSend(\"Hôm nay thời tiết thế nào\")' style='font-size: 12px; padding: 6px 12px; background: #e3f2fd; border-color: #1976d2; color: #1976d2;'>🌤️ Thời tiết</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='quickSend(\"Kể cho tôi một câu chuyện vui\")' style='font-size: 12px; padding: 6px 12px; background: #e3f2fd; border-color: #1976d2; color: #1976d2;'>📖 Kể chuyện</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='quickSend(\"Bạn có thể làm gì\")' style='font-size: 12px; padding: 6px 12px; background: #e3f2fd; border-color: #1976d2; color: #1976d2;'>❓ Trợ giúp</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='clearChatHistory()' style='font-size: 12px; padding: 6px 12px; background: #ffebee; border-color: #f44336; color: #f44336;'>🗑️ Xóa</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Status area
    httpd_resp_sendstr_chunk(req, "<div id='ai-text-status' style='padding: 8px; background: #e8f5e9; border-radius: 6px; font-size: 13px; text-align: center; color: #2e7d32;'>✅ Sẵn sàng chat với Kiki</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Schedule Section - Hẹn giờ (Báo thức hoặc Tin nhắn)
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>⏰ Hẹn Giờ & Báo Thức</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #fff3e0, #ffffff); border: 2px solid #ff9800; border-radius: 15px; padding: 15px;'>");
    
    // Mode selection - Radio buttons
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 10px; background: #fafafa; border-radius: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #e65100;'>🎯 Chọn chế độ:</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 15px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: flex; align-items: center; gap: 8px; cursor: pointer; padding: 10px 15px; background: #fff; border: 2px solid #4caf50; border-radius: 8px; flex: 1; min-width: 140px;'>");
    httpd_resp_sendstr_chunk(req, "<input type='radio' name='schedule_mode' value='alarm' id='mode_alarm' checked onchange='updateScheduleMode()' style='width: 18px; height: 18px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-size: 14px;'>🔔 <strong>Báo thức</strong></span>");
    httpd_resp_sendstr_chunk(req, "</label>");
    httpd_resp_sendstr_chunk(req, "<label style='display: flex; align-items: center; gap: 8px; cursor: pointer; padding: 10px 15px; background: #fff; border: 2px solid #2196f3; border-radius: 8px; flex: 1; min-width: 140px;'>");
    httpd_resp_sendstr_chunk(req, "<input type='radio' name='schedule_mode' value='message' id='mode_message' onchange='updateScheduleMode()' style='width: 18px; height: 18px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-size: 14px;'>💬 <strong>Hẹn tin nhắn</strong></span>");
    httpd_resp_sendstr_chunk(req, "</label>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Message input for scheduled message (hidden by default for alarm mode)
    httpd_resp_sendstr_chunk(req, "<div id='message_input_section' style='margin-bottom: 12px; display: none;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 5px; color: #1976d2;'>📝 Tin nhắn sẽ gửi:</label>");
    httpd_resp_sendstr_chunk(req, "<textarea id='schedule_message' placeholder='Nhập tin nhắn muốn hẹn giờ gửi...' rows='2' maxlength='500' style='width: 100%; padding: 10px; border: 2px solid #2196f3; border-radius: 8px; font-size: 14px; resize: none;'></textarea>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Action slot selection dropdown
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 5px; color: #9c27b0;'>🎭 Hành động kèm theo:</label>");
    httpd_resp_sendstr_chunk(req, "<select id='schedule_action_slot' style='width: 100%; padding: 10px; border: 2px solid #9c27b0; border-radius: 8px; font-size: 14px; background: white;'>");
    httpd_resp_sendstr_chunk(req, "<option value='0'>⚪ Không chọn hành động</option>");
    httpd_resp_sendstr_chunk(req, "<option value='1'>📍 Vị trí 1</option>");
    httpd_resp_sendstr_chunk(req, "<option value='2'>📍 Vị trí 2</option>");
    httpd_resp_sendstr_chunk(req, "<option value='3'>📍 Vị trí 3</option>");
    httpd_resp_sendstr_chunk(req, "</select>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size: 12px; color: #666; margin-top: 4px;'>💡 Chọn hành động đã lưu để tự động thực hiện khi đến giờ</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Date input - ngày tháng năm
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 5px; color: #e65100;'>📅 Ngày:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='date' id='schedule_date' style='width: 100%; padding: 10px; border: 2px solid #ff9800; border-radius: 8px; font-size: 16px;'>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Time input - giờ phút
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<label style='display: block; font-weight: bold; margin-bottom: 5px; color: #e65100;'>🕐 Giờ:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='time' id='schedule_time' style='width: 100%; padding: 10px; border: 2px solid #ff9800; border-radius: 8px; font-size: 16px;'>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Quick time buttons
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setQuickSchedule(1)' style='font-size: 12px; padding: 6px 12px; background: #fff3e0; border-color: #ff9800; color: #e65100;'>+1 phút</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setQuickSchedule(5)' style='font-size: 12px; padding: 6px 12px; background: #fff3e0; border-color: #ff9800; color: #e65100;'>+5 phút</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setQuickSchedule(10)' style='font-size: 12px; padding: 6px 12px; background: #fff3e0; border-color: #ff9800; color: #e65100;'>+10 phút</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setQuickSchedule(30)' style='font-size: 12px; padding: 6px 12px; background: #fff3e0; border-color: #ff9800; color: #e65100;'>+30 phút</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setQuickSchedule(60)' style='font-size: 12px; padding: 6px 12px; background: #fff3e0; border-color: #ff9800; color: #e65100;'>+1 giờ</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Control buttons
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 10px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<button id='scheduleStartBtn' class='btn' onclick='startSchedule()' style='flex: 1; background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: none; padding: 12px; font-size: 16px; font-weight: bold;'>🔔 Đặt Báo Thức</button>");
    httpd_resp_sendstr_chunk(req, "<button id='scheduleCancelBtn' class='btn' onclick='cancelSchedule()' style='flex: 1; background: linear-gradient(145deg, #f44336, #e57373); color: white; border: none; padding: 12px; font-size: 16px; font-weight: bold; display: none;'>⏹️ Hủy</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Countdown display - show target time and remaining time
    httpd_resp_sendstr_chunk(req, "<div id='schedule-countdown' style='padding: 12px; background: #e8f5e9; border-radius: 8px; font-size: 16px; text-align: center; color: #2e7d32; font-weight: bold; display: none;'>");
    httpd_resp_sendstr_chunk(req, "<div id='countdown-mode-label'>🔔 Báo thức lúc: <span id='target-datetime'>--</span></div>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size: 20px; margin-top: 8px;'>⏰ Còn lại: <span id='countdown-time'>00:00:00</span></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Status area
    httpd_resp_sendstr_chunk(req, "<div id='schedule-status' style='padding: 8px; background: #fff3e0; border-radius: 6px; font-size: 13px; text-align: center; color: #e65100;'>💡 Chọn chế độ, đặt ngày giờ, sau đó nhấn nút để bắt đầu</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Response area for Page 1
    httpd_resp_sendstr_chunk(req, "<div class='response' id='response'>Ready for commands...</div>");
    httpd_resp_sendstr_chunk(req, "</div>"); // End Page 1
    
    // Page 2: Settings & Configuration
    httpd_resp_sendstr_chunk(req, "<div class='page' id='page2'>");
    
    // Touch Sensor Control Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section' style='display:none;'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🖐️ Cảm Biến Chạm TTP223</div>");
    httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setTouchSensor(true)' id='touch-on' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 16px; font-weight: bold;'>🖐️ BẬT Cảm Biến Chạm</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setTouchSensor(false)' id='touch-off' style='background: linear-gradient(145deg, #f44336, #e57373); color: white; border-color: #c62828; font-size: 16px; font-weight: bold;'>🚫 TẮT Cảm Biến Chạm</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='text-align: center; margin-top: 10px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "Khi BẬT: chạm vào cảm biến → robot nhảy + emoji cười<br>");
    httpd_resp_sendstr_chunk(req, "Khi TẮT: chạm vào cảm biến không có phản ứng");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // System Controls Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>⚙️ Điều Khiển Hệ Thống</div>");
    httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='powerSaveBtn' onclick='toggleScreen()' style='background: linear-gradient(145deg, #9e9e9e, #bdbdbd); color: white; border-color: #616161; font-size: 16px; font-weight: bold;'>📱 Tiết Kiệm: TẮT</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='micBtn' onclick='toggleMic()' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 16px; font-weight: bold;'>🎤 Mic: TẮT</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' id='idleClockBtn' onclick='toggleIdleClock()' style='background: linear-gradient(145deg, #9e9e9e, #bdbdbd); color: white; border-color: #616161; font-size: 16px; font-weight: bold;'>⏰ Đồng Hồ Chờ: TẮT</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='forgetWiFi()' style='background: linear-gradient(145deg, #ff5722, #ff7043); color: white; border-color: #d84315; font-size: 16px; font-weight: bold;'>🔄 Quên WiFi & Tạo AP</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='text-align: center; margin-top: 10px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "<strong>Tiết Kiệm Năng Lượng:</strong> TẮT = bình thường, BẬT = giảm tiêu thụ WiFi<br>");
    httpd_resp_sendstr_chunk(req, "<strong>Mic:</strong> TẮT/BẬT microphone để lắng nghe giọng nói<br>");
    httpd_resp_sendstr_chunk(req, "<strong>Đồng Hồ Chờ:</strong> BẬT = hiển thị đồng hồ khi robot nghỉ<br>");
    httpd_resp_sendstr_chunk(req, "<strong>Quên WiFi & Tạo AP:</strong> xóa WiFi hiện tại, robot sẽ tạo Access Point để cấu hình WiFi mới");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // LED Control Section - GPIO 12 (8 LEDs WS2812)
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>💡 Điều Khiển LED Strip (GPIO 12)</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #e8f5e9, #ffffff); border: 2px solid #4caf50; border-radius: 15px; padding: 15px;'>");
    
    // LED Mode Selection
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #2e7d32; font-size: 15px;'>🎨 Chế Độ LED:</div>");
    httpd_resp_sendstr_chunk(req, "<div class='mode-grid'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"off\")' style='background: #9e9e9e; color: white; border-color: #616161; font-size: 14px;'>⚫ Tắt</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"solid\")' style='background: #2196f3; color: white; border-color: #1565c0; font-size: 14px;'>🔵 Đơn Sắc</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"rainbow\")' style='background: linear-gradient(90deg, #f44336, #ff9800, #ffeb3b, #4caf50, #2196f3, #9c27b0); color: white; border-color: #000; font-size: 14px;'>🌈 Cầu Vồng</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"breathing\")' style='background: #ff9800; color: white; border-color: #e65100; font-size: 14px;'>💨 Thở</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"chase\")' style='background: #9c27b0; color: white; border-color: #6a1b9a; font-size: 14px;'>🏃 Đuổi</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn mode-btn' onclick='setLedMode(\"blink\")' style='background: #f44336; color: white; border-color: #c62828; font-size: 14px;'>⚡ Nhấp Nháy</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Color Picker for Solid Mode
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 12px; background: #fff; border: 2px solid #2196f3; border-radius: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #1565c0; font-size: 14px;'>🎨 Chọn Màu (Chế độ Đơn Sắc):</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(255,0,0)' style='background: #f44336; color: white; border-color: #c62828; font-size: 12px; padding: 8px;'>🔴 Đỏ</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(0,255,0)' style='background: #4caf50; color: white; border-color: #2e7d32; font-size: 12px; padding: 8px;'>🟢 Xanh Lá</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(0,0,255)' style='background: #2196f3; color: white; border-color: #1565c0; font-size: 12px; padding: 8px;'>🔵 Xanh Dương</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(255,255,0)' style='background: #ffeb3b; color: #000; border-color: #f9a825; font-size: 12px; padding: 8px;'>🟡 Vàng</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(255,0,255)' style='background: #e91e63; color: white; border-color: #880e4f; font-size: 12px; padding: 8px;'>🟣 Tím Hồng</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(0,255,255)' style='background: #00bcd4; color: white; border-color: #006064; font-size: 12px; padding: 8px;'>🩵 Cyan</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(255,165,0)' style='background: #ff9800; color: white; border-color: #e65100; font-size: 12px; padding: 8px;'>🟠 Cam</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='setLedColor(255,255,255)' style='background: #ffffff; color: #000; border-color: #000; font-size: 12px; padding: 8px;'>⚪ Trắng</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Brightness Control
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 12px; background: #fff; border: 2px solid #ff9800; border-radius: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #e65100; font-size: 14px;'>💡 Độ Sáng:</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000;'>🔅</span>");
    httpd_resp_sendstr_chunk(req, "<input type='range' id='ledBrightness' min='10' max='255' value='128' oninput='updateLedBrightness(this.value)' style='flex: 1; height: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<span id='ledBrightnessValue' style='font-weight: bold; color: #000; min-width: 50px; text-align: center;'>128</span>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000;'>🔆</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Speed Control
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; padding: 12px; background: #fff; border: 2px solid #9c27b0; border-radius: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #6a1b9a; font-size: 14px;'>⚡ Tốc Độ Animation:</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000;'>🐢</span>");
    httpd_resp_sendstr_chunk(req, "<input type='range' id='ledSpeed' min='10' max='500' value='50' oninput='updateLedSpeed(this.value)' style='flex: 1; height: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<span id='ledSpeedValue' style='font-weight: bold; color: #000; min-width: 50px; text-align: center;'>50ms</span>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold; color: #000;'>🐇</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Control Buttons
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 10px; margin-bottom: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='turnOffLed()' style='flex: 1; background: linear-gradient(145deg, #f44336, #e57373); color: white; border: none; padding: 12px; font-size: 14px; font-weight: bold;'>⚫ Tắt Tất Cả</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='saveLedSettings()' style='flex: 1; background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: none; padding: 12px; font-size: 14px; font-weight: bold;'>💾 Lưu Cài Đặt</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // LED Status Display
    httpd_resp_sendstr_chunk(req, "<div id='led-status' style='padding: 12px; background: #e8f5e9; border-radius: 8px; font-size: 13px; text-align: center; color: #2e7d32; font-weight: bold;'>💡 Trạng thái: Đang tải...</div>");
    
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Screen Rotation Control
    httpd_resp_sendstr_chunk(req, "<div style='margin-top: 15px; padding: 12px; background: #fff3e0; border: 2px solid #ff9800; border-radius: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 10px; color: #e65100; font-size: 15px; text-align: center;'>🔄 Xoay Màn Hình</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-direction: column; gap: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='rotateScreen90()' style='flex: 1; background: #fff; border-color: #ff9800; color: #e65100; padding: 12px; font-size: 16px; font-weight: bold;'>🔄 Xoay 90°</button>");
    httpd_resp_sendstr_chunk(req, "<div id='currentRotation' style='padding: 8px 16px; background: #ffe0b2; border-radius: 6px; font-weight: bold; color: #e65100; white-space: nowrap;'>0°</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='saveScreenRotation()' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border: none; padding: 12px; font-size: 16px; font-weight: bold;'>💾 Lưu Vĩnh Viễn</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='margin-top: 8px; font-size: 12px; color: #666; text-align: center;'>💡 Bấm 'Xoay 90°' để xem trước, 'Lưu' để giữ sau khi reboot</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Idle Timeout Setting
    httpd_resp_sendstr_chunk(req, "<div style='margin-top: 10px; padding: 10px; background: #e8f5e9; border: 2px solid #4caf50; border-radius: 8px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<label style='font-weight: bold; color: #2e7d32; font-size: 14px;'>⏱️ Tự động tiết kiệm pin sau:</label>");
    // Dynamic idle timeout value
    {
        char timeout_input[128];
        snprintf(timeout_input, sizeof(timeout_input), 
            "<input type='number' id='idleTimeoutInput' class='time-input' value='%lu' min='5' max='180' style='width: 60px;'>",
            idle_timeout_minutes);
        httpd_resp_sendstr_chunk(req, timeout_input);
    }
    httpd_resp_sendstr_chunk(req, "<span style='font-size: 14px; color: #2e7d32;'>phút</span>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='updateIdleTimeout()' style='padding: 6px 12px; font-size: 12px; background: #4caf50; color: white;'>✓ Lưu</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='margin-top: 8px; font-size: 12px; color: #666;'>💡 Robot sẽ nằm xuống + tắt màn hình + detach servo sau thời gian không hoạt động</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Auto Pose Advanced Configuration
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>🔄 Cấu Hình Auto Pose</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-config'>");
    
    // Time interval setting - compact
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 10px; padding: 8px; background: #e3f2fd; border: 2px solid #2196f3; border-radius: 6px; display: flex; align-items: center; gap: 8px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<label style='font-weight: bold; color: #000; font-size: 12px;'>⏱️ Giữa tư thế:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='number' id='poseInterval' class='time-input' value='60' min='5' max='300'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-size: 12px;'>giây</span>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='updateInterval()' style='padding: 6px 12px; font-size: 12px;'>✓</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Pose selection checkboxes - 3 columns
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 8px; color: #000; font-size: 13px;'>✅ Chọn tư thế Auto:</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-grid'>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_sit' checked><label for='pose_sit'>🪑 Ngồi</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_jump' checked><label for='pose_jump'>🦘 Nhảy</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_wave' checked><label for='pose_wave'>👋 Vẫy</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_bow' checked><label for='pose_bow'>🙇 Cúi</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_stretch' checked><label for='pose_stretch'>🧘 Giãn</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_swing' checked><label for='pose_swing'>🎯 Lắc</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='pose_dance' checked><label for='pose_dance'>💃 Múa</label></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    httpd_resp_sendstr_chunk(req, "<button class='btn toggle-btn' id='autoPoseBtn2' onclick='toggleAutoPose()' style='width: 100%; margin-top: 15px; font-size: 16px;'>🔄 Bật/Tắt Auto Pose</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Auto Emoji Advanced Configuration
    httpd_resp_sendstr_chunk(req, "<div class='movement-section'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>😊 Cấu Hình Auto Emoji</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-config'>");
    
    // Time interval setting for emoji - compact
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 10px; padding: 8px; background: #fff3e0; border: 2px solid #ff9800; border-radius: 6px; display: flex; align-items: center; gap: 8px; flex-wrap: wrap;'>");
    httpd_resp_sendstr_chunk(req, "<label style='font-weight: bold; color: #000; font-size: 12px;'>⏱️ Giữa emoji:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='number' id='emojiInterval' class='time-input' value='10' min='3' max='120'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-size: 12px;'>giây</span>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='updateEmojiInterval()' style='padding: 6px 12px; font-size: 12px;'>✓</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Emoji selection checkboxes - 3 columns
    httpd_resp_sendstr_chunk(req, "<div style='font-weight: bold; margin-bottom: 8px; color: #000; font-size: 13px;'>✅ Chọn emoji Auto:</div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-grid'>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_happy' checked><label for='emoji_happy'>😊 Vui</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_laughing' checked><label for='emoji_laughing'>😂 Cười</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_winking' checked><label for='emoji_winking'>😜 Nháy</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_cool' checked><label for='emoji_cool'>😎 Ngầu</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_love' checked><label for='emoji_love'>😍 Yêu</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_surprised' checked><label for='emoji_surprised'>😮 Ngạc</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_excited' checked><label for='emoji_excited'>🤩 Khích</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_sleepy' checked><label for='emoji_sleepy'>😴 Ngủ</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_sad' checked><label for='emoji_sad'>😢 Buồn</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_angry' checked><label for='emoji_angry'>😠 Giận</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_confused' checked><label for='emoji_confused'>😕 Rối</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_thinking' checked><label for='emoji_thinking'>🤔 Nghĩ</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_neutral' checked><label for='emoji_neutral'>😐 Thường</label></div>");
    httpd_resp_sendstr_chunk(req, "<div class='pose-item'><input type='checkbox' id='emoji_shocked' checked><label for='emoji_shocked'>😱 Sốc</label></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
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

    // MQTT Configuration Section
    httpd_resp_sendstr_chunk(req, "<div class='movement-section' style='display:none;'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title'>📡 Cấu Hình MQTT</div>");
    httpd_resp_sendstr_chunk(req, "<div style='background: linear-gradient(145deg, #f8f8f8, #ffffff); border: 2px solid #ff9800; border-radius: 15px; padding: 20px; margin-bottom: 20px;'>");
    httpd_resp_sendstr_chunk(req, "<div style='margin-bottom: 15px; color: #666; font-size: 14px;'>");
    httpd_resp_sendstr_chunk(req, "📡 Cấu hình MQTT server để Otto kết nối và giao tiếp qua MQTT protocol.<br>");
    httpd_resp_sendstr_chunk(req, "⚠️ <strong>Endpoint là bắt buộc</strong> (ví dụ: mqtt.example.com:8883 hoặc 192.168.1.100:8883)");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-direction: column; gap: 12px;'>");
    httpd_resp_sendstr_chunk(req, "<div><label style='display: block; font-weight: bold; margin-bottom: 5px; color: #000;'>Endpoint <span style='color: red;'>*</span>:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='text' id='mqttEndpoint' placeholder='mqtt.example.com:8883' style='width: 100%; padding: 10px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'></div>");
    httpd_resp_sendstr_chunk(req, "<div><label style='display: block; font-weight: bold; margin-bottom: 5px; color: #000;'>Client ID:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='text' id='mqttClientId' placeholder='otto-robot-001' style='width: 100%; padding: 10px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'></div>");
    httpd_resp_sendstr_chunk(req, "<div><label style='display: block; font-weight: bold; margin-bottom: 5px; color: #000;'>Username:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='text' id='mqttUsername' placeholder='(tùy chọn)' style='width: 100%; padding: 10px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'></div>");
    httpd_resp_sendstr_chunk(req, "<div><label style='display: block; font-weight: bold; margin-bottom: 5px; color: #000;'>Password:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='password' id='mqttPassword' placeholder='(tùy chọn)' style='width: 100%; padding: 10px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'></div>");
    httpd_resp_sendstr_chunk(req, "<div><label style='display: block; font-weight: bold; margin-bottom: 5px; color: #000;'>Publish Topic:</label>");
    httpd_resp_sendstr_chunk(req, "<input type='text' id='mqttPublishTopic' placeholder='otto/robot/001' style='width: 100%; padding: 10px; border: 2px solid #ddd; border-radius: 8px; font-size: 14px;'></div>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='saveMqttConfig()' style='background: linear-gradient(145deg, #ff9800, #ffa726); color: white; border-color: #f57c00; font-weight: bold; padding: 12px 20px; width: 100%; margin-top: 10px;'>💾 Lưu Cấu Hình MQTT</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div id='mqttConfigStatus' style='margin-top: 10px; font-size: 14px; color: #666;'></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");

    // Response area for Page 2
    httpd_resp_sendstr_chunk(req, "<div class='response' id='response2'>Cấu hình sẵn sàng...</div>");
    httpd_resp_sendstr_chunk(req, "</div>"); // End Page 2
    
    // Page 4: Drawing Canvas
    httpd_resp_sendstr_chunk(req, "<div class='page' id='page4'>");
    httpd_resp_sendstr_chunk(req, "<div class='section-title' style='text-align: center; margin-bottom: 15px;'>🎨 Vẽ & Hiển Thị Lên Robot</div>");
    
    // Canvas container
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-direction: column; align-items: center;'>");
    httpd_resp_sendstr_chunk(req, "<canvas id='drawCanvas' width='240' height='240' style='border: 3px solid #333; border-radius: 12px; background: #fff; touch-action: none;'></canvas>");
    
    // Drawing tools
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-wrap: wrap; gap: 10px; justify-content: center; margin-top: 15px;'>");
    
    // Color palette
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; flex-wrap: wrap; gap: 5px; justify-content: center;'>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#000000\")' style='width: 35px; height: 35px; border-radius: 50%; background: #000; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#ff0000\")' style='width: 35px; height: 35px; border-radius: 50%; background: #ff0000; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#00ff00\")' style='width: 35px; height: 35px; border-radius: 50%; background: #00ff00; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#0000ff\")' style='width: 35px; height: 35px; border-radius: 50%; background: #0000ff; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#ffff00\")' style='width: 35px; height: 35px; border-radius: 50%; background: #ffff00; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#ff9800\")' style='width: 35px; height: 35px; border-radius: 50%; background: #ff9800; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#9c27b0\")' style='width: 35px; height: 35px; border-radius: 50%; background: #9c27b0; border: 2px solid #fff; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "<button class='color-btn' onclick='setColor(\"#ffffff\")' style='width: 35px; height: 35px; border-radius: 50%; background: #fff; border: 2px solid #333; box-shadow: 0 2px 4px rgba(0,0,0,0.3);'></button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Brush size
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; align-items: center; gap: 10px; margin-top: 10px;'>");
    httpd_resp_sendstr_chunk(req, "<span style='font-weight: bold;'>🖌️ Cọ:</span>");
    httpd_resp_sendstr_chunk(req, "<input type='range' id='brushSize' min='1' max='20' value='5' style='flex: 1;' onchange='updateBrushSize()'>");
    httpd_resp_sendstr_chunk(req, "<span id='brushSizeValue' style='min-width: 30px;'>5px</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Action buttons
    httpd_resp_sendstr_chunk(req, "<div style='display: flex; gap: 10px; margin-top: 15px; flex-wrap: wrap; justify-content: center;'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='clearCanvas()' style='background: linear-gradient(145deg, #f44336, #e57373); color: white; border-color: #c62828; padding: 12px 25px; font-size: 16px;'>🗑️ Xóa</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='sendDrawing()' style='background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; padding: 12px 25px; font-size: 16px;'>📤 Gửi Đến Robot</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='exitDrawing()' style='background: linear-gradient(145deg, #9e9e9e, #bdbdbd); color: white; border-color: #616161; padding: 12px 25px; font-size: 16px;'>↩️ Quay Lại Emoji</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Response area for Page 4
    httpd_resp_sendstr_chunk(req, "<div class='response' id='response4'>Vẽ và gửi hình đến robot!</div>");
    httpd_resp_sendstr_chunk(req, "</div>"); // End Page 4
    
    httpd_resp_sendstr_chunk(req, "</div>"); // End container
    
    // JavaScript - Simple and clean
    httpd_resp_sendstr_chunk(req, "<script>");
    // showStatus function - From kytuoi repository
    httpd_resp_sendstr_chunk(req, "function showStatus(message, isError) {");
    httpd_resp_sendstr_chunk(req, "  const status = document.getElementById('status');");
    httpd_resp_sendstr_chunk(req, "  if (status) {");
    httpd_resp_sendstr_chunk(req, "    status.className = 'status ' + (isError ? 'error' : 'success');");
    httpd_resp_sendstr_chunk(req, "    status.textContent = message;");
    httpd_resp_sendstr_chunk(req, "    setTimeout(() => status.textContent = '', 3000);");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Page navigation
    httpd_resp_sendstr_chunk(req, "function showPage(pageNum) {");
    httpd_resp_sendstr_chunk(req, "  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));");
    httpd_resp_sendstr_chunk(req, "  document.querySelectorAll('.nav-tab').forEach(t => t.classList.remove('active'));");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('page' + pageNum).classList.add('active');");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('tab' + pageNum).classList.add('active');");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Speed multiplier is now handled server-side
    // Track action recording for memory slots with explicit START/STOP
    httpd_resp_sendstr_chunk(req, "let recordedActions = [];");
    httpd_resp_sendstr_chunk(req, "let lastEmotion = 'neutral';");
    httpd_resp_sendstr_chunk(req, "let isRecording = false;");
    httpd_resp_sendstr_chunk(req, "let recordingSlot = 0;");
    httpd_resp_sendstr_chunk(req, "function sendAction(action, param1, param2) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Action:', action, 'p1:', param1, 'p2:', param2, 'emoji:', lastEmotion);");
    httpd_resp_sendstr_chunk(req, "  if (isRecording) {");
    httpd_resp_sendstr_chunk(req, "    recordedActions.push({action: action, p1: param1, p2: param2, emoji: lastEmotion});");
    httpd_resp_sendstr_chunk(req, "    updateRecordingStatus();");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  var url = '/action?cmd=' + action + '&p1=' + param1 + '&p2=' + param2;");
    httpd_resp_sendstr_chunk(req, "  fetch(url).then(r => r.text()).then(d => console.log('Success:', d));");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function sendEmotion(emotion) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Emotion:', emotion);");
    httpd_resp_sendstr_chunk(req, "  lastEmotion = emotion;");
    httpd_resp_sendstr_chunk(req, "  if (isRecording) {");
    httpd_resp_sendstr_chunk(req, "    recordedActions.push({action: 'emoji', p1: 0, p2: 0, emoji: emotion});");
    httpd_resp_sendstr_chunk(req, "    updateRecordingStatus();");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  fetch('/emotion?emotion=' + emotion).then(r => r.text()).then(d => console.log('Success:', d));");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function updateRecordingStatus() {");
    httpd_resp_sendstr_chunk(req, "  if (isRecording && recordingSlot > 0) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById(`slot${recordingSlot}-status`).innerHTML = `🟢 Đang ghi: ${recordedActions.length} hành động`;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Memory slot functions
    httpd_resp_sendstr_chunk(req, "function startRecording(slotNum) {");
    httpd_resp_sendstr_chunk(req, "  if (isRecording) {");
    httpd_resp_sendstr_chunk(req, "    alert('⚠️ Đang ghi vị trí ' + recordingSlot + '! Hãy lưu hoặc hủy trước.');");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  isRecording = true;");
    httpd_resp_sendstr_chunk(req, "  recordingSlot = slotNum;");
    httpd_resp_sendstr_chunk(req, "  recordedActions = [];");
    httpd_resp_sendstr_chunk(req, "  lastEmotion = 'neutral';");
    httpd_resp_sendstr_chunk(req, "  const recordBtn = document.getElementById(`record${slotNum}-btn`);");
    httpd_resp_sendstr_chunk(req, "  const saveBtn = document.getElementById(`save${slotNum}-btn`);");
    httpd_resp_sendstr_chunk(req, "  recordBtn.innerHTML = '🟢 Đang ghi...';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.style.background = 'linear-gradient(145deg, #4caf50, #66bb6a)';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.style.borderColor = '#2e7d32';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.onclick = () => stopRecording(slotNum);");
    httpd_resp_sendstr_chunk(req, "  saveBtn.disabled = false;");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.background = 'linear-gradient(145deg, #4caf50, #66bb6a)';");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.color = 'white';");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.borderColor = '#2e7d32';");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.cursor = 'pointer';");
    httpd_resp_sendstr_chunk(req, "  document.getElementById(`slot${slotNum}-status`).innerHTML = '🟢 Đang ghi: 0 hành động';");
    httpd_resp_sendstr_chunk(req, "  console.log('🔴 Started recording for slot', slotNum);");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function stopRecording(slotNum) {");
    httpd_resp_sendstr_chunk(req, "  if (!isRecording || recordingSlot !== slotNum) return;");
    httpd_resp_sendstr_chunk(req, "  isRecording = false;");
    httpd_resp_sendstr_chunk(req, "  const recordBtn = document.getElementById(`record${slotNum}-btn`);");
    httpd_resp_sendstr_chunk(req, "  const saveBtn = document.getElementById(`save${slotNum}-btn`);");
    httpd_resp_sendstr_chunk(req, "  recordBtn.innerHTML = '🔴 Bắt đầu ghi';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.style.background = 'linear-gradient(145deg, #ff5722, #ff7043)';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.style.borderColor = '#d84315';");
    httpd_resp_sendstr_chunk(req, "  recordBtn.onclick = () => startRecording(slotNum);");
    httpd_resp_sendstr_chunk(req, "  saveBtn.disabled = true;");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.background = '#e0e0e0';");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.color = '#999';");
    httpd_resp_sendstr_chunk(req, "  saveBtn.style.cursor = 'not-allowed';");
    httpd_resp_sendstr_chunk(req, "  document.getElementById(`slot${slotNum}-status`).innerHTML = '⚪ Đã dừng ghi';");
    httpd_resp_sendstr_chunk(req, "  recordedActions = [];");
    httpd_resp_sendstr_chunk(req, "  recordingSlot = 0;");
    httpd_resp_sendstr_chunk(req, "  console.log('⏹️ Stopped recording for slot', slotNum);");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function saveSlot(slotNum) {");
    httpd_resp_sendstr_chunk(req, "  if (!isRecording || recordingSlot !== slotNum) {");
    httpd_resp_sendstr_chunk(req, "    alert('⚠️ Chưa bắt đầu ghi! Bấm \"🔴 Bắt đầu ghi\" trước.');");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  if (recordedActions.length === 0) {");
    httpd_resp_sendstr_chunk(req, "    alert('⚠️ Chưa có hành động nào! Thực hiện hành động (đi, nhảy, ngồi...) rồi bấm Lưu.');");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  let actionsStr = recordedActions.map(a => `${a.action},${a.p1},${a.p2},${a.emoji}`).join(';');");
    httpd_resp_sendstr_chunk(req, "  fetch(`/save_slot?slot=${slotNum}&actions=${encodeURIComponent(actionsStr)}&emotion=${lastEmotion}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.json())");
    httpd_resp_sendstr_chunk(req, "    .then(data => {");
    httpd_resp_sendstr_chunk(req, "      if (data.success) {");
    httpd_resp_sendstr_chunk(req, "        alert(`✅ Đã lưu ${data.count} hành động vào vị trí ${slotNum}!`);");
    httpd_resp_sendstr_chunk(req, "        document.getElementById(`slot${slotNum}-info`).innerHTML = `📦 ${data.count} hành động • Emoji: ${data.emotion}`;");
    httpd_resp_sendstr_chunk(req, "        document.getElementById(`slot${slotNum}-status`).innerHTML = '✅ Đã lưu';");
    httpd_resp_sendstr_chunk(req, "        stopRecording(slotNum);");
    httpd_resp_sendstr_chunk(req, "      } else {");
    httpd_resp_sendstr_chunk(req, "        alert('❌ Lỗi: ' + data.error);");
    httpd_resp_sendstr_chunk(req, "      }");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => alert('❌ Lỗi kết nối: ' + e));");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function playSlot(slotNum) {");
    httpd_resp_sendstr_chunk(req, "  fetch(`/play_slot?slot=${slotNum}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.json())");
    httpd_resp_sendstr_chunk(req, "    .then(data => {");
    httpd_resp_sendstr_chunk(req, "      if (data.success) {");
    httpd_resp_sendstr_chunk(req, "        alert(`▶️ Đang phát lại ${data.count} hành động từ vị trí ${slotNum}`);");
    httpd_resp_sendstr_chunk(req, "      } else {");
    httpd_resp_sendstr_chunk(req, "        alert('❌ ' + data.error);");
    httpd_resp_sendstr_chunk(req, "      }");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => alert('❌ Lỗi: ' + e));");
    httpd_resp_sendstr_chunk(req, "}");
    
    // LED Control Functions
    httpd_resp_sendstr_chunk(req, "function setLedMode(mode) {");
    httpd_resp_sendstr_chunk(req, "  fetch(`/led_mode?mode=${mode}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('led-status').textContent = '💡 ' + d;");
    httpd_resp_sendstr_chunk(req, "      getLedState();");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function setLedColor(r, g, b) {");
    httpd_resp_sendstr_chunk(req, "  fetch(`/led?r=${r}&g=${g}&b=${b}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('led-status').textContent = '💡 ' + d;");
    httpd_resp_sendstr_chunk(req, "      setLedMode('solid');");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function updateLedBrightness(value) {");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('ledBrightnessValue').textContent = value;");
    httpd_resp_sendstr_chunk(req, "  fetch(`/led_brightness?value=${value}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => document.getElementById('led-status').textContent = '💡 ' + d)");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function updateLedSpeed(value) {");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('ledSpeedValue').textContent = value + 'ms';");
    httpd_resp_sendstr_chunk(req, "  fetch(`/led_speed?value=${value}`)");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => document.getElementById('led-status').textContent = '💡 ' + d)");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function turnOffLed() {");
    httpd_resp_sendstr_chunk(req, "  fetch('/led_off')");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('led-status').textContent = '⚫ ' + d;");
    httpd_resp_sendstr_chunk(req, "      getLedState();");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function saveLedSettings() {");
    httpd_resp_sendstr_chunk(req, "  fetch('/led_save')");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "    .then(d => document.getElementById('led-status').textContent = '💾 ' + d)");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Lỗi: ' + e);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function getLedState() {");
    httpd_resp_sendstr_chunk(req, "  fetch('/led_state')");
    httpd_resp_sendstr_chunk(req, "    .then(r => r.json())");
    httpd_resp_sendstr_chunk(req, "    .then(d => {");
    httpd_resp_sendstr_chunk(req, "      const modes = {0:'⚫ Tắt', 1:'🔵 Đơn Sắc', 2:'🌈 Cầu Vồng', 3:'💨 Thở', 4:'🏃 Đuổi', 5:'⚡ Nhấp Nháy'};");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('led-status').textContent = `💡 ${modes[d.mode]} • Màu: RGB(${d.r},${d.g},${d.b}) • Sáng: ${d.brightness} • Tốc độ: ${d.speed}ms`;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('ledBrightness').value = d.brightness;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('ledBrightnessValue').textContent = d.brightness;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('ledSpeed').value = d.speed;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('ledSpeedValue').textContent = d.speed + 'ms';");
    httpd_resp_sendstr_chunk(req, "    })");
    httpd_resp_sendstr_chunk(req, "    .catch(e => document.getElementById('led-status').textContent = '❌ Không thể tải trạng thái');");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "window.onload = function() {");
    httpd_resp_sendstr_chunk(req, "  getLedState();");
    httpd_resp_sendstr_chunk(req, "  for(let i=1; i<=3; i++) {");
    httpd_resp_sendstr_chunk(req, "    fetch(`/slot_info?slot=${i}`).then(r => r.json()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      if(d.used) {");
    httpd_resp_sendstr_chunk(req, "        document.getElementById(`slot${i}-info`).innerHTML = `📦 ${d.count} hành động • Emoji: ${d.emotion}`;");
    httpd_resp_sendstr_chunk(req, "        document.getElementById(`slot${i}-status`).innerHTML = '✅ Đã lưu';");
    httpd_resp_sendstr_chunk(req, "      }");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "};");
    httpd_resp_sendstr_chunk(req, "function setEmojiMode(useOttoEmoji) {");
    // For compatibility, send 'gif' when Otto mode is selected (server also accepts 'otto')
    httpd_resp_sendstr_chunk(req, "  var mode = useOttoEmoji ? 'gif' : 'default';");
    httpd_resp_sendstr_chunk(req, "  fetch('/emoji_mode?mode=' + mode).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Mode:', d);");
    // Update button styles
    httpd_resp_sendstr_chunk(req, "    var ottoBtn = document.getElementById('otto-mode');");
    httpd_resp_sendstr_chunk(req, "    var defaultBtn = document.getElementById('default-mode');");
    httpd_resp_sendstr_chunk(req, "    if (useOttoEmoji) {");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.classList.add('active');");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.style.cssText = 'background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 18px; font-weight: bold;';");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.innerHTML = '🤖 OTTO GIF MODE (ACTIVE)';");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.classList.remove('active');");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.style.cssText = '';");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.innerHTML = '😊 Twemoji Text Mode';");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.classList.add('active');");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.style.cssText = 'background: linear-gradient(145deg, #4caf50, #66bb6a); color: white; border-color: #2e7d32; font-size: 18px; font-weight: bold;';");
    httpd_resp_sendstr_chunk(req, "      defaultBtn.innerHTML = '😊 TWEMOJI TEXT MODE (ACTIVE)';");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.classList.remove('active');");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.style.cssText = '';");
    httpd_resp_sendstr_chunk(req, "      ottoBtn.innerHTML = '🤖 Otto GIF Mode';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function setTouchSensor(enabled) {");
    httpd_resp_sendstr_chunk(req, "  console.log('Touch sensor:', enabled);");
    httpd_resp_sendstr_chunk(req, "  fetch('/touch_sensor?enabled=' + enabled).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Touch sensor result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
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
    
    // Screen rotation JavaScript - rotate 90 degrees at a time
    httpd_resp_sendstr_chunk(req, "let currentRotation = 0;");
    httpd_resp_sendstr_chunk(req, "function rotateScreen90() {");
    httpd_resp_sendstr_chunk(req, "  currentRotation = (currentRotation + 90) % 360;");
    httpd_resp_sendstr_chunk(req, "  console.log('Rotating screen to:', currentRotation);");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('currentRotation').textContent = currentRotation + '°';");
    httpd_resp_sendstr_chunk(req, "  fetch('/screen_rotation?angle=' + currentRotation + '&save=0').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Screen rotation result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '🔄 Xoay màn hình: ' + currentRotation + '° (chưa lưu)';");
    httpd_resp_sendstr_chunk(req, "  }).catch(err => {");
    httpd_resp_sendstr_chunk(req, "    console.error('Screen rotation error:', err);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '❌ Lỗi xoay màn hình';");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function saveScreenRotation() {");
    httpd_resp_sendstr_chunk(req, "  console.log('Saving screen rotation:', currentRotation);");
    httpd_resp_sendstr_chunk(req, "  fetch('/screen_rotation?angle=' + currentRotation + '&save=1').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Save result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '✅ Đã lưu: ' + currentRotation + '° (sẽ giữ sau reboot)';");
    httpd_resp_sendstr_chunk(req, "  }).catch(err => {");
    httpd_resp_sendstr_chunk(req, "    console.error('Save error:', err);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = '❌ Lỗi lưu cài đặt';");
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
    
    // Toggle idle clock JavaScript - with state tracking
    httpd_resp_sendstr_chunk(req, "let idleClockActive = false;");
    httpd_resp_sendstr_chunk(req, "function toggleIdleClock() {");
    httpd_resp_sendstr_chunk(req, "  const btn = document.getElementById('idleClockBtn');");
    httpd_resp_sendstr_chunk(req, "  if (idleClockActive) {");
    httpd_resp_sendstr_chunk(req, "    console.log('Disabling idle clock...');");
    httpd_resp_sendstr_chunk(req, "    fetch('/idle_clock?enable=0').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Idle clock disabled:', d);");
    httpd_resp_sendstr_chunk(req, "      idleClockActive = false;");
    httpd_resp_sendstr_chunk(req, "      btn.innerHTML = '⏰ Đồng Hồ Chờ: TẮT';");
    httpd_resp_sendstr_chunk(req, "      btn.style.background = 'linear-gradient(145deg, #9e9e9e, #bdbdbd)';");
    httpd_resp_sendstr_chunk(req, "      btn.style.borderColor = '#616161';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    console.log('Enabling idle clock...');");
    httpd_resp_sendstr_chunk(req, "    fetch('/idle_clock?enable=1').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "      console.log('Idle clock enabled:', d);");
    httpd_resp_sendstr_chunk(req, "      idleClockActive = true;");
    httpd_resp_sendstr_chunk(req, "      btn.innerHTML = '⏰ Đồng Hồ Chờ: BẬT';");
    httpd_resp_sendstr_chunk(req, "      btn.style.background = 'linear-gradient(145deg, #2196f3, #42a5f5)';");
    httpd_resp_sendstr_chunk(req, "      btn.style.borderColor = '#1976d2';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Update idle timeout JavaScript
    httpd_resp_sendstr_chunk(req, "function updateIdleTimeout() {");
    httpd_resp_sendstr_chunk(req, "  const minutes = document.getElementById('idleTimeoutInput').value;");
    httpd_resp_sendstr_chunk(req, "  if (minutes < 5 || minutes > 180) {");
    httpd_resp_sendstr_chunk(req, "    alert('Thời gian phải từ 5-180 phút!');");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  console.log('Setting idle timeout:', minutes, 'minutes');");
    httpd_resp_sendstr_chunk(req, "  fetch('/idle_timeout?minutes=' + minutes).then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    console.log('Idle timeout result:', d);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response2').innerHTML = d;");
    httpd_resp_sendstr_chunk(req, "  });");
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
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '✅ Tự động đổi biểu cảm BẬT';");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    if(btn) { btn.classList.remove('active'); btn.style.background = ''; btn.style.color = ''; }");
    httpd_resp_sendstr_chunk(req, "    if(document.getElementById('response2')) document.getElementById('response2').innerHTML = '⛔ Tự động đổi biểu cảm TẮT';");
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
    
    // Gemini API Key functions (REMOVED - Not needed)
    /*
    httpd_resp_sendstr_chunk(req, "function saveGeminiKey() {");
    httpd_resp_sendstr_chunk(req, "  var apiKey = document.getElementById('geminiApiKey').value;");
    httpd_resp_sendstr_chunk(req, "  if (!apiKey || apiKey.trim() === '') {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').innerHTML = '❌ Vui lòng nhập API key!';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('geminiKeyStatus').innerHTML = '⏳ Đang lưu...';");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('geminiKeyStatus').style.color = '#666';");
    httpd_resp_sendstr_chunk(req, "  fetch('/gemini_api_key', {");
    httpd_resp_sendstr_chunk(req, "    method: 'POST',");
    httpd_resp_sendstr_chunk(req, "    headers: {'Content-Type': 'application/json'},");
    httpd_resp_sendstr_chunk(req, "    body: JSON.stringify({api_key: apiKey})");
    httpd_resp_sendstr_chunk(req, "  }).then(r => r.json()).then(data => {");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '✅ API key đã được lưu thành công!';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#4caf50';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiApiKey').value = '';");
    httpd_resp_sendstr_chunk(req, "      loadGeminiKeyStatus();");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '❌ Lỗi: ' + data.error;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  }).catch(e => {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').innerHTML = '❌ Lỗi kết nối: ' + e;");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('geminiKeyStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function loadGeminiKeyStatus() {");
    httpd_resp_sendstr_chunk(req, "  fetch('/gemini_api_key').then(r => r.json()).then(data => {");
    httpd_resp_sendstr_chunk(req, "    if (data.configured) {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '✅ API key đã cấu hình: ' + data.key_preview;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#4caf50';");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').innerHTML = '⚠️ Chưa có API key. Nhập key để kích hoạt Gemini AI.';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('geminiKeyStatus').style.color = '#ff9800';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    */
    
    // AI text chat function - IMPROVED VERSION
    httpd_resp_sendstr_chunk(req, "let chatHistory = [];");
    httpd_resp_sendstr_chunk(req, "function addToChatHistory(sender, message) {");
    httpd_resp_sendstr_chunk(req, "  chatHistory.push({sender, message, time: new Date().toLocaleTimeString('vi-VN', {hour: '2-digit', minute: '2-digit'})});");
    httpd_resp_sendstr_chunk(req, "  if(chatHistory.length > 20) chatHistory.shift();");
    httpd_resp_sendstr_chunk(req, "  updateChatDisplay();");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function updateChatDisplay() {");
    httpd_resp_sendstr_chunk(req, "  const historyDiv = document.getElementById('chat-history');");
    httpd_resp_sendstr_chunk(req, "  if(chatHistory.length === 0) {");
    httpd_resp_sendstr_chunk(req, "    historyDiv.innerHTML = '<div style=\"color:#999;text-align:center;\">💬 Lịch sử chat sẽ hiển thị ở đây...</div>';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  let html = '';");
    httpd_resp_sendstr_chunk(req, "  chatHistory.forEach(c => {");
    httpd_resp_sendstr_chunk(req, "    const isUser = c.sender === 'user';");
    httpd_resp_sendstr_chunk(req, "    const align = isUser ? 'right' : 'left';");
    httpd_resp_sendstr_chunk(req, "    const bg = isUser ? '#1976d2' : '#e0e0e0';");
    httpd_resp_sendstr_chunk(req, "    const color = isUser ? 'white' : '#333';");
    httpd_resp_sendstr_chunk(req, "    const icon = isUser ? '👤' : '🤖';");
    httpd_resp_sendstr_chunk(req, "    html += `<div style='text-align:${align};margin:5px 0;'><span style='display:inline-block;max-width:80%;padding:8px 12px;border-radius:12px;background:${bg};color:${color};font-size:13px;'>${icon} ${c.message}<span style='font-size:10px;opacity:0.7;margin-left:8px;'>${c.time}</span></span></div>`;");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "  historyDiv.innerHTML = html;");
    httpd_resp_sendstr_chunk(req, "  historyDiv.scrollTop = historyDiv.scrollHeight;");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function clearChatHistory() {");
    httpd_resp_sendstr_chunk(req, "  chatHistory = [];");
    httpd_resp_sendstr_chunk(req, "  updateChatDisplay();");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('ai-text-status').innerHTML = '🗑️ Đã xóa lịch sử chat';");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function quickSend(text) {");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('ai_text_input').value = text;");
    httpd_resp_sendstr_chunk(req, "  sendTextToAI();");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "async function sendTextToAI() {");
    httpd_resp_sendstr_chunk(req, "  const input = document.getElementById('ai_text_input');");
    httpd_resp_sendstr_chunk(req, "  let text = input.value.trim();");
    httpd_resp_sendstr_chunk(req, "  if (!text) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').innerHTML = '⚠️ Vui lòng nhập tin nhắn';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').style.background = '#fff3e0';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').style.color = '#e65100';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    // Add text length validation - max 1500 characters (same as MAX_TEXT_LENGTH in application.cc)
    httpd_resp_sendstr_chunk(req, "  const maxLen = 1500;");
    httpd_resp_sendstr_chunk(req, "  if (text.length > maxLen) {");
    httpd_resp_sendstr_chunk(req, "    text = text.substring(0, maxLen);");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').innerHTML = '⚠️ Tin nhắn quá dài, đã cắt bớt còn ' + maxLen + ' ký tự';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').style.background = '#fff3e0';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('ai-text-status').style.color = '#e65100';");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  addToChatHistory('user', text);");
    httpd_resp_sendstr_chunk(req, "  input.value = '';");
    httpd_resp_sendstr_chunk(req, "  const statusDiv = document.getElementById('ai-text-status');");
    httpd_resp_sendstr_chunk(req, "  statusDiv.innerHTML = '⏳ Đang gửi đến Kiki...';");
    httpd_resp_sendstr_chunk(req, "  statusDiv.style.background = '#e3f2fd';");
    httpd_resp_sendstr_chunk(req, "  statusDiv.style.color = '#1976d2';");
    httpd_resp_sendstr_chunk(req, "  try {");
    httpd_resp_sendstr_chunk(req, "    const res = await fetch('/api/ai/send', {");
    httpd_resp_sendstr_chunk(req, "      method: 'POST',");
    httpd_resp_sendstr_chunk(req, "      headers: {'Content-Type': 'application/json'},");
    httpd_resp_sendstr_chunk(req, "      body: JSON.stringify({ text: text })");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "    const data = await res.json();");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      statusDiv.innerHTML = '✅ Kiki đã nhận tin nhắn! Đang xử lý...';");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.background = '#e8f5e9';");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.color = '#2e7d32';");
    httpd_resp_sendstr_chunk(req, "      addToChatHistory('kiki', 'Đang trả lời...');");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      statusDiv.innerHTML = '❌ Lỗi: ' + data.message;");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "      statusDiv.style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  } catch (e) {");
    httpd_resp_sendstr_chunk(req, "    statusDiv.innerHTML = '❌ Lỗi kết nối: ' + e.message;");
    httpd_resp_sendstr_chunk(req, "    statusDiv.style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "    statusDiv.style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "document.getElementById('ai_text_input').addEventListener('keydown', function(e) {");
    httpd_resp_sendstr_chunk(req, "  if (e.key === 'Enter' && !e.shiftKey) {");
    httpd_resp_sendstr_chunk(req, "    e.preventDefault();");
    httpd_resp_sendstr_chunk(req, "    sendTextToAI();");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "});");
    
    // Character counter function for AI text input
    httpd_resp_sendstr_chunk(req, "function updateCharCount() {");
    httpd_resp_sendstr_chunk(req, "  const input = document.getElementById('ai_text_input');");
    httpd_resp_sendstr_chunk(req, "  const counter = document.getElementById('char-counter');");
    httpd_resp_sendstr_chunk(req, "  const len = input.value.length;");
    httpd_resp_sendstr_chunk(req, "  counter.textContent = len + ' / 1500 ký tự';");
    httpd_resp_sendstr_chunk(req, "  if (len > 1400) { counter.style.color = '#f44336'; }");
    httpd_resp_sendstr_chunk(req, "  else if (len > 1200) { counter.style.color = '#ff9800'; }");
    httpd_resp_sendstr_chunk(req, "  else { counter.style.color = '#666'; }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Schedule Message JavaScript functions - với ngày tháng năm giờ phút
    httpd_resp_sendstr_chunk(req, "let scheduleActive = false;");
    httpd_resp_sendstr_chunk(req, "let countdownInterval = null;");
    httpd_resp_sendstr_chunk(req, "let targetTimestamp = 0;");
    httpd_resp_sendstr_chunk(req, "let currentScheduleMode = 'alarm';");
    
    // Update schedule mode when radio button changes
    httpd_resp_sendstr_chunk(req, "function updateScheduleMode() {");
    httpd_resp_sendstr_chunk(req, "  const mode = document.querySelector('input[name=\"schedule_mode\"]:checked').value;");
    httpd_resp_sendstr_chunk(req, "  currentScheduleMode = mode;");
    httpd_resp_sendstr_chunk(req, "  const msgSection = document.getElementById('message_input_section');");
    httpd_resp_sendstr_chunk(req, "  const startBtn = document.getElementById('scheduleStartBtn');");
    httpd_resp_sendstr_chunk(req, "  if (mode === 'message') {");
    httpd_resp_sendstr_chunk(req, "    msgSection.style.display = 'block';");
    httpd_resp_sendstr_chunk(req, "    startBtn.innerHTML = '💬 Hẹn Gửi Tin Nhắn';");
    httpd_resp_sendstr_chunk(req, "    startBtn.style.background = 'linear-gradient(145deg, #2196f3, #64b5f6)';");
    httpd_resp_sendstr_chunk(req, "  } else {");
    httpd_resp_sendstr_chunk(req, "    msgSection.style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "    startBtn.innerHTML = '🔔 Đặt Báo Thức';");
    httpd_resp_sendstr_chunk(req, "    startBtn.style.background = 'linear-gradient(145deg, #4caf50, #66bb6a)';");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Initialize date/time inputs với thời gian hiện tại + 5 phút
    httpd_resp_sendstr_chunk(req, "function initScheduleDateTime() {");
    httpd_resp_sendstr_chunk(req, "  const now = new Date();");
    httpd_resp_sendstr_chunk(req, "  now.setMinutes(now.getMinutes() + 5);");
    httpd_resp_sendstr_chunk(req, "  const dateStr = now.toISOString().split('T')[0];");
    httpd_resp_sendstr_chunk(req, "  const timeStr = now.toTimeString().slice(0,5);");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('schedule_date').value = dateStr;");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('schedule_time').value = timeStr;");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Đặt nhanh thời gian: thêm X phút vào thời gian hiện tại
    httpd_resp_sendstr_chunk(req, "function setQuickSchedule(minutes) {");
    httpd_resp_sendstr_chunk(req, "  const now = new Date();");
    httpd_resp_sendstr_chunk(req, "  now.setMinutes(now.getMinutes() + minutes);");
    httpd_resp_sendstr_chunk(req, "  const dateStr = now.toISOString().split('T')[0];");
    httpd_resp_sendstr_chunk(req, "  const timeStr = now.toTimeString().slice(0,5);");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('schedule_date').value = dateStr;");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('schedule_time').value = timeStr;");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function formatTime(totalSeconds) {");
    httpd_resp_sendstr_chunk(req, "  if (totalSeconds < 0) totalSeconds = 0;");
    httpd_resp_sendstr_chunk(req, "  const d = Math.floor(totalSeconds / 86400);");
    httpd_resp_sendstr_chunk(req, "  const h = Math.floor((totalSeconds % 86400) / 3600);");
    httpd_resp_sendstr_chunk(req, "  const m = Math.floor((totalSeconds % 3600) / 60);");
    httpd_resp_sendstr_chunk(req, "  const s = totalSeconds % 60;");
    httpd_resp_sendstr_chunk(req, "  if (d > 0) return d + ' ngày ' + String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');");
    httpd_resp_sendstr_chunk(req, "  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function formatDateTime(timestamp) {");
    httpd_resp_sendstr_chunk(req, "  const d = new Date(timestamp * 1000);");
    httpd_resp_sendstr_chunk(req, "  return d.toLocaleDateString('vi-VN') + ' ' + d.toLocaleTimeString('vi-VN', {hour:'2-digit', minute:'2-digit'});");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function updateCountdown() {");
    httpd_resp_sendstr_chunk(req, "  const now = Math.floor(Date.now() / 1000);");
    httpd_resp_sendstr_chunk(req, "  const remaining = targetTimestamp - now;");
    httpd_resp_sendstr_chunk(req, "  if (remaining <= 0) {");
    httpd_resp_sendstr_chunk(req, "    clearInterval(countdownInterval);");
    httpd_resp_sendstr_chunk(req, "    scheduleActive = false;");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-countdown').style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('scheduleStartBtn').style.display = 'block';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('scheduleCancelBtn').style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "    const msg = currentScheduleMode === 'alarm' ? '🔔 Báo thức đã reo!' : '✅ Tin nhắn đã được gửi!';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').innerHTML = msg;");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.background = '#e8f5e9';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.color = '#2e7d32';");
    httpd_resp_sendstr_chunk(req, "    updateScheduleMode();");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('countdown-time').textContent = formatTime(remaining);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "async function startSchedule() {");
    httpd_resp_sendstr_chunk(req, "  const mode = currentScheduleMode;");
    httpd_resp_sendstr_chunk(req, "  let message = '';");
    httpd_resp_sendstr_chunk(req, "  if (mode === 'message') {");
    httpd_resp_sendstr_chunk(req, "    message = document.getElementById('schedule_message').value.trim();");
    httpd_resp_sendstr_chunk(req, "    if (!message) {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').innerHTML = '⚠️ Vui lòng nhập tin nhắn!';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "      return;");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  const dateVal = document.getElementById('schedule_date').value;");
    httpd_resp_sendstr_chunk(req, "  const timeVal = document.getElementById('schedule_time').value;");
    httpd_resp_sendstr_chunk(req, "  if (!dateVal || !timeVal) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').innerHTML = '⚠️ Vui lòng chọn ngày và giờ!';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  const targetDate = new Date(dateVal + 'T' + timeVal + ':00');");
    httpd_resp_sendstr_chunk(req, "  const targetTs = Math.floor(targetDate.getTime() / 1000);");
    httpd_resp_sendstr_chunk(req, "  const nowTs = Math.floor(Date.now() / 1000);");
    httpd_resp_sendstr_chunk(req, "  const totalSeconds = targetTs - nowTs;");
    httpd_resp_sendstr_chunk(req, "  if (totalSeconds < 10) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').innerHTML = '⚠️ Thời gian phải ít nhất 10 giây trong tương lai!';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  try {");
    httpd_resp_sendstr_chunk(req, "    const actionSlot = parseInt(document.getElementById('schedule_action_slot').value) || 0;");
    httpd_resp_sendstr_chunk(req, "    const res = await fetch('/schedule_message', {");
    httpd_resp_sendstr_chunk(req, "      method: 'POST',");
    httpd_resp_sendstr_chunk(req, "      headers: {'Content-Type': 'application/json'},");
    httpd_resp_sendstr_chunk(req, "      body: JSON.stringify({ message: message, seconds: totalSeconds, target_timestamp: targetTs, mode: mode, action_slot: actionSlot })");
    httpd_resp_sendstr_chunk(req, "    });");
    httpd_resp_sendstr_chunk(req, "    const data = await res.json();");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      scheduleActive = true;");
    httpd_resp_sendstr_chunk(req, "      targetTimestamp = targetTs;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-countdown').style.display = 'block';");
    httpd_resp_sendstr_chunk(req, "      const modeLabel = mode === 'alarm' ? '🔔 Báo thức lúc: ' : '💬 Gửi tin nhắn lúc: ';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('countdown-mode-label').innerHTML = modeLabel + '<span id=\"target-datetime\">' + formatDateTime(targetTs) + '</span>';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('countdown-time').textContent = formatTime(totalSeconds);");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('scheduleStartBtn').style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('scheduleCancelBtn').style.display = 'block';");
    httpd_resp_sendstr_chunk(req, "      const statusMsg = mode === 'alarm' ? '🔔 Đang đếm ngược... Kiki sẽ reo chuông!' : '💬 Đang đếm ngược... Kiki sẽ gửi tin nhắn!';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').innerHTML = statusMsg;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.background = '#e3f2fd';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.color = '#1976d2';");
    httpd_resp_sendstr_chunk(req, "      countdownInterval = setInterval(updateCountdown, 1000);");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').innerHTML = '❌ Lỗi: ' + data.message;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.color = '#c62828';");;
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  } catch (e) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').innerHTML = '❌ Lỗi kết nối: ' + e.message;");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.background = '#ffebee';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('schedule-status').style.color = '#c62828';");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "async function cancelSchedule() {");
    httpd_resp_sendstr_chunk(req, "  try {");
    httpd_resp_sendstr_chunk(req, "    const res = await fetch('/schedule_message?action=cancel');");
    httpd_resp_sendstr_chunk(req, "    const data = await res.json();");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      if (countdownInterval) clearInterval(countdownInterval);");
    httpd_resp_sendstr_chunk(req, "      scheduleActive = false;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-countdown').style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('scheduleStartBtn').style.display = 'block';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('scheduleCancelBtn').style.display = 'none';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').innerHTML = '⏹️ Đã hủy hẹn giờ';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.background = '#fff3e0';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('schedule-status').style.color = '#e65100';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  } catch (e) {");
    httpd_resp_sendstr_chunk(req, "    console.error('Cancel error:', e);");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "}");
    
    // MQTT Configuration JavaScript functions
    httpd_resp_sendstr_chunk(req, "function saveMqttConfig() {");
    httpd_resp_sendstr_chunk(req, "  const endpoint = document.getElementById('mqttEndpoint').value.trim();");
    httpd_resp_sendstr_chunk(req, "  if (!endpoint) {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('mqttConfigStatus').innerHTML = '❌ Endpoint là bắt buộc!';");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('mqttConfigStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    return;");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "  const config = {");
    httpd_resp_sendstr_chunk(req, "    endpoint: endpoint,");
    httpd_resp_sendstr_chunk(req, "    client_id: document.getElementById('mqttClientId').value.trim(),");
    httpd_resp_sendstr_chunk(req, "    username: document.getElementById('mqttUsername').value.trim(),");
    httpd_resp_sendstr_chunk(req, "    password: document.getElementById('mqttPassword').value.trim(),");
    httpd_resp_sendstr_chunk(req, "    publish_topic: document.getElementById('mqttPublishTopic').value.trim()");
    httpd_resp_sendstr_chunk(req, "  };");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('mqttConfigStatus').innerHTML = '⏳ Đang lưu...';");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('mqttConfigStatus').style.color = '#666';");
    httpd_resp_sendstr_chunk(req, "  fetch('/mqtt_config', {");
    httpd_resp_sendstr_chunk(req, "    method: 'POST',");
    httpd_resp_sendstr_chunk(req, "    headers: {'Content-Type': 'application/json'},");
    httpd_resp_sendstr_chunk(req, "    body: JSON.stringify(config)");
    httpd_resp_sendstr_chunk(req, "  }).then(r => r.json()).then(data => {");
    httpd_resp_sendstr_chunk(req, "    if (data.success) {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').innerHTML = '✅ Cấu hình MQTT đã được lưu thành công! Robot sẽ tự động kết nối lại.';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').style.color = '#4caf50';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttPassword').value = '';");
    httpd_resp_sendstr_chunk(req, "      loadMqttConfig();");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').innerHTML = '❌ Lỗi: ' + data.error;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  }).catch(e => {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('mqttConfigStatus').innerHTML = '❌ Lỗi kết nối: ' + e;");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('mqttConfigStatus').style.color = '#f44336';");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function loadMqttConfig() {");
    httpd_resp_sendstr_chunk(req, "  fetch('/mqtt_config').then(r => r.json()).then(data => {");
    httpd_resp_sendstr_chunk(req, "    if (data.configured) {");
    httpd_resp_sendstr_chunk(req, "      if (data.endpoint) document.getElementById('mqttEndpoint').value = data.endpoint;");
    httpd_resp_sendstr_chunk(req, "      if (data.client_id) document.getElementById('mqttClientId').value = data.client_id;");
    httpd_resp_sendstr_chunk(req, "      if (data.username) document.getElementById('mqttUsername').value = data.username;");
    httpd_resp_sendstr_chunk(req, "      if (data.publish_topic) document.getElementById('mqttPublishTopic').value = data.publish_topic;");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').innerHTML = '✅ MQTT đã được cấu hình. Endpoint: ' + (data.endpoint || 'N/A');");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').style.color = '#4caf50';");
    httpd_resp_sendstr_chunk(req, "    } else {");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').innerHTML = '⚠️ Chưa có cấu hình MQTT. Vui lòng nhập endpoint để kết nối.';");
    httpd_resp_sendstr_chunk(req, "      document.getElementById('mqttConfigStatus').style.color = '#ff9800';");
    httpd_resp_sendstr_chunk(req, "    }");
    httpd_resp_sendstr_chunk(req, "  }).catch(e => {");
    httpd_resp_sendstr_chunk(req, "    console.error('Error loading MQTT config:', e);");
    httpd_resp_sendstr_chunk(req, "  });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Drawing canvas JavaScript
    httpd_resp_sendstr_chunk(req, "var drawCanvas, drawCtx, isDrawing = false;");
    httpd_resp_sendstr_chunk(req, "var currentColor = '#000000';");
    httpd_resp_sendstr_chunk(req, "var brushSize = 5;");
    httpd_resp_sendstr_chunk(req, "var lastX = 0, lastY = 0;");
    
    httpd_resp_sendstr_chunk(req, "function initCanvas() {");
    httpd_resp_sendstr_chunk(req, "  drawCanvas = document.getElementById('drawCanvas');");
    httpd_resp_sendstr_chunk(req, "  if (!drawCanvas) return;");
    httpd_resp_sendstr_chunk(req, "  drawCtx = drawCanvas.getContext('2d');");
    httpd_resp_sendstr_chunk(req, "  drawCtx.fillStyle = '#ffffff';");
    httpd_resp_sendstr_chunk(req, "  drawCtx.fillRect(0, 0, 240, 240);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineCap = 'round';");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineJoin = 'round';");
    // Mouse events
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('mousedown', startDraw);");
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('mousemove', draw);");
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('mouseup', stopDraw);");
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('mouseout', stopDraw);");
    // Touch events
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('touchstart', handleTouchStart, {passive: false});");
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('touchmove', handleTouchMove, {passive: false});");
    httpd_resp_sendstr_chunk(req, "  drawCanvas.addEventListener('touchend', stopDraw);");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function getPos(e) {");
    httpd_resp_sendstr_chunk(req, "  var rect = drawCanvas.getBoundingClientRect();");
    httpd_resp_sendstr_chunk(req, "  var scaleX = 240 / rect.width;");
    httpd_resp_sendstr_chunk(req, "  var scaleY = 240 / rect.height;");
    httpd_resp_sendstr_chunk(req, "  return { x: (e.clientX - rect.left) * scaleX, y: (e.clientY - rect.top) * scaleY };");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function startDraw(e) {");
    httpd_resp_sendstr_chunk(req, "  isDrawing = true;");
    httpd_resp_sendstr_chunk(req, "  var pos = getPos(e);");
    httpd_resp_sendstr_chunk(req, "  lastX = pos.x; lastY = pos.y;");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function draw(e) {");
    httpd_resp_sendstr_chunk(req, "  if (!isDrawing) return;");
    httpd_resp_sendstr_chunk(req, "  var pos = getPos(e);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.strokeStyle = currentColor;");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineWidth = brushSize;");
    httpd_resp_sendstr_chunk(req, "  drawCtx.beginPath();");
    httpd_resp_sendstr_chunk(req, "  drawCtx.moveTo(lastX, lastY);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineTo(pos.x, pos.y);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.stroke();");
    httpd_resp_sendstr_chunk(req, "  lastX = pos.x; lastY = pos.y;");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function stopDraw() { isDrawing = false; }");
    
    httpd_resp_sendstr_chunk(req, "function handleTouchStart(e) {");
    httpd_resp_sendstr_chunk(req, "  e.preventDefault();");
    httpd_resp_sendstr_chunk(req, "  var touch = e.touches[0];");
    httpd_resp_sendstr_chunk(req, "  var rect = drawCanvas.getBoundingClientRect();");
    httpd_resp_sendstr_chunk(req, "  var scaleX = 240 / rect.width;");
    httpd_resp_sendstr_chunk(req, "  var scaleY = 240 / rect.height;");
    httpd_resp_sendstr_chunk(req, "  isDrawing = true;");
    httpd_resp_sendstr_chunk(req, "  lastX = (touch.clientX - rect.left) * scaleX;");
    httpd_resp_sendstr_chunk(req, "  lastY = (touch.clientY - rect.top) * scaleY;");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function handleTouchMove(e) {");
    httpd_resp_sendstr_chunk(req, "  e.preventDefault();");
    httpd_resp_sendstr_chunk(req, "  if (!isDrawing) return;");
    httpd_resp_sendstr_chunk(req, "  var touch = e.touches[0];");
    httpd_resp_sendstr_chunk(req, "  var rect = drawCanvas.getBoundingClientRect();");
    httpd_resp_sendstr_chunk(req, "  var scaleX = 240 / rect.width;");
    httpd_resp_sendstr_chunk(req, "  var scaleY = 240 / rect.height;");
    httpd_resp_sendstr_chunk(req, "  var x = (touch.clientX - rect.left) * scaleX;");
    httpd_resp_sendstr_chunk(req, "  var y = (touch.clientY - rect.top) * scaleY;");
    httpd_resp_sendstr_chunk(req, "  drawCtx.strokeStyle = currentColor;");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineWidth = brushSize;");
    httpd_resp_sendstr_chunk(req, "  drawCtx.beginPath();");
    httpd_resp_sendstr_chunk(req, "  drawCtx.moveTo(lastX, lastY);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.lineTo(x, y);");
    httpd_resp_sendstr_chunk(req, "  drawCtx.stroke();");
    httpd_resp_sendstr_chunk(req, "  lastX = x; lastY = y;");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function setColor(color) { currentColor = color; }");
    httpd_resp_sendstr_chunk(req, "function updateBrushSize() {");
    httpd_resp_sendstr_chunk(req, "  brushSize = document.getElementById('brushSize').value;");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('brushSizeValue').textContent = brushSize + 'px';");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function clearCanvas() {");
    httpd_resp_sendstr_chunk(req, "  drawCtx.fillStyle = '#ffffff';");
    httpd_resp_sendstr_chunk(req, "  drawCtx.fillRect(0, 0, 240, 240);");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('response4').textContent = '🗑️ Đã xóa canvas!';");
    httpd_resp_sendstr_chunk(req, "}");
    
    httpd_resp_sendstr_chunk(req, "function sendDrawing() {");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('response4').textContent = '📤 Đang gửi...';");
    httpd_resp_sendstr_chunk(req, "  var imageData = drawCtx.getImageData(0, 0, 240, 240);");
    httpd_resp_sendstr_chunk(req, "  var data = imageData.data;");
    // Convert RGBA to RGB565
    httpd_resp_sendstr_chunk(req, "  var rgb565 = new Uint16Array(240 * 240);");
    httpd_resp_sendstr_chunk(req, "  for (var i = 0; i < 240 * 240; i++) {");
    httpd_resp_sendstr_chunk(req, "    var r = data[i * 4] >> 3;");
    httpd_resp_sendstr_chunk(req, "    var g = data[i * 4 + 1] >> 2;");
    httpd_resp_sendstr_chunk(req, "    var b = data[i * 4 + 2] >> 3;");
    httpd_resp_sendstr_chunk(req, "    rgb565[i] = (r << 11) | (g << 5) | b;");
    httpd_resp_sendstr_chunk(req, "  }");
    // Send as binary
    httpd_resp_sendstr_chunk(req, "  fetch('/draw', { method: 'POST', body: rgb565.buffer, headers: {'Content-Type': 'application/octet-stream'} })");
    httpd_resp_sendstr_chunk(req, "  .then(r => r.text())");
    httpd_resp_sendstr_chunk(req, "  .then(d => { document.getElementById('response4').textContent = '✅ ' + d; })");
    httpd_resp_sendstr_chunk(req, "  .catch(e => { document.getElementById('response4').textContent = '❌ Lỗi: ' + e; });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Exit drawing mode and return to emoji
    httpd_resp_sendstr_chunk(req, "function exitDrawing() {");
    httpd_resp_sendstr_chunk(req, "  document.getElementById('response4').textContent = '↩️ Đang quay lại...';");
    httpd_resp_sendstr_chunk(req, "  fetch('/draw_exit').then(r => r.text()).then(d => {");
    httpd_resp_sendstr_chunk(req, "    document.getElementById('response4').textContent = '✅ ' + d;");
    httpd_resp_sendstr_chunk(req, "  }).catch(e => { document.getElementById('response4').textContent = '❌ Lỗi: ' + e; });");
    httpd_resp_sendstr_chunk(req, "}");
    
    // Initialize volume slider and schedule date/time
    httpd_resp_sendstr_chunk(req, "window.onload = function() {");
    httpd_resp_sendstr_chunk(req, "  loadMqttConfig();");
    httpd_resp_sendstr_chunk(req, "  initScheduleDateTime();");
    httpd_resp_sendstr_chunk(req, "  initCanvas();");
    httpd_resp_sendstr_chunk(req, "  var slider = document.getElementById('volumeSlider');");
    httpd_resp_sendstr_chunk(req, "  var output = document.getElementById('volumeValue');");
    httpd_resp_sendstr_chunk(req, "  slider.oninput = function() {");
    httpd_resp_sendstr_chunk(req, "    output.innerHTML = this.value + '%';");
    httpd_resp_sendstr_chunk(req, "    setVolume(this.value);");
    httpd_resp_sendstr_chunk(req, "  }");
    httpd_resp_sendstr_chunk(req, "};");
    httpd_resp_sendstr_chunk(req, "</script>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    
    httpd_resp_sendstr_chunk(req, NULL); // End of chunks
}

// Root page handler
esp_err_t otto_root_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Root page requested");
    webserver_reset_auto_stop_timer();  // Reset 5-minute timer on page access
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
        ret = otto_controller_queue_action(ACTION_HOME, 1, param2, 0, 0);
        ESP_LOGI(TAG, "🏠 Going to home position with speed %d", param2);
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
    } else if (strstr(action, "show_clock")) {
        // Show clock on display
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
            if (otto_display) {
                int duration_sec = param1 > 0 ? param1 : 10;  // Default 10 seconds
                otto_display->ShowClock(duration_sec * 1000);
                ESP_LOGI(TAG, "⏰ Showing clock for %d seconds", duration_sec);
            }
        }
        ret = ESP_OK;
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
    webserver_reset_auto_stop_timer();  // Reset 5-minute timer on action
    
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
        
        // Apply speed multiplier: lower multiplier = faster (less delay)
        // speed_multiplier: 50 = fastest (50%), 100 = normal, 200 = slowest (200%)
        int adjusted_speed = (param2 * speed_multiplier) / 100;
        if (adjusted_speed < 10) adjusted_speed = 10;  // Minimum speed
        
        ESP_LOGI(TAG, "Action: %s, P1: %d, P2: %d (speed_mult: %d%% -> adjusted: %d)", 
                 cmd, param1, param2, speed_multiplier, adjusted_speed);
        
        // Execute action with adjusted speed
        otto_execute_web_action(cmd, param1, adjusted_speed);
        
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
    webserver_reset_auto_stop_timer();  // Reset timer on status check
    httpd_resp_set_type(req, "text/plain");
    
    // Simple status - can be expanded with actual Otto status
    httpd_resp_sendstr(req, "ready");
    
    return ESP_OK;
}

// Emotion handler
esp_err_t otto_emotion_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "😊 EMOTION HANDLER CALLED!"); // Debug logging
    webserver_reset_auto_stop_timer();  // Reset timer on emotion change
    
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

// Save action slot handler
esp_err_t otto_save_slot_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "💾 SAVE SLOT HANDLER CALLED!");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char query[600] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing parameters\"}");
        return ESP_OK;
    }
    
    char slot_str[10] = {0};
    char actions[512] = {0};
    char emotion[32] = {0};
    
    httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str));
    httpd_query_key_value(query, "actions", actions, sizeof(actions));
    httpd_query_key_value(query, "emotion", emotion, sizeof(emotion));
    
    int slot = atoi(slot_str);
    if (slot < 1 || slot > 3) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid slot number\"}");
        return ESP_OK;
    }
    
    // Decode URL-encoded actions string (replace %XX with actual characters)
    // For simplicity, handle common cases: %2C -> comma, %3B -> semicolon
    char decoded_actions[512] = {0};
    int j = 0;
    for (size_t i = 0; i < strlen(actions) && j < sizeof(decoded_actions) - 1; i++) {
        if (actions[i] == '%' && i + 2 < strlen(actions)) {
            char hex[3] = {actions[i+1], actions[i+2], '\0'};
            int ch = (int)strtol(hex, NULL, 16);
            decoded_actions[j++] = (char)ch;
            i += 2;
        } else {
            decoded_actions[j++] = actions[i];
        }
    }
    
    // Save to memory slot (slot numbers are 1-3, array is 0-2)
    int idx = slot - 1;
    strncpy(memory_slots[idx].actions, decoded_actions, sizeof(memory_slots[idx].actions) - 1);
    strncpy(memory_slots[idx].emotion, emotion, sizeof(memory_slots[idx].emotion) - 1);
    memory_slots[idx].used = true;
    
    // Count actions (separated by ';')
    int count = 0;
    if (strlen(decoded_actions) > 0) {
        count = 1;
        for (size_t i = 0; i < strlen(decoded_actions); i++) {
            if (decoded_actions[i] == ';') count++;
        }
    }
    
    ESP_LOGI(TAG, "💾 Saved %d actions to slot %d: '%s' with emotion '%s'", count, slot, decoded_actions, emotion);
    
    // Save to NVS for persistence
    save_memory_slots_to_nvs();
    
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"slot\":%d,\"count\":%d,\"emotion\":\"%s\"}", 
             slot, count, emotion);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// Play action slot handler
esp_err_t otto_play_slot_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "▶️ PLAY SLOT HANDLER CALLED!");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Missing slot parameter\"}");
        return ESP_OK;
    }
    
    char slot_str[10] = {0};
    httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str));
    
    int slot = atoi(slot_str);
    if (slot < 1 || slot > 3) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid slot number\"}");
        return ESP_OK;
    }
    
    int idx = slot - 1;
    if (!memory_slots[idx].used || strlen(memory_slots[idx].actions) == 0) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Vị trí này chưa có dữ liệu\"}");
        return ESP_OK;
    }
    
    // Set emotion first
    if (strlen(memory_slots[idx].emotion) > 0) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
            if (otto_display) {
                otto_display->SetEmotion(memory_slots[idx].emotion);
            } else {
                display->SetEmotion(memory_slots[idx].emotion);
            }
        }
        ESP_LOGI(TAG, "▶️ Set emotion: %s", memory_slots[idx].emotion);
    }
    
    // Parse and execute actions with delay between each
    char actions_copy[512];
    strncpy(actions_copy, memory_slots[idx].actions, sizeof(actions_copy) - 1);
    actions_copy[sizeof(actions_copy) - 1] = '\0';
    
    ESP_LOGI(TAG, "▶️ Actions string: '%s'", actions_copy);
    
    int count = 0;
    char* token = strtok(actions_copy, ";");
    while (token != NULL) {
        // Parse "action,param1,param2,emoji"
        char action[50] = {0};
        char emoji[32] = {0};
        int p1 = 0, p2 = 0;
        
        // Try to parse with emoji first (new format)
        int parsed = sscanf(token, "%[^,],%d,%d,%[^\n]", action, &p1, &p2, emoji);
        if (parsed < 3) {
            // Fallback to old format without emoji
            parsed = sscanf(token, "%[^,],%d,%d", action, &p1, &p2);
            strcpy(emoji, "neutral");
        }
        
        if (parsed >= 1) {
            ESP_LOGI(TAG, "▶️ Action %d: '%s' (p1:%d, p2:%d, emoji:%s)", count+1, action, p1, p2, emoji);
            
            // Set emoji for this action
            if (strlen(emoji) > 0) {
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                    if (otto_display) {
                        otto_display->SetEmotion(emoji);
                    } else {
                        display->SetEmotion(emoji);
                    }
                }
            }
            
            // Check if this is an emoji-only action (no movement)
            if (strcmp(action, "emoji") == 0) {
                ESP_LOGI(TAG, "▶️ Emoji change: %s", emoji);
                count++;
            } else {
                // Apply speed multiplier only if p2 > 0 (it's a speed parameter)
                int adjusted_speed = p2;
                if (p2 > 0) {
                    adjusted_speed = (p2 * speed_multiplier) / 100;
                    if (adjusted_speed < 10) adjusted_speed = 10;
                }
                
                // Execute action
                otto_execute_web_action(action, p1, adjusted_speed);
                count++;
                
                // Add delay between actions to ensure robot completes previous action
                // Stop action: no delay
                // Other actions: 100ms delay
                if (strstr(action, "stop") == NULL) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            ESP_LOGW(TAG, "▶️ Failed to parse token: '%s'", token);
        }
        
        token = strtok(NULL, ";");
    }
    
    ESP_LOGI(TAG, "▶️ Completed: Played %d actions from slot %d", count, slot);
    
    char response[100];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"slot\":%d,\"count\":%d}", 
             slot, count);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// Get slot info handler
esp_err_t otto_slot_info_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char query[100] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"used\":false}");
        return ESP_OK;
    }
    
    char slot_str[10] = {0};
    httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str));
    
    int slot = atoi(slot_str);
    if (slot < 1 || slot > 3) {
        httpd_resp_sendstr(req, "{\"used\":false}");
        return ESP_OK;
    }
    
    int idx = slot - 1;
    if (!memory_slots[idx].used) {
        httpd_resp_sendstr(req, "{\"used\":false}");
        return ESP_OK;
    }
    
    // Count actions
    int count = 1;
    for (size_t i = 0; i < strlen(memory_slots[idx].actions); i++) {
        if (memory_slots[idx].actions[i] == ';') count++;
    }
    
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"used\":true,\"count\":%d,\"emotion\":\"%s\"}", 
             count, memory_slots[idx].emotion);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// Play memory slot directly - can be called from other modules like application.cc
// slot: 1-3 (slot number)
// Returns: number of actions played, 0 if slot empty or invalid
int otto_play_memory_slot(int slot) {
    ESP_LOGI(TAG, "🎭 otto_play_memory_slot(%d) called", slot);
    
    if (slot < 1 || slot > 3) {
        ESP_LOGW(TAG, "❌ Invalid slot number: %d", slot);
        return 0;
    }
    
    int idx = slot - 1;
    if (!memory_slots[idx].used || strlen(memory_slots[idx].actions) == 0) {
        ESP_LOGW(TAG, "❌ Slot %d is empty", slot);
        return 0;
    }
    
    // Set emotion first
    if (strlen(memory_slots[idx].emotion) > 0) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
            if (otto_display) {
                otto_display->SetEmotion(memory_slots[idx].emotion);
            } else {
                display->SetEmotion(memory_slots[idx].emotion);
            }
        }
        ESP_LOGI(TAG, "▶️ Set emotion: %s", memory_slots[idx].emotion);
    }
    
    // Parse and execute actions with delay between each
    char actions_copy[512];
    strncpy(actions_copy, memory_slots[idx].actions, sizeof(actions_copy) - 1);
    actions_copy[sizeof(actions_copy) - 1] = '\0';
    
    ESP_LOGI(TAG, "▶️ Actions string: '%s'", actions_copy);
    
    int count = 0;
    char* saveptr = NULL;
    char* token = strtok_r(actions_copy, ";", &saveptr);
    while (token != NULL) {
        // Parse "action,param1,param2,emoji"
        char action[50] = {0};
        char emoji[32] = {0};
        int p1 = 0, p2 = 0;
        
        // Try to parse with emoji first (new format)
        int parsed = sscanf(token, "%[^,],%d,%d,%[^\n]", action, &p1, &p2, emoji);
        if (parsed < 3) {
            // Fallback to old format without emoji
            parsed = sscanf(token, "%[^,],%d,%d", action, &p1, &p2);
            strcpy(emoji, "neutral");
        }
        
        if (parsed >= 1) {
            ESP_LOGI(TAG, "▶️ Action %d: '%s' (p1:%d, p2:%d, emoji:%s)", count+1, action, p1, p2, emoji);
            
            // Set emoji for this action
            if (strlen(emoji) > 0) {
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                    if (otto_display) {
                        otto_display->SetEmotion(emoji);
                    } else {
                        display->SetEmotion(emoji);
                    }
                }
            }
            
            // Check if this is an emoji-only action (no movement)
            if (strcmp(action, "emoji") == 0) {
                ESP_LOGI(TAG, "▶️ Emoji change: %s", emoji);
                count++;
            } else {
                // Apply speed multiplier only if p2 > 0 (it's a speed parameter)
                int adjusted_speed = p2;
                if (p2 > 0) {
                    adjusted_speed = (p2 * speed_multiplier) / 100;
                    if (adjusted_speed < 10) adjusted_speed = 10;
                }
                
                // Execute action
                otto_execute_web_action(action, p1, adjusted_speed);
                count++;
                
                // Add delay between actions to ensure robot completes previous action
                // Stop action: no delay
                // Other actions: 100ms delay
                if (strstr(action, "stop") == NULL) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            ESP_LOGW(TAG, "▶️ Failed to parse token: '%s'", token);
        }
        
        token = strtok_r(NULL, ";", &saveptr);
    }
    
    ESP_LOGI(TAG, "▶️ Completed: Played %d actions from slot %d", count, slot);
    return count;
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
                // Use text emoji mode
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    otto_display->SetEmojiMode(false); // Set to text emoji mode
                    otto_display->SetEmotion("neutral"); // Set neutral text emoji
                } else {
                    display->SetEmotion("neutral"); // Fallback for non-Otto displays
                }
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "✅ Emoji mode set to: Default Text");
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

#ifdef TOUCH_TTP223_GPIO
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
#endif // TOUCH_TTP223_GPIO

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

// Screen rotation handler - set display rotation (0°, 90°, 180°, 270°)
esp_err_t otto_screen_rotation_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🔄 SCREEN ROTATION HANDLER CALLED!");
    webserver_reset_auto_stop_timer();

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Parse query parameters for rotation angle
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char angle_str[16] = {0};
        char save_str[8] = {0};
        
        // Get rotation angle parameter
        if (httpd_query_key_value(query, "angle", angle_str, sizeof(angle_str)) == ESP_OK) {
            int rotation_angle = atoi(angle_str);
            
            // Validate rotation angle (only 0, 90, 180, 270 degrees allowed)
            if (rotation_angle != 0 && rotation_angle != 90 && rotation_angle != 180 && rotation_angle != 270) {
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "❌ Góc xoay không hợp lệ. Chỉ chấp nhận: 0, 90, 180, 270");
                return ESP_OK;
            }
            
            // Check if should save to NVS
            bool should_save = false;
            if (httpd_query_key_value(query, "save", save_str, sizeof(save_str)) == ESP_OK) {
                should_save = (atoi(save_str) == 1);
            }
            
            ESP_LOGI(TAG, "🔄 Setting screen rotation to: %d degrees (save=%d)", rotation_angle, should_save);
            
            // Save rotation to NVS only if requested
            if (should_save) {
                nvs_handle_t nvs_handle;
                esp_err_t err = nvs_open("display", NVS_READWRITE, &nvs_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "❌ Failed to open NVS: %s", esp_err_to_name(err));
                } else {
                    err = nvs_set_i32(nvs_handle, "rotation", rotation_angle);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "❌ Failed to set rotation in NVS: %s", esp_err_to_name(err));
                    } else {
                        err = nvs_commit(nvs_handle);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "❌ Failed to commit NVS: %s", esp_err_to_name(err));
                        } else {
                            ESP_LOGI(TAG, "💾 Rotation saved to NVS: %d°", rotation_angle);
                        }
                    }
                    nvs_close(nvs_handle);
                }
            }
            
            // Apply rotation settings based on angle
            bool mirror_x = DISPLAY_MIRROR_X;
            bool mirror_y = DISPLAY_MIRROR_Y;
            bool swap_xy = DISPLAY_SWAP_XY;
            
            // Calculate mirror and swap settings based on rotation
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
            }
            
            // Get display instance and apply rotation
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    // Get the panel handle from display
                    esp_lcd_panel_handle_t panel = otto_display->GetPanel();
                    if (panel) {
                        // Apply rotation settings to LCD panel
                        esp_lcd_panel_swap_xy(panel, swap_xy);
                        esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
                        
                        ESP_LOGI(TAG, "✅ Screen rotation applied: %d° (swap_xy=%d, mirror_x=%d, mirror_y=%d)", 
                                 rotation_angle, swap_xy, mirror_x, mirror_y);
                        
                        // Refresh display to show changes
                        otto_display->SetEmotion("happy");
                        
                        httpd_resp_set_type(req, "text/plain");
                        char response[128];
                        if (should_save) {
                            snprintf(response, sizeof(response), "✅ Đã lưu xoay màn hình: %d° (giữ sau reboot)", rotation_angle);
                        } else {
                            snprintf(response, sizeof(response), "🔄 Xoay màn hình: %d° (chưa lưu)", rotation_angle);
                        }
                        httpd_resp_sendstr(req, response);
                        return ESP_OK;
                    }
                }
            }
            
            ESP_LOGE(TAG, "❌ Failed to get display or panel handle");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "❌ Không thể truy cập màn hình");
            return ESP_OK;
        }
    }

    // No angle parameter or invalid request
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "❌ Thiếu tham số 'angle'");
    return ESP_OK;
}

// Drawing handler - receive RGB565 image data from web and display on robot LCD
esp_err_t otto_draw_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎨 DRAW HANDLER CALLED!");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Expected size: 240x240 pixels * 2 bytes per pixel (RGB565) = 115200 bytes
    static const size_t EXPECTED_SIZE = 240 * 240 * 2;
    size_t content_len = req->content_len;
    
    ESP_LOGI(TAG, "🎨 Received drawing data: %d bytes (expected %d)", (int)content_len, (int)EXPECTED_SIZE);
    
    if (content_len != EXPECTED_SIZE) {
        ESP_LOGE(TAG, "Invalid drawing data size: %d (expected %d)", (int)content_len, (int)EXPECTED_SIZE);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Kích thước dữ liệu không đúng");
        return ESP_FAIL;
    }
    
    // Lazy init draw buffer on first use
    if (!init_draw_buffer()) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Không đủ bộ nhớ");
        return ESP_FAIL;
    }
    
    // Dùng static buffer thay vì malloc - tránh fragmentation
    if (xSemaphoreTake(draw_buffer_pool.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Draw buffer busy, request dropped");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Bận xử lý, thử lại");
        return ESP_FAIL;
    }
    
    if (draw_buffer_pool.in_use) {
        xSemaphoreGive(draw_buffer_pool.mutex);
        ESP_LOGW(TAG, "Draw buffer in use");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Đang xử lý, thử lại");
        return ESP_FAIL;
    }
    
    draw_buffer_pool.in_use = true;
    xSemaphoreGive(draw_buffer_pool.mutex);
    
    // Read image data vào static buffer
    size_t received = 0;
    while (received < EXPECTED_SIZE) {
        int ret = httpd_req_recv(req, (char*)draw_buffer_pool.buffer + received, EXPECTED_SIZE - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive drawing data");
            xSemaphoreTake(draw_buffer_pool.mutex, portMAX_DELAY);
            draw_buffer_pool.in_use = false;
            xSemaphoreGive(draw_buffer_pool.mutex);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Lỗi: Không nhận được dữ liệu");
            return ESP_FAIL;
        }
        received += ret;
    }
    
    ESP_LOGI(TAG, "🎨 Received %d bytes of drawing data", (int)received);
    
    // Get display and show the drawing
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        OttoEmojiDisplay* otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
        if (otto_display) {
            otto_display->SetDrawingImage((uint16_t*)draw_buffer_pool.buffer, 240, 240);
            ESP_LOGI(TAG, "🎨 Drawing displayed on robot LCD!");
            
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Đã hiển thị hình vẽ lên robot!");
        } else {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Lỗi: Display không hỗ trợ");
        }
    } else {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Không có display");
    }
    
    // Release buffer
    xSemaphoreTake(draw_buffer_pool.mutex, portMAX_DELAY);
    draw_buffer_pool.in_use = false;
    xSemaphoreGive(draw_buffer_pool.mutex);
    return ESP_OK;
}

// Exit drawing mode and return to emoji display
esp_err_t otto_draw_exit_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "↩️ EXIT DRAWING MODE CALLED!");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Get display and disable drawing canvas
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        OttoEmojiDisplay* otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
        if (otto_display) {
            otto_display->EnableDrawingCanvas(false);
            ESP_LOGI(TAG, "↩️ Drawing canvas disabled, returning to emoji!");
            
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Đã quay lại hiển thị emoji!");
        } else {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "Lỗi: Display không hỗ trợ");
        }
    } else {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Lỗi: Không có display");
    }
    
    return ESP_OK;
}

// ============= MUSIC PLAYER HANDLERS =============
// Music player page handler
esp_err_t otto_music_page_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎵 Music page requested");
    webserver_reset_auto_stop_timer();
    
    // HTML page for music player with search and playback - Modern UI with YouTube Thumbnail
    static const char MUSIC_PAGE[] = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>🎵 Kiki Music Player</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { 
    font-family: 'Segoe UI', system-ui, sans-serif; 
    background: linear-gradient(135deg, #0f0c29 0%, #302b63 50%, #24243e 100%);
    color: #fff; 
    min-height: 100vh;
    padding: 10px;
}
.container { max-width: 500px; margin: 0 auto; }
h1 { 
    text-align: center; 
    font-size: 1.6em; 
    margin: 10px 0;
    background: linear-gradient(90deg, #f953c6, #b91d73, #f953c6);
    background-size: 200% auto;
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    animation: shine 3s linear infinite;
}
@keyframes shine { to { background-position: 200% center; } }

.search-box {
    display: flex;
    gap: 8px;
    margin: 12px 0;
}
.search-box input {
    flex: 1;
    padding: 14px 18px;
    border: none;
    border-radius: 30px;
    font-size: 15px;
    background: rgba(255,255,255,0.15);
    color: #fff;
    outline: none;
    transition: all 0.3s;
}
.search-box input:focus {
    background: rgba(255,255,255,0.25);
    box-shadow: 0 0 20px rgba(249,83,198,0.4);
}
.search-box input::placeholder { color: rgba(255,255,255,0.5); }
.btn-search {
    padding: 14px 22px;
    border: none;
    border-radius: 30px;
    font-size: 16px;
    cursor: pointer;
    background: linear-gradient(135deg, #f953c6, #b91d73);
    color: #fff;
    font-weight: bold;
    transition: all 0.3s;
}
.btn-search:hover { transform: scale(1.05); box-shadow: 0 5px 25px rgba(249,83,198,0.5); }
.btn-search:active { transform: scale(0.95); }

.player-card {
    background: linear-gradient(145deg, rgba(255,255,255,0.1) 0%, rgba(255,255,255,0.05) 100%);
    border-radius: 25px;
    padding: 20px;
    margin: 15px 0;
    backdrop-filter: blur(15px);
    border: 1px solid rgba(255,255,255,0.15);
    box-shadow: 0 10px 40px rgba(0,0,0,0.3);
}

.thumbnail-container {
    position: relative;
    width: 100%;
    padding-bottom: 56.25%;
    border-radius: 15px;
    overflow: hidden;
    margin-bottom: 15px;
    background: linear-gradient(135deg, #1a1a2e 0%, #302b63 100%);
}
.thumbnail {
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    object-fit: cover;
    transition: transform 0.5s, opacity 0.5s;
}
.thumbnail.loading { opacity: 0.5; }
.thumbnail-overlay {
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: linear-gradient(transparent 50%, rgba(0,0,0,0.8) 100%);
    pointer-events: none;
}
.vinyl-animation {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    width: 80px;
    height: 80px;
    border-radius: 50%;
    background: radial-gradient(circle at 30% 30%, #333 0%, #111 100%);
    border: 3px solid #444;
    display: none;
}
.vinyl-animation.playing {
    display: block;
    animation: spin 3s linear infinite;
}
.vinyl-animation::before {
    content: '';
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: linear-gradient(135deg, #f953c6, #b91d73);
}
@keyframes spin { to { transform: translate(-50%, -50%) rotate(360deg); } }

.song-info {
    text-align: center;
    margin-bottom: 15px;
}
.song-title {
    font-size: 1.3em;
    font-weight: bold;
    margin-bottom: 5px;
    color: #fff;
    text-shadow: 0 2px 10px rgba(0,0,0,0.3);
}
.song-artist {
    color: rgba(255,255,255,0.7);
    font-size: 0.95em;
}

.progress-bar {
    width: 100%;
    height: 4px;
    background: rgba(255,255,255,0.2);
    border-radius: 2px;
    margin: 15px 0;
    overflow: hidden;
}
.progress-fill {
    height: 100%;
    width: 0%;
    background: linear-gradient(90deg, #f953c6, #b91d73);
    border-radius: 2px;
    transition: width 0.3s;
}

.player-controls {
    display: flex;
    justify-content: center;
    align-items: center;
    gap: 20px;
}
.ctrl-btn {
    width: 55px;
    height: 55px;
    border-radius: 50%;
    border: none;
    font-size: 22px;
    cursor: pointer;
    transition: all 0.3s;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(255,255,255,0.1);
    color: #fff;
}
.ctrl-btn.stop {
    width: 65px;
    height: 65px;
    font-size: 26px;
    background: linear-gradient(135deg, #ff416c, #ff4b2b);
}
.ctrl-btn:hover { transform: scale(1.1); box-shadow: 0 5px 20px rgba(255,65,108,0.4); }
.ctrl-btn:active { transform: scale(0.95); }

.status-bar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 10px 15px;
    background: rgba(0,0,0,0.2);
    border-radius: 12px;
    margin: 10px 0;
    font-size: 0.85em;
}
.status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 8px;
    display: inline-block;
}
.status-dot.idle { background: #7f8c8d; }
.status-dot.playing { background: #2ecc71; animation: pulse 1s infinite; }
.status-dot.loading { background: #f39c12; animation: pulse 0.5s infinite; }
@keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }

.section-title {
    color: #f953c6;
    font-size: 0.9em;
    margin: 15px 0 10px 0;
    display: flex;
    align-items: center;
    gap: 6px;
}

.quick-songs {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
}
.quick-btn {
    padding: 10px 16px;
    background: rgba(255,255,255,0.08);
    border: 1px solid rgba(255,255,255,0.15);
    border-radius: 25px;
    color: #fff;
    font-size: 0.85em;
    cursor: pointer;
    transition: all 0.3s;
}
.quick-btn:hover {
    background: linear-gradient(135deg, rgba(249,83,198,0.3), rgba(185,29,115,0.3));
    border-color: #f953c6;
    transform: translateY(-2px);
}

.history-list {
    max-height: 180px;
    overflow-y: auto;
}
.history-item {
    display: flex;
    align-items: center;
    padding: 10px 12px;
    background: rgba(255,255,255,0.05);
    border-radius: 10px;
    margin-bottom: 6px;
    cursor: pointer;
    transition: all 0.3s;
}
.history-item:hover {
    background: rgba(249,83,198,0.2);
    transform: translateX(5px);
}
.history-thumb {
    width: 45px;
    height: 45px;
    border-radius: 8px;
    margin-right: 12px;
    object-fit: cover;
    background: #333;
}
.history-info { flex: 1; }
.history-info .song { font-size: 0.9em; margin-bottom: 2px; }
.history-info .time { color: rgba(255,255,255,0.5); font-size: 0.75em; }

.back-btn {
    display: block;
    text-align: center;
    padding: 14px;
    background: rgba(255,255,255,0.08);
    border-radius: 15px;
    color: #fff;
    text-decoration: none;
    margin-top: 15px;
    transition: all 0.3s;
    border: 1px solid rgba(255,255,255,0.1);
}
.back-btn:hover { background: rgba(255,255,255,0.15); }

.toast {
    position: fixed;
    bottom: 20px;
    left: 50%;
    transform: translateX(-50%) translateY(100px);
    background: linear-gradient(135deg, #f953c6, #b91d73);
    color: #fff;
    padding: 12px 25px;
    border-radius: 25px;
    transition: transform 0.3s;
    z-index: 1000;
    font-weight: bold;
    box-shadow: 0 5px 25px rgba(249,83,198,0.4);
}
.toast.show { transform: translateX(-50%) translateY(0); }
</style>
</head>
<body>
<div class="container">
    <h1>🎵 Kiki Music</h1>
    
    <div class="search-box">
        <input type="text" id="searchInput" placeholder="Tìm bài hát, ca sĩ..." autocomplete="off">
        <button class="btn-search" onclick="searchMusic()">🔍</button>
    </div>
    
    <div class="player-card">
        <div class="thumbnail-container">
            <img class="thumbnail" id="thumbnail" src="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 56'%3E%3Crect fill='%23302b63' width='100' height='56'/%3E%3Ctext x='50' y='30' font-size='12' fill='%23fff' text-anchor='middle'%3E🎵%3C/text%3E%3C/svg%3E" alt="thumbnail">
            <div class="thumbnail-overlay"></div>
            <div class="vinyl-animation" id="vinyl"></div>
        </div>
        <div class="song-info">
            <div class="song-title" id="songTitle">Chọn bài hát để phát</div>
            <div class="song-artist" id="songArtist">Kiki Music Player</div>
        </div>
        <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
        <div class="player-controls">
            <button class="ctrl-btn stop" onclick="stopMusic()">⏹️</button>
        </div>
    </div>
    
    <div class="status-bar">
        <span><span class="status-dot idle" id="statusDot"></span><span id="statusText">Sẵn sàng</span></span>
        <span id="bufferInfo"></span>
    </div>
    
    <div class="section-title">🔥 Đề xuất</div>
    <div class="quick-songs">
        <button class="quick-btn" onclick="playQuick('Chúng Ta Của Hiện Tại')">Chúng Ta Của Hiện Tại</button>
        <button class="quick-btn" onclick="playQuick('Lạc Trôi')">Lạc Trôi</button>
        <button class="quick-btn" onclick="playQuick('See You Again')">See You Again</button>
        <button class="quick-btn" onclick="playQuick('Despacito')">Despacito</button>
        <button class="quick-btn" onclick="playQuick('Shape of You')">Shape of You</button>
        <button class="quick-btn" onclick="playQuick('Có Chắc Yêu Là Đây')">Có Chắc Yêu Là Đây</button>
    </div>
    
    <div class="section-title">📜 Lịch sử</div>
    <div class="history-list" id="historyList"></div>
    
    <a href="/" class="back-btn">⬅️ Quay lại trang chính</a>
</div>

<div class="toast" id="toast"></div>

<script>
let isPlaying = false;
let currentSong = '';
let currentThumbnail = '';
let progressInterval = null;
let history = JSON.parse(localStorage.getItem('musicHistory') || '[]');
const defaultThumb = "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 56'%3E%3Crect fill='%23302b63' width='100' height='56'/%3E%3Ctext x='50' y='30' font-size='12' fill='%23fff' text-anchor='middle'%3E🎵%3C/text%3E%3C/svg%3E";

function showToast(msg) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 2500);
}

function setStatus(status, text) {
    const dot = document.getElementById('statusDot');
    const txt = document.getElementById('statusText');
    dot.className = 'status-dot ' + status;
    txt.textContent = text;
}

function setThumbnail(url) {
    const thumb = document.getElementById('thumbnail');
    const vinyl = document.getElementById('vinyl');
    thumb.classList.add('loading');
    if (url && url.length > 10) {
        thumb.src = url;
        thumb.onload = () => { thumb.classList.remove('loading'); vinyl.classList.remove('playing'); };
        thumb.onerror = () => { thumb.src = defaultThumb; thumb.classList.remove('loading'); };
        currentThumbnail = url;
    } else {
        thumb.src = defaultThumb;
        thumb.classList.remove('loading');
        vinyl.classList.add('playing');
    }
}

function updateUI(playing, song, artist, thumbnail) {
    isPlaying = playing;
    document.getElementById('songTitle').textContent = song || 'Chọn bài hát để phát';
    document.getElementById('songArtist').textContent = artist || 'Kiki Music Player';
    const vinyl = document.getElementById('vinyl');
    if (playing) {
        setStatus('playing', 'Đang phát');
        currentSong = song;
        vinyl.classList.add('playing');
        startProgress();
        if (thumbnail) setThumbnail(thumbnail);
    } else {
        setStatus('idle', 'Sẵn sàng');
        vinyl.classList.remove('playing');
        stopProgress();
        setThumbnail('');
    }
}

function startProgress() {
    stopProgress();
    let progress = 0;
    const fill = document.getElementById('progressFill');
    progressInterval = setInterval(() => {
        progress += 0.5;
        if (progress > 100) progress = 0;
        fill.style.width = progress + '%';
    }, 500);
}

function stopProgress() {
    if (progressInterval) { clearInterval(progressInterval); progressInterval = null; }
    document.getElementById('progressFill').style.width = '0%';
}

function addToHistory(song, thumbnail) {
    if (!song) return;
    history = history.filter(h => h.song !== song);
    history.unshift({ song: song, time: new Date().toLocaleTimeString(), thumb: thumbnail || '' });
    if (history.length > 10) history.pop();
    localStorage.setItem('musicHistory', JSON.stringify(history));
    renderHistory();
}

function renderHistory() {
    const list = document.getElementById('historyList');
    let html = '';
    history.forEach(h => {
        const thumbUrl = h.thumb || defaultThumb;
        html += `<div class="history-item" onclick="playQuick('${h.song.replace(/'/g, "\\'")}')">
            <img class="history-thumb" src="${thumbUrl}" onerror="this.src='${defaultThumb}'">
            <div class="history-info">
                <div class="song">${h.song}</div>
                <div class="time">${h.time}</div>
            </div>
        </div>`;
    });
    list.innerHTML = html;
}

function searchMusic() {
    const input = document.getElementById('searchInput');
    const query = input.value.trim();
    if (!query) {
        showToast('Vui lòng nhập tên bài hát!');
        return;
    }
    playMusic(query);
}

function playQuick(song) {
    document.getElementById('searchInput').value = song;
    playMusic(song);
}

function playMusic(song) {
    setStatus('loading', 'Đang tìm kiếm...');
    showToast('🔍 Đang tìm: ' + song);
    setThumbnail('');
    
    fetch('/music/play?song=' + encodeURIComponent(song))
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                const thumb = data.thumbnail || '';
                updateUI(true, data.title || song, data.artist || '', thumb);
                addToHistory(data.title || song, thumb);
                showToast('🎵 Đang phát: ' + (data.title || song));
            } else {
                setStatus('idle', 'Lỗi');
                showToast('❌ ' + (data.error || 'Không tìm thấy bài hát'));
            }
        })
        .catch(e => {
            setStatus('idle', 'Lỗi kết nối');
            showToast('❌ Lỗi kết nối!');
        });
}

function stopMusic() {
    fetch('/music/stop')
        .then(r => r.json())
        .then(data => {
            updateUI(false, '', '', '');
            showToast('⏹️ Đã dừng phát');
        })
        .catch(e => {
            showToast('❌ Lỗi kết nối!');
        });
}

// Check status periodically
function checkStatus() {
    fetch('/music/status')
        .then(r => r.json())
        .then(data => {
            if (data.playing !== isPlaying || data.song !== currentSong) {
                updateUI(data.playing, data.song, data.artist, data.thumbnail);
            }
            if (data.buffer_size) {
                document.getElementById('bufferInfo').textContent = 'Buffer: ' + Math.round(data.buffer_size/1024) + 'KB';
            }
        })
        .catch(() => {});
}

// Enter key to search
document.getElementById('searchInput').addEventListener('keypress', e => {
    if (e.key === 'Enter') searchMusic();
});

// Init
renderHistory();
checkStatus();
setInterval(checkStatus, 3000);
</script>
</body>
</html>
)rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, MUSIC_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Music play handler
esp_err_t otto_music_play_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎵 Music play requested");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    // Get query string
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        const char* err = "{\"success\":false,\"error\":\"Missing song parameter\"}";
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // Parse song name
    char song[200] = {0};
    if (httpd_query_key_value(query, "song", song, sizeof(song)) != ESP_OK) {
        const char* err = "{\"success\":false,\"error\":\"Missing song parameter\"}";
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    // URL decode the song name
    // Simple URL decode (replace + with space, handle %XX)
    char decoded[200] = {0};
    int j = 0;
    for (int i = 0; song[i] && j < (int)sizeof(decoded)-1; i++) {
        if (song[i] == '+') {
            decoded[j++] = ' ';
        } else if (song[i] == '%' && song[i+1] && song[i+2]) {
            char hex[3] = {song[i+1], song[i+2], 0};
            decoded[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            decoded[j++] = song[i];
        }
    }
    decoded[j] = '\0';
    
    ESP_LOGI(TAG, "🎵 Playing song: %s", decoded);
    
    // Use Application to schedule music download/play on main thread
    std::string song_name = decoded;
    
    // Schedule on main task to use Esp32Music
    Application::GetInstance().Schedule([song_name]() {
        // Import Esp32Music and call Download
        extern bool otto_music_download_and_play(const std::string& song);
        otto_music_download_and_play(song_name);
    });
    
    // Return success immediately (playback happens async)
    char response[300];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"title\":\"%s\",\"message\":\"Starting playback...\"}", decoded);
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Music stop handler
esp_err_t otto_music_stop_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🎵 Music stop requested");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    // Schedule stop on main task
    Application::GetInstance().Schedule([]() {
        extern void otto_music_stop();
        otto_music_stop();
    });
    
    const char* response = "{\"success\":true,\"message\":\"Music stopped\"}";
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Music status handler
esp_err_t otto_music_status_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    // Get status from music player
    extern bool otto_music_get_status(bool* playing, size_t* buffer_size, char* song, int song_len, char* artist, int artist_len, char* thumbnail, int thumb_len);
    
    bool playing = false;
    size_t buffer_size = 0;
    char song[200] = {0};
    char artist[100] = {0};
    char thumbnail[300] = {0};
    
    otto_music_get_status(&playing, &buffer_size, song, sizeof(song), artist, sizeof(artist), thumbnail, sizeof(thumbnail));
    
    char response[700];
    snprintf(response, sizeof(response), 
             "{\"playing\":%s,\"song\":\"%s\",\"artist\":\"%s\",\"thumbnail\":\"%s\",\"buffer_size\":%d}",
             playing ? "true" : "false", song, artist, thumbnail, (int)buffer_size);
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ============= END MUSIC PLAYER HANDLERS =============

// Send text to AI handler - Original from kytuoi repository
esp_err_t otto_send_text_to_ai_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "HandleSendTextToAI called");
    webserver_reset_auto_stop_timer();  // Reset timer on AI text send
    
    // Giới hạn content length nhỏ hơn để dùng static buffer
    static const size_t MAX_CONTENT_LEN = 2048;  // Reduced from 4096
    size_t content_len = req->content_len;
    ESP_LOGI(TAG, "Content length: %d", (int)content_len);
    
    if (content_len == 0 || content_len > MAX_CONTENT_LEN) {
        ESP_LOGE(TAG, "Invalid content length: %d (max: %d)", (int)content_len, (int)MAX_CONTENT_LEN);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Text quá dài, vui lòng nhập tối đa 1500 ký tự");
        return ESP_FAIL;
    }
    
    // Dùng static buffer để tránh malloc - giảm heap fragmentation
    static char content[2050];  // 2048 + padding
    static SemaphoreHandle_t content_mutex = NULL;
    if (content_mutex == NULL) {
        content_mutex = xSemaphoreCreateMutex();
    }
    
    if (xSemaphoreTake(content_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server bận, thử lại");
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, content, content_len);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive content: %d", ret);
        xSemaphoreGive(content_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive content");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    ESP_LOGI(TAG, "Received content: %s", content);
    
    cJSON* json = cJSON_Parse(content);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        xSemaphoreGive(content_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON* text_item = cJSON_GetObjectItem(json, "text");
    if (!cJSON_IsString(text_item)) {
        ESP_LOGE(TAG, "Missing or invalid 'text' field");
        cJSON_Delete(json);
        xSemaphoreGive(content_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'text' field");
        return ESP_FAIL;
    }
    
    std::string text = text_item->valuestring;
    ESP_LOGI(TAG, "Sending text to AI: %s (length: %d)", text.c_str(), (int)text.length());
    cJSON_Delete(json);
    xSemaphoreGive(content_mutex);  // Release mutex after copying text
    
    // Schedule on main task to avoid stack issues
    // Send text directly to AI server (same flow as mic input)
    std::string text_copy = text;  // Copy for lambda capture
    Application::GetInstance().Schedule([text_copy]() {
        ESP_LOGI(TAG, "Scheduled task executing, calling SendSttMessage with: %s", text_copy.c_str());
        bool success = Application::GetInstance().SendSttMessage(text_copy);
        if (!success) {
            ESP_LOGW(TAG, "Failed to send STT message to server");
        } else {
            ESP_LOGI(TAG, "STT message sent successfully");
        }
    });
    
    // Return success immediately
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Text sent to AI successfully");
    
    char* response_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    free(response_str);
    cJSON_Delete(response);
    // json already deleted at line 2731, don't delete again!
    return ESP_OK;
}

// Schedule message handler - Hẹn giờ gửi tin nhắn đến AI
esp_err_t otto_schedule_message_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "⏰ SCHEDULE MESSAGE HANDLER CALLED!");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Check for cancel action (GET request)
    char query_buf[32];
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char action[16];
        if (httpd_query_key_value(query_buf, "action", action, sizeof(action)) == ESP_OK) {
            if (strcmp(action, "cancel") == 0) {
                ESP_LOGI(TAG, "⏰ Cancelling scheduled message");
                schedule_active = false;
                if (schedule_message_timer != NULL) {
                    xTimerStop(schedule_message_timer, 0);
                }
                scheduled_message[0] = '\0';
                schedule_remaining_seconds = 0;
                schedule_target_timestamp = 0;
                
                // Clear from NVS
                clear_schedule_from_nvs();
                
                cJSON* response = cJSON_CreateObject();
                cJSON_AddBoolToObject(response, "success", true);
                cJSON_AddStringToObject(response, "message", "Đã hủy hẹn giờ");
                char* response_str = cJSON_Print(response);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, response_str, strlen(response_str));
                free(response_str);
                cJSON_Delete(response);
                return ESP_OK;
            }
        }
    }
    
    // POST request - set schedule (dùng static buffer để tránh malloc)
    static char schedule_content[1026];  // 1024 + padding
    static SemaphoreHandle_t schedule_mutex = NULL;
    if (schedule_mutex == NULL) {
        schedule_mutex = xSemaphoreCreateMutex();
    }
    
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }
    
    if (xSemaphoreTake(schedule_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server bận");
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, schedule_content, content_len);
    if (ret <= 0) {
        xSemaphoreGive(schedule_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive content");
        return ESP_FAIL;
    }
    schedule_content[ret] = '\0';
    ESP_LOGI(TAG, "⏰ Schedule request: %s", schedule_content);
    
    cJSON* json = cJSON_Parse(schedule_content);
    if (!json) {
        xSemaphoreGive(schedule_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON* message_item = cJSON_GetObjectItem(json, "message");
    cJSON* seconds_item = cJSON_GetObjectItem(json, "seconds");
    cJSON* mode_item = cJSON_GetObjectItem(json, "mode");
    cJSON* action_slot_item = cJSON_GetObjectItem(json, "action_slot");
    
    if (!cJSON_IsNumber(seconds_item)) {
        xSemaphoreGive(schedule_mutex);
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing seconds");
        return ESP_FAIL;
    }
    
    // Get mode (default: alarm)
    const char* mode = "alarm";
    if (cJSON_IsString(mode_item)) {
        mode = mode_item->valuestring;
    }
    
    // Get action slot (default: 0 = none)
    int action_slot = 0;
    if (cJSON_IsNumber(action_slot_item)) {
        action_slot = action_slot_item->valueint;
        if (action_slot < 0 || action_slot > 3) {
            action_slot = 0;
        }
    }
    
    const char* message = "";
    if (cJSON_IsString(message_item)) {
        message = message_item->valuestring;
    }
    int seconds = seconds_item->valueint;
    
    // Validate: message mode requires non-empty message
    bool is_message_mode = (strcmp(mode, "message") == 0);
    if (is_message_mode && strlen(message) == 0) {
        xSemaphoreGive(schedule_mutex);
        cJSON_Delete(json);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Chế độ hẹn tin nhắn cần có nội dung tin nhắn");
        char* response_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        free(response_str);
        cJSON_Delete(response);
        return ESP_OK;
    }
    
    if (seconds < 10) {
        xSemaphoreGive(schedule_mutex);
        cJSON_Delete(json);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", false);
        cJSON_AddStringToObject(response, "message", "Thời gian tối thiểu 10 giây");
        char* response_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        free(response_str);
        cJSON_Delete(response);
        return ESP_OK;
    }
    
    // Store mode and message
    strncpy(schedule_mode, mode, sizeof(schedule_mode) - 1);
    schedule_mode[sizeof(schedule_mode) - 1] = '\0';
    strncpy(scheduled_message, message, sizeof(scheduled_message) - 1);
    scheduled_message[sizeof(scheduled_message) - 1] = '\0';
    scheduled_action_slot = action_slot;
    schedule_remaining_seconds = seconds;
    schedule_active = true;
    
    // Calculate and store target timestamp for NVS persistence
    time_t now;
    time(&now);
    schedule_target_timestamp = (int64_t)now + seconds;
    
    ESP_LOGI(TAG, "⏰ Scheduled: mode='%s', msg='%s', slot=%d in %d seconds (target: %lld)", 
             schedule_mode, scheduled_message, scheduled_action_slot, seconds, schedule_target_timestamp);
    
    // Save to NVS for persistence across reboots
    save_schedule_to_nvs();
    
    // Create or restart the timer
    if (schedule_message_timer == NULL) {
        schedule_message_timer = xTimerCreate(
            "schedule_msg_timer",
            pdMS_TO_TICKS(1000),  // 1 second period
            pdTRUE,              // Auto-reload
            NULL,
            schedule_countdown_callback
        );
    }
    
    if (schedule_message_timer != NULL) {
        xTimerStop(schedule_message_timer, 0);
        xTimerStart(schedule_message_timer, 0);
    }
    
    xSemaphoreGive(schedule_mutex);
    cJSON_Delete(json);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Đã đặt hẹn giờ gửi tin nhắn");
    cJSON_AddNumberToObject(response, "seconds", seconds);
    char* response_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    free(response_str);
    cJSON_Delete(response);
    
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

// Idle clock handler - Toggle persistent clock display in idle mode
esp_err_t otto_idle_clock_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "⏰ IDLE CLOCK HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Check for enable parameter
    char query_buf[32];
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char enable_str[8];
        if (httpd_query_key_value(query_buf, "enable", enable_str, sizeof(enable_str)) == ESP_OK) {
            bool enable = (strcmp(enable_str, "1") == 0);
            
            // Get display from Board
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                auto otto_display = dynamic_cast<OttoEmojiDisplay*>(display);
                if (otto_display) {
                    otto_display->SetIdleClockEnabled(enable);
                    
                    httpd_resp_set_type(req, "text/plain");
                    if (enable) {
                        httpd_resp_sendstr(req, "✅ Đồng hồ chờ đã BẬT! ⏰");
                    } else {
                        httpd_resp_sendstr(req, "✅ Đồng hồ chờ đã TẮT! 😊");
                    }
                    return ESP_OK;
                }
            }
            
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "❌ Không tìm thấy display!");
            return ESP_OK;
        }
    }
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "❌ Thiếu tham số enable!");
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

// Idle timeout configuration handler
esp_err_t otto_idle_timeout_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "⏰ IDLE TIMEOUT HANDLER CALLED!");
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Get minutes parameter
    char query_buf[32];
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char minutes_str[16];
        if (httpd_query_key_value(query_buf, "minutes", minutes_str, sizeof(minutes_str)) == ESP_OK) {
            int minutes = atoi(minutes_str);
            ESP_LOGI(TAG, "⏰ Setting idle timeout to %d minutes", minutes);
            
            // Validate range (5-180 minutes)
            if (minutes < 5 || minutes > 180) {
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "❌ Thời gian phải từ 5-180 phút!");
                return ESP_OK;
            }
            
            // Update variable and save to NVS
            idle_timeout_minutes = (uint32_t)minutes;
            
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open("otto", NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                nvs_set_u32(nvs_handle, "idle_timeout", idle_timeout_minutes);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
                ESP_LOGI(TAG, "⏰ Saved idle timeout to NVS: %lu minutes", idle_timeout_minutes);
            }
            
            // Update otto_controller with new timeout value
            extern void otto_controller_set_idle_timeout(uint32_t timeout_ms);
            otto_controller_set_idle_timeout(idle_timeout_minutes * 60 * 1000);
            
            char response[64];
            snprintf(response, sizeof(response), "✅ Đã đặt thời gian ngủ: %d phút", minutes);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
    }
    
    // Return current setting
    char response[64];
    snprintf(response, sizeof(response), "Thời gian ngủ hiện tại: %lu phút", idle_timeout_minutes);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// MQTT Configuration handler - Save
esp_err_t otto_mqtt_config_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    if (req->method == HTTP_POST) {
        // Save MQTT configuration
        char buf[512];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid request body\"}");
            return ESP_OK;
        }
        buf[ret] = '\0';
        
        // Parse JSON
        cJSON *root = cJSON_Parse(buf);
        if (!root) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return ESP_OK;
        }
        
        cJSON *endpoint_json = cJSON_GetObjectItem(root, "endpoint");
        if (!cJSON_IsString(endpoint_json) || strlen(endpoint_json->valuestring) == 0) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Endpoint is required\"}");
            return ESP_OK;
        }
        
        // Save to NVS using Settings class
        Settings settings("mqtt", true);
        settings.SetString("endpoint", endpoint_json->valuestring);
        
        cJSON *client_id_json = cJSON_GetObjectItem(root, "client_id");
        if (cJSON_IsString(client_id_json) && strlen(client_id_json->valuestring) > 0) {
            settings.SetString("client_id", client_id_json->valuestring);
        }
        
        cJSON *username_json = cJSON_GetObjectItem(root, "username");
        if (cJSON_IsString(username_json) && strlen(username_json->valuestring) > 0) {
            settings.SetString("username", username_json->valuestring);
        }
        
        cJSON *password_json = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(password_json) && strlen(password_json->valuestring) > 0) {
            settings.SetString("password", password_json->valuestring);
        }
        
        cJSON *publish_topic_json = cJSON_GetObjectItem(root, "publish_topic");
        if (cJSON_IsString(publish_topic_json) && strlen(publish_topic_json->valuestring) > 0) {
            settings.SetString("publish_topic", publish_topic_json->valuestring);
        }
        
        ESP_LOGI(TAG, "✅ MQTT configuration saved. Endpoint: %s", endpoint_json->valuestring);
        ESP_LOGI(TAG, "📡 MQTT will reconnect automatically on next connection attempt");
        
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"MQTT configuration saved successfully\"}");
        return ESP_OK;
    } else {
        // GET - Return current configuration
        Settings settings("mqtt", false);
        std::string endpoint = settings.GetString("endpoint");
        std::string client_id = settings.GetString("client_id");
        std::string username = settings.GetString("username");
        std::string publish_topic = settings.GetString("publish_topic");
        
        httpd_resp_set_type(req, "application/json");
        
        if (endpoint.empty()) {
            httpd_resp_sendstr(req, "{\"configured\":false}");
            return ESP_OK;
        }
        
        char response[512];
        snprintf(response, sizeof(response),
                "{\"configured\":true,\"endpoint\":\"%s\",\"client_id\":\"%s\",\"username\":\"%s\",\"publish_topic\":\"%s\"}",
                endpoint.c_str(),
                client_id.c_str(),
                username.c_str(),
                publish_topic.c_str());
        httpd_resp_sendstr(req, response);
        return ESP_OK;
    }
}

// Gemini API Key handler - REMOVED (Not needed)
/*
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
*/

// Get Gemini API Key handler - REMOVED (Not needed)
/*
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
*/

// Servo Calibration Page Handler
esp_err_t otto_servo_calibration_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Servo calibration page requested");
    webserver_reset_auto_stop_timer();
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head>");
    httpd_resp_sendstr_chunk(req, "<meta charset='UTF-8'>");
    httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    httpd_resp_sendstr_chunk(req, "<title>Servo Calibration - Kiki</title>");
    httpd_resp_sendstr_chunk(req, "<style>");
    httpd_resp_sendstr_chunk(req, "body{font-family:Arial,sans-serif;max-width:800px;margin:20px auto;padding:20px;background:#f0f0f0}");
    httpd_resp_sendstr_chunk(req, "h1{color:#333;text-align:center}");
    httpd_resp_sendstr_chunk(req, ".servo-control{background:white;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}");
    httpd_resp_sendstr_chunk(req, ".servo-name{font-size:18px;font-weight:bold;margin-bottom:10px}");
    httpd_resp_sendstr_chunk(req, ".slider-container{display:flex;align-items:center;gap:10px;margin:10px 0}");
    httpd_resp_sendstr_chunk(req, "input[type='range']{flex:1;height:30px}");
    httpd_resp_sendstr_chunk(req, ".value-display{min-width:50px;font-weight:bold;font-size:18px}");
    httpd_resp_sendstr_chunk(req, "button{background:#4CAF50;color:white;padding:12px 24px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin:5px}");
    httpd_resp_sendstr_chunk(req, "button:hover{background:#45a049}");
    httpd_resp_sendstr_chunk(req, "button.reset{background:#f44336}");
    httpd_resp_sendstr_chunk(req, "button.reset:hover{background:#da190b}");
    httpd_resp_sendstr_chunk(req, ".button-group{text-align:center;margin:20px 0}");
    httpd_resp_sendstr_chunk(req, ".status{padding:10px;margin:10px 0;border-radius:4px;text-align:center}");
    httpd_resp_sendstr_chunk(req, ".success{background:#d4edda;color:#155724}");
    httpd_resp_sendstr_chunk(req, ".error{background:#f8d7da;color:#721c24}");
    httpd_resp_sendstr_chunk(req, "</style></head><body>");
    
    httpd_resp_sendstr_chunk(req, "<h1>🤖 Kiki Servo Calibration</h1>");
    httpd_resp_sendstr_chunk(req, "<div id='status' class='status' style='display:none'></div>");
    
    // Servo LF
    httpd_resp_sendstr_chunk(req, "<div class='servo-control'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🦵 Left Front (LF)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='180' value='90' id='lf' oninput='updateServo(\"lf\",this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='lf-val'>90°</span>");
    httpd_resp_sendstr_chunk(req, "</div></div>");
    
    // Servo RF
    httpd_resp_sendstr_chunk(req, "<div class='servo-control'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🦵 Right Front (RF)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='180' value='90' id='rf' oninput='updateServo(\"rf\",this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='rf-val'>90°</span>");
    httpd_resp_sendstr_chunk(req, "</div></div>");
    
    // Servo LB
    httpd_resp_sendstr_chunk(req, "<div class='servo-control'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🦵 Left Back (LB)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='180' value='90' id='lb' oninput='updateServo(\"lb\",this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='lb-val'>90°</span>");
    httpd_resp_sendstr_chunk(req, "</div></div>");
    
    // Servo RB
    httpd_resp_sendstr_chunk(req, "<div class='servo-control'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🦵 Right Back (RB)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='180' value='90' id='rb' oninput='updateServo(\"rb\",this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='rb-val'>90°</span>");
    httpd_resp_sendstr_chunk(req, "</div></div>");
    
    // Servo Tail
    httpd_resp_sendstr_chunk(req, "<div class='servo-control' style='background:linear-gradient(145deg,#fff3e0,#ffe0b2);border:2px solid #ff9800'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🐕 Tail Servo (GPIO 39)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='180' value='90' id='tail' oninput='updateServo(\"tail\",this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='tail-val'>90°</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size:12px;color:#666;margin-top:5px'>⚙️ Góc chuẩn: 90° (vị trí giữa)</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Speed Multiplier Section
    httpd_resp_sendstr_chunk(req, "<div class='servo-control' style='background:linear-gradient(145deg,#e8f5e9,#c8e6c9);border:2px solid #4caf50'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>⚡ Tốc Độ Di Chuyển (Speed)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='25' max='200' value='100' id='speed_mult' oninput='updateSpeed(this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='speed_mult-val'>100%</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size:12px;color:#666;margin-top:8px'>");
    httpd_resp_sendstr_chunk(req, "📝 <strong>Hướng dẫn:</strong><br>");
    httpd_resp_sendstr_chunk(req, "• <strong>25-50%:</strong> Rất nhanh - robot di chuyển nhanh gấp 2-4 lần<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>50-75%:</strong> Nhanh - phù hợp chơi đùa<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>100%:</strong> Bình thường - tốc độ mặc định<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>125-150%:</strong> Chậm - chuyển động mượt hơn<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>150-200%:</strong> Rất chậm - xem rõ từng động tác<br>");
    httpd_resp_sendstr_chunk(req, "⚠️ <em>Giá trị thấp = tốc độ cao, giá trị cao = tốc độ thấp</em>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<button onclick='saveSpeed()' style='margin-top:10px;background:#4caf50;color:white;padding:12px 24px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer'>⚡ Lưu Tốc Độ</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Mic Gain Section
    httpd_resp_sendstr_chunk(req, "<div class='servo-control' style='background:linear-gradient(145deg,#e3f2fd,#bbdefb);border:2px solid #2196f3'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🎤 Độ Nhạy Microphone (Input Gain)</div>");
    httpd_resp_sendstr_chunk(req, "<div class='slider-container'>");
    httpd_resp_sendstr_chunk(req, "<input type='range' min='0' max='100' value='30' id='mic_gain' oninput='updateMicGain(this.value)'>");
    httpd_resp_sendstr_chunk(req, "<span class='value-display' id='mic_gain-val'>30</span>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size:12px;color:#666;margin-top:8px'>");
    httpd_resp_sendstr_chunk(req, "📝 <strong>Hướng dẫn điều chỉnh:</strong><br>");
    httpd_resp_sendstr_chunk(req, "• <strong>0-20:</strong> Rất thấp - chỉ nghe giọng nói lớn, gần<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>20-40:</strong> Thấp - phù hợp môi trường yên tĩnh<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>40-60:</strong> Trung bình - mặc định, cân bằng<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>60-80:</strong> Cao - nghe giọng nói xa hơn<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>80-100:</strong> Rất cao - nhạy với tiếng ồn<br>");
    httpd_resp_sendstr_chunk(req, "⚠️ <em>Tăng quá cao có thể gây nhận diện sai wakeword</em>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    // Custom Keyword Emoji Section
    httpd_resp_sendstr_chunk(req, "<div class='servo-control' style='background:linear-gradient(145deg,#fff8e1,#ffecb3);border:2px solid #ffc107'>");
    httpd_resp_sendstr_chunk(req, "<div class='servo-name'>🍕 Từ Khóa Kích Hoạt Emoji + Pose</div>");
    httpd_resp_sendstr_chunk(req, "<div style='margin:10px 0'>");
    httpd_resp_sendstr_chunk(req, "<input type='text' id='delicious_keyword' placeholder='Nhập từ khóa tiếng Việt hoặc tiếng Anh (VD: ngon quá, yummy, tuyệt vời...)' style='width:100%;padding:12px;border:2px solid #ffc107;border-radius:8px;font-size:14px;box-sizing:border-box'>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='margin:10px 0'>");
    httpd_resp_sendstr_chunk(req, "<label style='font-weight:bold;color:#666'>🐕 Chọn Pose kèm theo:</label><br>");
    httpd_resp_sendstr_chunk(req, "<select id='delicious_pose' style='width:100%;padding:12px;border:2px solid #ffc107;border-radius:8px;font-size:14px;margin-top:5px'>");
    httpd_resp_sendstr_chunk(req, "<option value='none'>❌ Không có Pose</option>");
    httpd_resp_sendstr_chunk(req, "<option value='sit'>🪑 Ngồi (Sit)</option>");
    httpd_resp_sendstr_chunk(req, "<option value='wave'>👋 Vẫy tay (Wave)</option>");
    httpd_resp_sendstr_chunk(req, "<option value='bow'>🙇 Cúi chào (Bow)</option>");
    httpd_resp_sendstr_chunk(req, "<option value='stretch'>🙆 Vươn vai (Stretch)</option>");
    httpd_resp_sendstr_chunk(req, "<option value='swing'>💃 Lắc lư (Swing)</option>");
    httpd_resp_sendstr_chunk(req, "<option value='dance'>🕺 Nhảy (Dance)</option>");
    httpd_resp_sendstr_chunk(req, "</select>");
    httpd_resp_sendstr_chunk(req, "</div>");
    // Action slot selection for keyword
    httpd_resp_sendstr_chunk(req, "<div style='margin:10px 0'>");
    httpd_resp_sendstr_chunk(req, "<label style='font-weight:bold;color:#9c27b0'>🎭 Hành động đã lưu:</label><br>");
    httpd_resp_sendstr_chunk(req, "<select id='keyword_action_slot' style='width:100%;padding:12px;border:2px solid #9c27b0;border-radius:8px;font-size:14px;margin-top:5px;background:#f3e5f5'>");
    httpd_resp_sendstr_chunk(req, "<option value='0'>⚪ Không chọn hành động</option>");
    httpd_resp_sendstr_chunk(req, "<option value='1'>📍 Vị trí 1</option>");
    httpd_resp_sendstr_chunk(req, "<option value='2'>📍 Vị trí 2</option>");
    httpd_resp_sendstr_chunk(req, "<option value='3'>📍 Vị trí 3</option>");
    httpd_resp_sendstr_chunk(req, "</select>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size:11px;color:#9c27b0;margin-top:4px'>💡 Chọn hành động đã lưu ở trang Điều Khiển</div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div style='font-size:12px;color:#666;margin-top:8px'>");;
    httpd_resp_sendstr_chunk(req, "📝 <strong>Hướng dẫn sử dụng từ khóa:</strong><br>");
    httpd_resp_sendstr_chunk(req, "• Nhập từ khóa bạn muốn kích hoạt emoji 'Delicious' 😋<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>Nhiều từ khóa:</strong> cách nhau bằng dấu phẩy (,) hoặc chấm phẩy (;)<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>Ví dụ tiếng Việt:</strong> ngon quá, tuyệt vời, xuất sắc, thích quá<br>");
    httpd_resp_sendstr_chunk(req, "• <strong>Ví dụ tiếng Anh:</strong> yummy, delicious, awesome, great<br>");
    httpd_resp_sendstr_chunk(req, "• Từ khóa không phân biệt HOA/thường<br>");
    httpd_resp_sendstr_chunk(req, "• Khi nói từ khóa, Kiki sẽ hiển thị emoji + thực hiện Pose");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div id='keyword_status' style='display:none;padding:12px;border-radius:8px;margin-top:10px;font-weight:bold;text-align:center'></div>");
    httpd_resp_sendstr_chunk(req, "<button onclick='saveDeliciousKeyword()' style='margin-top:10px;background:#ffc107;color:#333;padding:12px 24px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer'>🍕 Lưu Từ Khóa + Pose</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    httpd_resp_sendstr_chunk(req, "<div class='button-group'>");
    httpd_resp_sendstr_chunk(req, "<button onclick='saveCalibration()'>💾 Save Servo</button>");
    httpd_resp_sendstr_chunk(req, "<button onclick='saveMicGain()'>🎤 Save Mic Gain</button>");
    httpd_resp_sendstr_chunk(req, "<button class='reset' onclick='resetToDefault()'>🔄 Reset to 90°</button>");
    httpd_resp_sendstr_chunk(req, "<button onclick='window.location.href=\"/\"'>🏠 Back to Control</button>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    httpd_resp_sendstr_chunk(req, "<script>");
    httpd_resp_sendstr_chunk(req, "function updateServo(servo,val){");
    httpd_resp_sendstr_chunk(req, "document.getElementById(servo+'-val').textContent=val+'°';");
    httpd_resp_sendstr_chunk(req, "fetch('/servo_set?servo='+servo+'&angle='+val);");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function saveCalibration(){");
    httpd_resp_sendstr_chunk(req, "var lf=document.getElementById('lf').value;");
    httpd_resp_sendstr_chunk(req, "var rf=document.getElementById('rf').value;");
    httpd_resp_sendstr_chunk(req, "var lb=document.getElementById('lb').value;");
    httpd_resp_sendstr_chunk(req, "var rb=document.getElementById('rb').value;");
    httpd_resp_sendstr_chunk(req, "var tail=document.getElementById('tail').value;");
    httpd_resp_sendstr_chunk(req, "var url='/servo_save?lf='+lf+'&rf='+rf+'&lb='+lb+'&rb='+rb+'&tail='+tail;");
    httpd_resp_sendstr_chunk(req, "fetch(url).then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "var s=document.getElementById('status');");
    httpd_resp_sendstr_chunk(req, "s.style.display='block';");
    httpd_resp_sendstr_chunk(req, "if(d.success){s.className='status success';s.textContent='✅ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "else{s.className='status error';s.textContent='❌ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "setTimeout(function(){s.style.display='none';},3000);");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Save error:',e);});");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function resetToDefault(){");
    httpd_resp_sendstr_chunk(req, "var servos=['lf','rf','lb','rb','tail'];");
    httpd_resp_sendstr_chunk(req, "for(var i=0;i<servos.length;i++){var s=servos[i];document.getElementById(s).value=90;updateServo(s,90);}");
    httpd_resp_sendstr_chunk(req, "}");
    // Mic gain functions
    httpd_resp_sendstr_chunk(req, "function updateMicGain(val){");
    httpd_resp_sendstr_chunk(req, "document.getElementById('mic_gain-val').textContent=val;");
    httpd_resp_sendstr_chunk(req, "fetch('/mic_gain_set?gain='+val);");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function saveMicGain(){");
    httpd_resp_sendstr_chunk(req, "var gain=document.getElementById('mic_gain').value;");
    httpd_resp_sendstr_chunk(req, "fetch('/mic_gain_save?gain='+gain).then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "var s=document.getElementById('status');");
    httpd_resp_sendstr_chunk(req, "s.style.display='block';");
    httpd_resp_sendstr_chunk(req, "if(d.success){s.className='status success';s.textContent='✅ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "else{s.className='status error';s.textContent='❌ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "setTimeout(function(){s.style.display='none';},3000);");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Mic save error:',e);});");
    httpd_resp_sendstr_chunk(req, "}");
    // Speed functions
    httpd_resp_sendstr_chunk(req, "function updateSpeed(val){");
    httpd_resp_sendstr_chunk(req, "document.getElementById('speed_mult-val').textContent=val+'%';");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "function saveSpeed(){");
    httpd_resp_sendstr_chunk(req, "var speed=document.getElementById('speed_mult').value;");
    httpd_resp_sendstr_chunk(req, "fetch('/speed_save?speed='+speed).then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "var s=document.getElementById('status');");
    httpd_resp_sendstr_chunk(req, "s.style.display='block';");
    httpd_resp_sendstr_chunk(req, "if(d.success){s.className='status success';s.textContent='✅ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "else{s.className='status error';s.textContent='❌ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "setTimeout(function(){s.style.display='none';},3000);");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Speed save error:',e);});");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "window.onload=function(){");
    httpd_resp_sendstr_chunk(req, "fetch('/servo_get').then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "if(d.lf){document.getElementById('lf').value=d.lf;document.getElementById('lf-val').textContent=d.lf+'°';}");
    httpd_resp_sendstr_chunk(req, "if(d.rf){document.getElementById('rf').value=d.rf;document.getElementById('rf-val').textContent=d.rf+'°';}");
    httpd_resp_sendstr_chunk(req, "if(d.lb){document.getElementById('lb').value=d.lb;document.getElementById('lb-val').textContent=d.lb+'°';}");
    httpd_resp_sendstr_chunk(req, "if(d.rb){document.getElementById('rb').value=d.rb;document.getElementById('rb-val').textContent=d.rb+'°';}");
    httpd_resp_sendstr_chunk(req, "if(d.tail){document.getElementById('tail').value=d.tail;document.getElementById('tail-val').textContent=d.tail+'°';}");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Servo get error:',e);});");
    httpd_resp_sendstr_chunk(req, "fetch('/mic_gain_get').then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "if(d.gain!==undefined){document.getElementById('mic_gain').value=d.gain;document.getElementById('mic_gain-val').textContent=d.gain;}");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Mic get error:',e);});");
    // Load speed
    httpd_resp_sendstr_chunk(req, "fetch('/speed_get').then(function(r){return r.json();}).then(function(d){");
    httpd_resp_sendstr_chunk(req, "if(d.speed!==undefined){document.getElementById('speed_mult').value=d.speed;document.getElementById('speed_mult-val').textContent=d.speed+'%';}");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Speed get error:',e);});");
    // Load delicious keyword and pose
    httpd_resp_sendstr_chunk(req, "fetch('/delicious_keyword_get').then(function(r){");
    httpd_resp_sendstr_chunk(req, "if(!r.ok){throw new Error('HTTP '+r.status);}");
    httpd_resp_sendstr_chunk(req, "return r.text();");
    httpd_resp_sendstr_chunk(req, "}).then(function(txt){");
    httpd_resp_sendstr_chunk(req, "var d;try{d=JSON.parse(txt);}catch(e){console.log('Parse error:',txt);return;}");
    httpd_resp_sendstr_chunk(req, "if(d.keyword){document.getElementById('delicious_keyword').value=d.keyword;}");
    httpd_resp_sendstr_chunk(req, "if(d.pose){document.getElementById('delicious_pose').value=d.pose;}");
    httpd_resp_sendstr_chunk(req, "if(d.action_slot!==undefined){document.getElementById('keyword_action_slot').value=d.action_slot;}");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.log('Load keyword error:',e);});");
    httpd_resp_sendstr_chunk(req, "};");
    // Save delicious keyword function
    httpd_resp_sendstr_chunk(req, "function saveDeliciousKeyword(){");
    httpd_resp_sendstr_chunk(req, "var kw=document.getElementById('delicious_keyword').value;");
    httpd_resp_sendstr_chunk(req, "var pose=document.getElementById('delicious_pose').value;");
    httpd_resp_sendstr_chunk(req, "var actionSlot=document.getElementById('keyword_action_slot').value;");
    httpd_resp_sendstr_chunk(req, "kw=kw.trim();");
    httpd_resp_sendstr_chunk(req, "if(kw.length==0){alert('Vui lòng nhập từ khóa!');return;}");
    httpd_resp_sendstr_chunk(req, "var ks=document.getElementById('keyword_status');");
    httpd_resp_sendstr_chunk(req, "ks.style.display='block';ks.style.background='#fff3cd';ks.style.color='#856404';ks.textContent='⏳ Đang lưu...';");
    httpd_resp_sendstr_chunk(req, "var url='/delicious_keyword_save?keyword='+encodeURIComponent(kw)+'&pose='+pose+'&action_slot='+actionSlot;");
    httpd_resp_sendstr_chunk(req, "console.log('Saving keyword:',kw,'pose:',pose,'action_slot:',actionSlot,'URL:',url);");
    httpd_resp_sendstr_chunk(req, "fetch(url).then(function(r){");
    httpd_resp_sendstr_chunk(req, "if(!r.ok){throw new Error('HTTP '+r.status);}");
    httpd_resp_sendstr_chunk(req, "return r.text();");
    httpd_resp_sendstr_chunk(req, "}).then(function(txt){");
    httpd_resp_sendstr_chunk(req, "console.log('Response:',txt);");
    httpd_resp_sendstr_chunk(req, "var d;try{d=JSON.parse(txt);}catch(e){throw new Error('Invalid JSON: '+txt.substring(0,50));}");
    httpd_resp_sendstr_chunk(req, "var ks=document.getElementById('keyword_status');");
    httpd_resp_sendstr_chunk(req, "if(d.success){ks.style.background='#d4edda';ks.style.color='#155724';ks.textContent='✅ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "else{ks.style.background='#f8d7da';ks.style.color='#721c24';ks.textContent='❌ '+d.message;}");
    httpd_resp_sendstr_chunk(req, "setTimeout(function(){ks.style.display='none';},5000);");
    httpd_resp_sendstr_chunk(req, "}).catch(function(e){console.error('Save error:',e);var ks=document.getElementById('keyword_status');ks.style.background='#f8d7da';ks.style.color='#721c24';ks.textContent='❌ Lỗi: '+e.message;});");
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, "</script>");
    
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Servo Set Angle Handler
esp_err_t otto_servo_set_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char servo[10], angle[10];
        if (httpd_query_key_value(buf, "servo", servo, sizeof(servo)) == ESP_OK &&
            httpd_query_key_value(buf, "angle", angle, sizeof(angle)) == ESP_OK) {
            
            int angle_val = atoi(angle);
            int servo_id = -1;
            
            if (strcmp(servo, "lf") == 0) servo_id = 0;  // SERVO_LF
            else if (strcmp(servo, "rf") == 0) servo_id = 1;  // SERVO_RF
            else if (strcmp(servo, "lb") == 0) servo_id = 2;  // SERVO_LB
            else if (strcmp(servo, "rb") == 0) servo_id = 3;  // SERVO_RB
            else if (strcmp(servo, "tail") == 0) servo_id = 4;  // SERVO_TAIL
            
            if (servo_id >= 0) {
                otto_controller_set_servo_angle(servo_id, angle_val);
                ESP_LOGI(TAG, "Set servo %s to %d°", servo, angle_val);
            }
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// Servo Get Angles Handler
esp_err_t otto_servo_get_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    
    // Get current servo angles from NVS or defaults
    nvs_handle_t nvs_handle;
    int32_t lf = 90, rf = 90, lb = 90, rb = 90, tail = 90;
    
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_i32(nvs_handle, "servo_lf", &lf);
        nvs_get_i32(nvs_handle, "servo_rf", &rf);
        nvs_get_i32(nvs_handle, "servo_lb", &lb);
        nvs_get_i32(nvs_handle, "servo_rb", &rb);
        nvs_get_i32(nvs_handle, "servo_tail", &tail);
        nvs_close(nvs_handle);
    }
    
    char response[250];
    snprintf(response, sizeof(response), 
        "{\"lf\":%d,\"rf\":%d,\"lb\":%d,\"rb\":%d,\"tail\":%d}", (int)lf, (int)rf, (int)lb, (int)rb, (int)tail);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Servo Save Calibration Handler
esp_err_t otto_servo_save_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    
    char buf[200];
    int lf = 90, rf = 90, lb = 90, rb = 90, tail = 90;
    
    // Get angles from query parameters (RAW angles from sliders)
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[10];
        if (httpd_query_key_value(buf, "lf", val, sizeof(val)) == ESP_OK) lf = atoi(val);
        if (httpd_query_key_value(buf, "rf", val, sizeof(val)) == ESP_OK) rf = atoi(val);
        if (httpd_query_key_value(buf, "lb", val, sizeof(val)) == ESP_OK) lb = atoi(val);
        if (httpd_query_key_value(buf, "rb", val, sizeof(val)) == ESP_OK) rb = atoi(val);
        if (httpd_query_key_value(buf, "tail", val, sizeof(val)) == ESP_OK) tail = atoi(val);
    }
    
    // Save RAW angles to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, "servo_lf", lf);
        nvs_set_i32(nvs_handle, "servo_rf", rf);
        nvs_set_i32(nvs_handle, "servo_lb", lb);
        nvs_set_i32(nvs_handle, "servo_rb", rb);
        nvs_set_i32(nvs_handle, "servo_tail", tail);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        ESP_LOGI(TAG, "💾 Saved servo calibration (RAW): LF=%d RF=%d LB=%d RB=%d TAIL=%d", lf, rf, lb, rb, tail);
        
        // Apply new home position immediately (no reboot needed)
        otto_controller_apply_servo_home(lf, rf, lb, rb);
        
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Calibration saved and applied!\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to save calibration\"}");
    }
    
    return ESP_OK;
}

// Mic Gain Set Handler - realtime update
esp_err_t otto_mic_gain_set_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char gain_str[10];
        if (httpd_query_key_value(buf, "gain", gain_str, sizeof(gain_str)) == ESP_OK) {
            int gain_val = atoi(gain_str);
            if (gain_val < 0) gain_val = 0;
            if (gain_val > 100) gain_val = 100;
            
            // Apply mic gain immediately via AudioCodec
            Board& board = Board::GetInstance();
            if (board.GetAudioCodec()) {
                board.GetAudioCodec()->SetInputGain((float)gain_val);
                ESP_LOGI(TAG, "🎤 Mic gain set to: %d", gain_val);
            }
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// Mic Gain Get Handler
esp_err_t otto_mic_gain_get_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    // Get current mic gain from NVS or default
    nvs_handle_t nvs_handle;
    int32_t gain = 30;  // Default value
    
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_i32(nvs_handle, "mic_gain", &gain);
        nvs_close(nvs_handle);
    }
    
    char response[50];
    snprintf(response, sizeof(response), "{\"gain\":%d}", (int)gain);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Mic Gain Save Handler
esp_err_t otto_mic_gain_save_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char buf[100];
    int gain = 30;
    
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char gain_str[10];
        if (httpd_query_key_value(buf, "gain", gain_str, sizeof(gain_str)) == ESP_OK) {
            gain = atoi(gain_str);
            if (gain < 0) gain = 0;
            if (gain > 100) gain = 100;
        }
    }
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, "mic_gain", gain);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        // Apply mic gain immediately
        Board& board = Board::GetInstance();
        if (board.GetAudioCodec()) {
            board.GetAudioCodec()->SetInputGain((float)gain);
        }
        
        ESP_LOGI(TAG, "💾 Saved mic gain: %d", gain);
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Mic gain saved!\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to save mic gain\"}");
    }
    
    return ESP_OK;
}

// Speed Multiplier Get Handler
esp_err_t otto_speed_get_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char response[50];
    snprintf(response, sizeof(response), "{\"speed\":%d}", speed_multiplier);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Speed Multiplier Save Handler
esp_err_t otto_speed_save_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char buf[100];
    int speed = 100;
    
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char speed_str[10];
        if (httpd_query_key_value(buf, "speed", speed_str, sizeof(speed_str)) == ESP_OK) {
            speed = atoi(speed_str);
            if (speed < 50) speed = 50;
            if (speed > 200) speed = 200;
        }
    }
    
    // Update global
    speed_multiplier = speed;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, "speed_mult", (int32_t)speed);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        ESP_LOGI(TAG, "💾 Saved speed multiplier: %d%%", speed);
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Speed saved!\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to save speed\"}");
    }
    
    return ESP_OK;
}

// Delicious Keyword Get Handler
esp_err_t otto_delicious_keyword_get_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    nvs_handle_t nvs_handle;
    char keyword[512] = "";
    char pose[32] = "none";
    int8_t action_slot = 0;
    
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = sizeof(keyword);
        nvs_get_str(nvs_handle, "delicious_kw", keyword, &len);
        len = sizeof(pose);
        nvs_get_str(nvs_handle, "delicious_pose", pose, &len);
        nvs_get_i8(nvs_handle, "kw_action_slot", &action_slot);
        nvs_close(nvs_handle);
    }
    
    // Use cJSON for proper escaping
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "keyword", keyword);
    cJSON_AddStringToObject(json, "pose", pose);
    cJSON_AddNumberToObject(json, "action_slot", action_slot);
    char* response = cJSON_PrintUnformatted(json);
    httpd_resp_sendstr(req, response);
    free(response);
    cJSON_Delete(json);
    return ESP_OK;
}

// Delicious Keyword Save Handler
esp_err_t otto_delicious_keyword_save_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    
    char buf[1024] = "";
    char keyword[512] = "";
    char pose[32] = "none";
    char action_slot_str[8] = "0";
    
    esp_err_t query_err = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    ESP_LOGI(TAG, "📝 Keyword save - query_err=%d, buf='%s'", query_err, buf);
    
    if (query_err == ESP_OK) {
        char keyword_encoded[512] = "";
        esp_err_t key_err = httpd_query_key_value(buf, "keyword", keyword_encoded, sizeof(keyword_encoded));
        ESP_LOGI(TAG, "📝 Keyword save - key_err=%d, encoded='%s'", key_err, keyword_encoded);
        
        if (key_err == ESP_OK) {
            // URL decode the keyword
            size_t src_len = strlen(keyword_encoded);
            size_t dst_idx = 0;
            for (size_t i = 0; i < src_len && dst_idx < sizeof(keyword) - 1; i++) {
                if (keyword_encoded[i] == '%' && i + 2 < src_len) {
                    char hex[3] = {keyword_encoded[i+1], keyword_encoded[i+2], 0};
                    keyword[dst_idx++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else if (keyword_encoded[i] == '+') {
                    keyword[dst_idx++] = ' ';
                } else {
                    keyword[dst_idx++] = keyword_encoded[i];
                }
            }
            keyword[dst_idx] = '\0';
            ESP_LOGI(TAG, "📝 Keyword save - decoded='%s'", keyword);
        }
        
        // Get pose parameter
        httpd_query_key_value(buf, "pose", pose, sizeof(pose));
        ESP_LOGI(TAG, "📝 Keyword save - pose='%s'", pose);
        
        // Get action slot parameter
        httpd_query_key_value(buf, "action_slot", action_slot_str, sizeof(action_slot_str));
        ESP_LOGI(TAG, "📝 Keyword save - action_slot='%s'", action_slot_str);
    }
    
    if (strlen(keyword) == 0) {
        ESP_LOGW(TAG, "❌ Keyword save - empty keyword!");
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Keyword is empty!\"}");
        return ESP_OK;
    }
    
    int8_t action_slot = (int8_t)atoi(action_slot_str);
    if (action_slot < 0 || action_slot > 3) action_slot = 0;
    
    // Save to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, "delicious_kw", keyword);
        nvs_set_str(nvs_handle, "delicious_pose", pose);
        nvs_set_i8(nvs_handle, "kw_action_slot", action_slot);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        ESP_LOGI(TAG, "💾 Saved delicious keyword: %s, pose: %s, action_slot: %d", keyword, pose, action_slot);
        
        // Reload cached keywords immediately so changes take effect without reboot
        Application::GetInstance().ReloadCustomKeywords();
        
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Keyword + Pose + Action saved!\"}");
    } else {
        ESP_LOGE(TAG, "❌ NVS open error: %d", err);
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"NVS error!\"}");
    }
    
    return ESP_OK;
}

// ========== LED CONTROL HANDLERS ==========

// LED set color handler: /led?r=255&g=0&b=0
esp_err_t otto_led_color_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char buf[200];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No parameters!\"}");
        return ESP_OK;
    }
    
    char r_str[10] = "0", g_str[10] = "0", b_str[10] = "0";
    httpd_query_key_value(buf, "r", r_str, sizeof(r_str));
    httpd_query_key_value(buf, "g", g_str, sizeof(g_str));
    httpd_query_key_value(buf, "b", b_str, sizeof(b_str));
    
    uint8_t r = (uint8_t)atoi(r_str);
    uint8_t g = (uint8_t)atoi(g_str);
    uint8_t b = (uint8_t)atoi(b_str);
    
    ESP_LOGI(TAG, "🎨 Web LED color: R=%d G=%d B=%d", r, g, b);
    
    kiki_led_set_color(r, g, b);
    kiki_led_set_mode(LED_MODE_SOLID);
    kiki_led_update();
    
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"message\":\"LED color set to RGB(%d,%d,%d)\"}", r, g, b);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// LED set mode handler: /led_mode?mode=rainbow
esp_err_t otto_led_mode_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No mode parameter!\"}");
        return ESP_OK;
    }
    
    char mode_str[20] = "solid";
    httpd_query_key_value(buf, "mode", mode_str, sizeof(mode_str));
    
    led_mode_t mode = LED_MODE_SOLID;
    const char* mode_name = "Solid";
    
    if (strcmp(mode_str, "off") == 0) {
        mode = LED_MODE_OFF;
        mode_name = "Off";
    } else if (strcmp(mode_str, "solid") == 0) {
        mode = LED_MODE_SOLID;
        mode_name = "Solid";
    } else if (strcmp(mode_str, "rainbow") == 0) {
        mode = LED_MODE_RAINBOW;
        mode_name = "Rainbow";
    } else if (strcmp(mode_str, "breathing") == 0) {
        mode = LED_MODE_BREATHING;
        mode_name = "Breathing";
    } else if (strcmp(mode_str, "chase") == 0) {
        mode = LED_MODE_CHASE;
        mode_name = "Chase";
    } else if (strcmp(mode_str, "blink") == 0) {
        mode = LED_MODE_BLINK;
        mode_name = "Blink";
    }
    
    ESP_LOGI(TAG, "🎯 Web LED mode: %s", mode_name);
    
    kiki_led_set_mode(mode);
    kiki_led_update();
    
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"message\":\"LED mode set to %s\"}", mode_name);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// LED set brightness handler: /led_brightness?value=128
esp_err_t otto_led_brightness_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No value parameter!\"}");
        return ESP_OK;
    }
    
    char value_str[10] = "128";
    httpd_query_key_value(buf, "value", value_str, sizeof(value_str));
    
    uint8_t brightness = (uint8_t)atoi(value_str);
    
    ESP_LOGI(TAG, "💡 Web LED brightness: %d", brightness);
    
    kiki_led_set_brightness(brightness);
    kiki_led_update();
    
    int percent = (brightness * 100) / 255;
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"message\":\"LED brightness set to %d (%d%%)\"}", brightness, percent);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// LED set speed handler: /led_speed?value=50
esp_err_t otto_led_speed_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No value parameter!\"}");
        return ESP_OK;
    }
    
    char value_str[10] = "50";
    httpd_query_key_value(buf, "value", value_str, sizeof(value_str));
    
    uint16_t speed = (uint16_t)atoi(value_str);
    if (speed < 10) speed = 10;
    if (speed > 500) speed = 500;
    
    ESP_LOGI(TAG, "⚡ Web LED speed: %d ms", speed);
    
    kiki_led_set_speed(speed);
    kiki_led_update();
    
    char response[150];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"message\":\"LED speed set to %d ms\"}", speed);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// LED get state handler: /led_state
esp_err_t otto_led_state_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    const led_state_t* state = kiki_led_get_state();
    
    const char* mode_name = "Unknown";
    switch (state->mode) {
        case LED_MODE_OFF: mode_name = "off"; break;
        case LED_MODE_SOLID: mode_name = "solid"; break;
        case LED_MODE_RAINBOW: mode_name = "rainbow"; break;
        case LED_MODE_BREATHING: mode_name = "breathing"; break;
        case LED_MODE_CHASE: mode_name = "chase"; break;
        case LED_MODE_BLINK: mode_name = "blink"; break;
    }
    
    char response[300];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"r\":%d,\"g\":%d,\"b\":%d,"
             "\"brightness\":%d,\"mode\":\"%s\",\"speed\":%d}",
             state->r, state->g, state->b, state->brightness, mode_name, state->speed);
    
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// LED off handler: /led_off
esp_err_t otto_led_off_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    ESP_LOGI(TAG, "💤 Web LED off");
    
    kiki_led_off();
    
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"All LEDs turned off\"}");
    
    return ESP_OK;
}

// LED save handler: /led_save
esp_err_t otto_led_save_handler(httpd_req_t *req) {
    webserver_reset_auto_stop_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    ESP_LOGI(TAG, "💾 Web LED save");
    
    kiki_led_save_to_nvs();
    
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"LED state saved to memory\"}");
    
    return ESP_OK;
}

// Start HTTP server
esp_err_t otto_start_webserver(void) {
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }
    
    // Load saved settings from NVS
    load_speed_from_nvs();
    
    // Load memory slots from NVS
    load_memory_slots_from_nvs();
    
    // Load idle timeout setting from NVS
    {
        nvs_handle_t nvs_handle;
        if (nvs_open("otto", NVS_READONLY, &nvs_handle) == ESP_OK) {
            uint32_t timeout = 60;
            if (nvs_get_u32(nvs_handle, "idle_timeout", &timeout) == ESP_OK) {
                idle_timeout_minutes = timeout;
                ESP_LOGI(TAG, "⏰ Loaded idle timeout: %lu minutes", idle_timeout_minutes);
            }
            nvs_close(nvs_handle);
        }
    }
    
    // Load and restore any pending scheduled messages from NVS
    load_schedule_from_nvs();
    
    // Khởi tạo draw buffer mutex
    if (draw_buffer_pool.mutex == NULL) {
        draw_buffer_pool.mutex = xSemaphoreCreateMutex();
        ESP_LOGI(TAG, "✅ Draw buffer pool initialized");
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 60;  // Increased for all handlers (currently 51 endpoints including music)
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
        
        // Memory slot URIs
        httpd_uri_t save_slot_uri = {
            .uri = "/save_slot",
            .method = HTTP_GET,
            .handler = otto_save_slot_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &save_slot_uri);
        
        httpd_uri_t play_slot_uri = {
            .uri = "/play_slot",
            .method = HTTP_GET,
            .handler = otto_play_slot_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &play_slot_uri);
        
        httpd_uri_t slot_info_uri = {
            .uri = "/slot_info",
            .method = HTTP_GET,
            .handler = otto_slot_info_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &slot_info_uri);
        
        // ============= MUSIC PLAYER ENDPOINTS =============
        httpd_uri_t music_page_uri = {
            .uri = "/music",
            .method = HTTP_GET,
            .handler = otto_music_page_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &music_page_uri);
        
        httpd_uri_t music_play_uri = {
            .uri = "/music/play",
            .method = HTTP_GET,
            .handler = otto_music_play_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &music_play_uri);
        
        httpd_uri_t music_stop_uri = {
            .uri = "/music/stop",
            .method = HTTP_GET,
            .handler = otto_music_stop_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &music_stop_uri);
        
        httpd_uri_t music_status_uri = {
            .uri = "/music/status",
            .method = HTTP_GET,
            .handler = otto_music_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &music_status_uri);
        // ============= END MUSIC PLAYER ENDPOINTS =============
        
#ifdef TOUCH_TTP223_GPIO
        httpd_uri_t touch_sensor_uri = {
            .uri = "/touch_sensor",
            .method = HTTP_GET,
            .handler = otto_touch_sensor_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &touch_sensor_uri);
#endif // TOUCH_TTP223_GPIO
        
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
        
        // Screen rotation handler registration
        httpd_uri_t screen_rotation_uri = {
            .uri = "/screen_rotation",
            .method = HTTP_GET,
            .handler = otto_screen_rotation_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &screen_rotation_uri);
        
        // Drawing handler registration (POST - receives RGB565 image data)
        httpd_uri_t draw_uri = {
            .uri = "/draw",
            .method = HTTP_POST,
            .handler = otto_draw_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &draw_uri);
        
        // Drawing exit handler registration (GET - returns to emoji display)
        httpd_uri_t draw_exit_uri = {
            .uri = "/draw_exit",
            .method = HTTP_GET,
            .handler = otto_draw_exit_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &draw_exit_uri);
        
        // Forget WiFi handler registration
        httpd_uri_t forget_wifi_uri = {
            .uri = "/forget_wifi",
            .method = HTTP_GET,
            .handler = otto_forget_wifi_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &forget_wifi_uri);
        
        // Idle timeout configuration handler
        httpd_uri_t idle_timeout_uri = {
            .uri = "/idle_timeout",
            .method = HTTP_GET,
            .handler = otto_idle_timeout_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &idle_timeout_uri);
        
        // MQTT configuration handler (GET and POST)
        httpd_uri_t mqtt_config_uri = {
            .uri = "/mqtt_config",
            .method = HTTP_GET,
            .handler = otto_mqtt_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mqtt_config_uri);
        
        httpd_uri_t mqtt_config_post_uri = {
            .uri = "/mqtt_config",
            .method = HTTP_POST,
            .handler = otto_mqtt_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mqtt_config_post_uri);
        
        // Wake microphone handler registration (supports start/stop)
        httpd_uri_t wake_mic_uri = {
            .uri = "/wake_mic",
            .method = HTTP_GET,
            .handler = otto_wake_mic_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wake_mic_uri);
        
        // LED control handlers registration
        httpd_uri_t led_color_uri = {
            .uri = "/led",
            .method = HTTP_GET,
            .handler = otto_led_color_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_color_uri);
        
        httpd_uri_t led_mode_uri = {
            .uri = "/led_mode",
            .method = HTTP_GET,
            .handler = otto_led_mode_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_mode_uri);
        
        httpd_uri_t led_brightness_uri = {
            .uri = "/led_brightness",
            .method = HTTP_GET,
            .handler = otto_led_brightness_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_brightness_uri);
        
        httpd_uri_t led_speed_uri = {
            .uri = "/led_speed",
            .method = HTTP_GET,
            .handler = otto_led_speed_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_speed_uri);
        
        httpd_uri_t led_state_uri = {
            .uri = "/led_state",
            .method = HTTP_GET,
            .handler = otto_led_state_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_state_uri);
        
        httpd_uri_t led_off_uri = {
            .uri = "/led_off",
            .method = HTTP_GET,
            .handler = otto_led_off_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_off_uri);
        
        httpd_uri_t led_save_uri = {
            .uri = "/led_save",
            .method = HTTP_GET,
            .handler = otto_led_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_save_uri);
        
        // Idle clock handler registration - Toggle persistent clock in idle mode
        httpd_uri_t idle_clock_uri = {
            .uri = "/idle_clock",
            .method = HTTP_GET,
            .handler = otto_idle_clock_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &idle_clock_uri);
        
        // Send text to AI handler - From kytuoi repository
        httpd_uri_t send_text_to_ai_uri = {
            .uri = "/api/ai/send",
            .method = HTTP_POST,
            .handler = otto_send_text_to_ai_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &send_text_to_ai_uri);
        
        // Schedule message handler - POST for setting schedule
        httpd_uri_t schedule_message_post_uri = {
            .uri = "/schedule_message",
            .method = HTTP_POST,
            .handler = otto_schedule_message_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &schedule_message_post_uri);
        
        // Schedule message handler - GET for cancel action
        httpd_uri_t schedule_message_get_uri = {
            .uri = "/schedule_message",
            .method = HTTP_GET,
            .handler = otto_schedule_message_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &schedule_message_get_uri);
        
        // Gemini API Key handlers - REMOVED (Not needed)
        /*
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
        */
        
        // Servo calibration handlers
        httpd_uri_t servo_calibration_uri = {
            .uri = "/servo_calibration",
            .method = HTTP_GET,
            .handler = otto_servo_calibration_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &servo_calibration_uri);
        
        httpd_uri_t servo_set_uri = {
            .uri = "/servo_set",
            .method = HTTP_GET,
            .handler = otto_servo_set_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &servo_set_uri);
        
        httpd_uri_t servo_get_uri = {
            .uri = "/servo_get",
            .method = HTTP_GET,
            .handler = otto_servo_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &servo_get_uri);
        
        httpd_uri_t servo_save_uri = {
            .uri = "/servo_save",
            .method = HTTP_GET,
            .handler = otto_servo_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &servo_save_uri);
        
        // Mic gain handlers
        httpd_uri_t mic_gain_set_uri = {
            .uri = "/mic_gain_set",
            .method = HTTP_GET,
            .handler = otto_mic_gain_set_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mic_gain_set_uri);
        
        httpd_uri_t mic_gain_get_uri = {
            .uri = "/mic_gain_get",
            .method = HTTP_GET,
            .handler = otto_mic_gain_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mic_gain_get_uri);
        
        httpd_uri_t mic_gain_save_uri = {
            .uri = "/mic_gain_save",
            .method = HTTP_GET,
            .handler = otto_mic_gain_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mic_gain_save_uri);
        
        // Speed multiplier handlers
        httpd_uri_t speed_get_uri = {
            .uri = "/speed_get",
            .method = HTTP_GET,
            .handler = otto_speed_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &speed_get_uri);
        
        httpd_uri_t speed_save_uri = {
            .uri = "/speed_save",
            .method = HTTP_GET,
            .handler = otto_speed_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &speed_save_uri);
        
        // Delicious keyword handlers
        httpd_uri_t delicious_keyword_get_uri = {
            .uri = "/delicious_keyword_get",
            .method = HTTP_GET,
            .handler = otto_delicious_keyword_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &delicious_keyword_get_uri);
        
        httpd_uri_t delicious_keyword_save_uri = {
            .uri = "/delicious_keyword_save",
            .method = HTTP_GET,
            .handler = otto_delicious_keyword_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &delicious_keyword_save_uri);
        
        ESP_LOGI(TAG, "HTTP server started successfully (with UDP Drawing + Gemini API + Servo Calibration + Mic Gain support)");
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
            ESP_LOGI(TAG, "⏱️ Webserver will auto-stop in 5 minutes if not used");
        }
        
        webserver_manual_mode = true;  // Mark as manually started
        
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
        
        // Cleanup draw buffer when server stops to free 120KB
        cleanup_draw_buffer();
        
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
#include "sleep_timer.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

#define TAG "SleepTimer"


SleepTimer::SleepTimer(int seconds_to_light_sleep, int seconds_to_deep_sleep)
    : seconds_to_light_sleep_(seconds_to_light_sleep), seconds_to_deep_sleep_(seconds_to_deep_sleep) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<SleepTimer*>(arg);
            self->CheckTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sleep_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sleep_timer_));
}

SleepTimer::~SleepTimer() {
    esp_timer_stop(sleep_timer_);
    esp_timer_delete(sleep_timer_);
}

void SleepTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", true)) {
            ESP_LOGI(TAG, "Power save timer is disabled by settings");
            return;
        }

        ticks_ = 0;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(sleep_timer_, 1000000));
        ESP_LOGI(TAG, "Sleep timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(sleep_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Sleep timer disabled");
    }
}

void SleepTimer::OnEnterLightSleepMode(std::function<void()> callback) {
    on_enter_light_sleep_mode_ = callback;
}

void SleepTimer::OnExitLightSleepMode(std::function<void()> callback) {
    on_exit_light_sleep_mode_ = callback;
}

void SleepTimer::OnEnterDeepSleepMode(std::function<void()> callback) {
    on_enter_deep_sleep_mode_ = callback;
}

void SleepTimer::CheckTimer() {
    auto& app = Application::GetInstance();
    if (!app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    ticks_++;
    if (seconds_to_light_sleep_ != -1 && ticks_ >= seconds_to_light_sleep_) {
        if (!in_light_sleep_mode_) {
            in_light_sleep_mode_ = true;
            
            // Schedule the light sleep setup to avoid blocking esp_timer task
            // This prevents watchdog timeout on IDLE task
            app.Schedule([this]() {
                if (on_enter_light_sleep_mode_) {
                    on_enter_light_sleep_mode_();
                }

                auto& app = Application::GetInstance();
                auto& audio_service = app.GetAudioService();
                bool is_wake_word_running = audio_service.IsWakeWordRunning();
                if (is_wake_word_running) {
                    audio_service.EnableWakeWordDetection(false);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            
                // Create a separate task for light sleep loop to avoid blocking main event loop
                // Use mutex to prevent race condition
                std::lock_guard<std::mutex> lock(task_mutex_);
                if (!IsTaskRunning()) {
                    BaseType_t result = xTaskCreate(LightSleepTask, "light_sleep", 2048, this, 5, &light_sleep_task_handle_);
                    if (result == pdPASS) {
                        ESP_LOGI(TAG, "Created light sleep task");
                    } else {
                        ESP_LOGE(TAG, "Failed to create light sleep task");
                        light_sleep_task_handle_ = nullptr;
                    }
                } else {
                    ESP_LOGD(TAG, "Light sleep task already running");
                }

                if (is_wake_word_running) {
                    audio_service.EnableWakeWordDetection(true);
                }
            });
        }
    }
    if (seconds_to_deep_sleep_ != -1 && ticks_ >= seconds_to_deep_sleep_) {
        if (on_enter_deep_sleep_mode_) {
            on_enter_deep_sleep_mode_();
        }

        esp_deep_sleep_start();
    }
}

void SleepTimer::LightSleepTask(void* arg) {
    SleepTimer* self = static_cast<SleepTimer*>(arg);
    
    while (self->in_light_sleep_mode_) {
        auto& board = Board::GetInstance();
        board.GetDisplay()->UpdateStatusBar(true);
        lv_refr_now(nullptr);
        lvgl_port_stop();

        // Configure timer wakeup source (30 seconds)
        esp_sleep_enable_timer_wakeup(30 * 1000000);
        
        // Enter light sleep mode
        esp_light_sleep_start();
        lvgl_port_resume();

        auto wakeup_reason = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Wake up from light sleep, wakeup_reason: %d", wakeup_reason);
        if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
            break;
        }
    }
    
    // Update state before cleanup
    self->ticks_ = 0;
    if (self->in_light_sleep_mode_) {
        self->in_light_sleep_mode_ = false;
        if (self->on_exit_light_sleep_mode_) {
            self->on_exit_light_sleep_mode_();
        }
    }
    
    // Clean up task handle with mutex protection
    {
        std::lock_guard<std::mutex> lock(self->task_mutex_);
        self->light_sleep_task_handle_ = nullptr;
    }
    vTaskDelete(NULL);
}

bool SleepTimer::IsTaskRunning() const {
    if (light_sleep_task_handle_ == nullptr) {
        return false;
    }
    eTaskState state = eTaskGetState(light_sleep_task_handle_);
    return (state != eDeleted && state != eInvalid);
}

void SleepTimer::WakeUp() {
    ticks_ = 0;
    if (in_light_sleep_mode_) {
        in_light_sleep_mode_ = false;
        if (on_exit_light_sleep_mode_) {
            on_exit_light_sleep_mode_();
        }
    }
    
    // If task is running, it will clean up itself
    // If task is not running, clear the handle
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (light_sleep_task_handle_ != nullptr) {
        if (!IsTaskRunning()) {
            // Task already finished, clear handle
            light_sleep_task_handle_ = nullptr;
            ESP_LOGD(TAG, "Cleared light sleep task handle (task already finished)");
        } else {
            // Task is still running, it will clean up itself
            ESP_LOGD(TAG, "Light sleep task still running, will clean up itself");
        }
    }
}

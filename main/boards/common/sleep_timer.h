#pragma once

#include <functional>
#include <mutex>

#include <esp_timer.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class SleepTimer {
public:
    SleepTimer(int seconds_to_light_sleep = 20, int seconds_to_deep_sleep = -1);
    ~SleepTimer();

    void SetEnabled(bool enabled);
    void OnEnterLightSleepMode(std::function<void()> callback);
    void OnExitLightSleepMode(std::function<void()> callback);
    void OnEnterDeepSleepMode(std::function<void()> callback);
    void WakeUp();

private:
    void CheckTimer();
    static void LightSleepTask(void* arg);
    bool IsTaskRunning() const;

    esp_timer_handle_t sleep_timer_ = nullptr;
    TaskHandle_t light_sleep_task_handle_ = nullptr;
    mutable std::mutex task_mutex_;  // Protect task creation/deletion
    bool enabled_ = false;
    int ticks_ = 0;
    int seconds_to_light_sleep_;
    int seconds_to_deep_sleep_;
    bool in_light_sleep_mode_ = false;

    std::function<void()> on_enter_light_sleep_mode_;
    std::function<void()> on_exit_light_sleep_mode_;
    std::function<void()> on_enter_deep_sleep_mode_;
};

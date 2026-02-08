#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    // Low-level: send raw JSON text message to server (MQTT/WebSocket)
    bool SendRawText(const std::string& json_text);
    // Send text as STT message to server (simulates user input)
    bool SendSttMessage(const std::string& text);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    bool IsForcingSillyEmoji() const { return force_silly_emoji_.load(); }  // Check if forcing silly emoji for celebration
    bool IsForcingShockedEmoji() const { return force_shocked_emoji_.load(); }  // Check if forcing shocked emoji for shoot command
    bool IsForcingDeliciousEmoji() const { return force_delicious_emoji_.load(); }  // Check if forcing delicious emoji for custom keyword
    void ReloadCustomKeywords();  // Reload custom keywords from NVS into cache
    void SetShowingIpAddress(bool showing) { showing_ip_address_.store(showing); }  // Set IP display state
    bool IsShowingIpAddress() const { return showing_ip_address_.load(); }  // Check if showing IP address
    
    // Music streaming support methods
    bool IsAudioStopRequested() const { return audio_stop_requested_.load(); }
    void RequestAudioStop() { audio_stop_requested_.store(true); }
    void ClearAudioStopRequest() { audio_stop_requested_.store(false); }
    void SetMediaLowSramMode(bool enable) { media_low_sram_mode_.store(enable); }
    bool IsMediaLowSramMode() const { return media_low_sram_mode_.load(); }
    
    // Audio stop suppression (prevent AbortSpeaking during music state transitions)
    bool IsAudioStopSuppressed() const { return audio_stop_suppressed_.load(); }
    void SetAudioStopSuppressed(bool suppressed) { audio_stop_suppressed_.store(suppressed); }
    
    // Add audio data for playback (used by music streaming)
    void AddAudioData(AudioStreamPacket&& packet) {
        audio_service_.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(std::move(packet)), false);
    }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::atomic<bool> skip_voice_processing_for_listening_{false};  // Flag to skip voice processing when sending text from web
    std::atomic<bool> force_silly_emoji_{false};  // Flag to force "silly" emoji during celebration TTS
    std::atomic<bool> force_shocked_emoji_{false};  // Flag to force "shocked" emoji during shoot command
    std::atomic<bool> force_delicious_emoji_{false};  // Flag to force "delicious" emoji for custom keyword
    std::atomic<bool> force_winking_emoji_{false};  // Flag to force "winking" emoji when showing QR code
    std::atomic<bool> showing_ip_address_{false};  // Flag to indicate IP address is being displayed
    std::atomic<bool> audio_stop_requested_{false};  // Flag for music streaming stop request
    std::atomic<bool> audio_stop_suppressed_{false};  // Flag to suppress audio stop during state transitions
    std::atomic<bool> media_low_sram_mode_{false};  // Flag for low SRAM mode during media playback
    esp_timer_handle_t winking_emoji_timer_handle_ = nullptr;  // Timer to reset winking emoji after 30s
    std::string last_error_message_;
    AudioService audio_service_;

    // Cached custom keywords (loaded from NVS once, updated when changed)
    std::vector<std::string> cached_keywords_;  // Pre-split keywords for fast matching
    std::string cached_emoji_;  // Emoji to show when keyword matched
    std::string cached_pose_;   // Pose name to execute (sit/wave/bow/stretch/swing/dance)
    int8_t cached_action_slot_ = 0;  // Action slot to execute (memory slot 1-3)
    bool keywords_loaded_ = false;  // Whether keywords have been loaded from NVS

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_

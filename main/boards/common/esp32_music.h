#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <esp_heap_caps.h>

#include "music.h"

// 🎵 Custom PSRAM Allocator để std::vector dùng PSRAM thay vì SRAM
// Giúp tiết kiệm ~10-20KB SRAM khi streaming nhạc
template<typename T>
class PSRAMAllocator {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    PSRAMAllocator() noexcept = default;
    template<typename U> PSRAMAllocator(const PSRAMAllocator<U>&) noexcept {}
    
    T* allocate(size_t n) {
        if (n == 0) return nullptr;
        void* ptr = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ptr) {
            // Fallback to internal RAM if PSRAM fails
            ptr = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }
    
    void deallocate(T* p, size_t) noexcept {
        if (p) heap_caps_free(p);
    }
    
    template<typename U>
    bool operator==(const PSRAMAllocator<U>&) const noexcept { return true; }
    template<typename U>
    bool operator!=(const PSRAMAllocator<U>&) const noexcept { return false; }
};

// Type alias cho vector dùng PSRAM
template<typename T>
using PSRAMVector = std::vector<T, PSRAMAllocator<T>>;
// FFT spectrum đã bị xóa để giải phóng SRAM

#include <http.h>
#include <decoder/esp_audio_dec.h>
#include <decoder/impl/esp_aac_dec.h>
#include <esp_audio_types.h>

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,
        DISPLAY_MODE_LYRICS = 1     // 当前默认模式：仅显示歌词/消息
    };

private:
    enum class AudioStreamFormat {
        Unknown = 0,
        MP3,
        AAC_ADTS,
    };

    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    std::string current_artist_;
    std::string current_thumbnail_;  // YouTube thumbnail URL
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    
    // 显示状态缓存，避免重复更新
    std::string last_displayed_song_title_;
    std::string last_displayed_lyric_text_;
    int64_t last_display_update_time_ms_;  // 上次更新时间，用于throttle
    
    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::atomic<bool> is_stopping_;  // Guard to prevent spam StopStreaming() calls
    std::atomic<bool> is_preparing_;  // 🎵 Flag to indicate music is preparing to download (blocks TTS/LLM)
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    // Buffer size giảm để tiết kiệm RAM - tối ưu cho ESP32-S3
    // MAX giảm từ 256KB xuống 48KB, MIN giảm từ 32KB xuống 12KB
    static constexpr size_t MAX_BUFFER_SIZE = 48 * 1024;   // 48KB buffer (tối ưu RAM)
    static constexpr size_t MIN_BUFFER_SIZE = 12 * 1024;   // 12KB minimum playback buffer
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;

    // AAC解码器相关
    esp_audio_dec_handle_t aac_decoder_;
    esp_audio_dec_info_t aac_stream_info_;
    bool aac_decoder_initialized_;
    bool aac_info_ready_;

    std::atomic<AudioStreamFormat> stream_format_;
    
    // HTTP handle for immediate abort - dùng raw pointer để tiết kiệm SRAM
    // Http object được tạo bởi CreateHttp() và được quản lý bởi DownloadAudioStream
    Http* active_http_;
    std::mutex http_mutex_;
    
    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    bool InitializeAacDecoder();
    void CleanupAacDecoder();
    void AacPlaybackLoop();
    void FinishPlaybackCleanup(size_t total_played);
    void ResetSampleRate();  // 重置采样率到原始值
    void MonitorPsramUsage(); // 监控PSRAM使用情况
    AudioStreamFormat DetermineStreamFormat(const uint8_t* data, size_t size) const;
    bool IsLikelyMp3Frame(const uint8_t* data, size_t size) const;
    bool IsLikelyAacAdts(const uint8_t* data, size_t size) const;
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

public:
    Esp32Music();
    ~Esp32Music();

    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
  
    virtual std::string GetDownloadResult() override;
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming(bool send_notification = true) override;  // 停止流式播放, send_notification: send MCP notification
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return nullptr; }
    
    // 🎵 Check if music is preparing to download (blocks TTS/LLM to save SRAM)
    bool IsPreparing() const { return is_preparing_.load(); }

    void SetExternalSongTitle(const std::string& title);
    
    // 显示模式控制方法
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }
    
    // 检查是否正在播放音乐
    bool IsPlaying() const { return is_playing_.load(); }
    
    // Get current song info for web UI
    std::string GetCurrentSongName() const { return current_song_name_; }
    std::string GetCurrentArtist() const { return current_artist_; }
    std::string GetCurrentThumbnail() const { return current_thumbnail_; }
};

#endif // ESP32_MUSIC_H

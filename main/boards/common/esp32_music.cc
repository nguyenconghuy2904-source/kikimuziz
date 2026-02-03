#include "esp32_music.h"
#include "music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <thread>   // 为线程ID比较
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Music"

// ========== Simple Linear Resampler ==========
// silk resampler không hỗ trợ 44100Hz, nên dùng linear interpolation
static void linear_resample(const int16_t* input, int input_samples, 
                            int16_t* output, int output_samples,
                            int input_rate, int output_rate) {
    if (input_samples <= 0 || output_samples <= 0) return;
    
    // Simple linear interpolation resampling
    double ratio = (double)input_rate / (double)output_rate;
    
    for (int i = 0; i < output_samples; i++) {
        double src_idx = i * ratio;
        int idx0 = (int)src_idx;
        int idx1 = idx0 + 1;
        double frac = src_idx - idx0;
        
        if (idx1 >= input_samples) {
            idx1 = input_samples - 1;
        }
        if (idx0 >= input_samples) {
            idx0 = input_samples - 1;
        }
        
        // Linear interpolation
        output[i] = (int16_t)((1.0 - frac) * input[idx0] + frac * input[idx1]);
    }
}

static int get_resampled_samples(int input_samples, int input_rate, int output_rate) {
    return (int)((int64_t)input_samples * output_rate / input_rate);
}

// ========== 简单的ESP32认证函数 ==========

/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id() {
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "xiaozhi-music-server-2024";
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http* http) {
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000;  // 转换为秒
    
    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 添加认证头
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        ESP_LOGD(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", 
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建
// Helper function to normalize base URL (remove trailing slash)
static std::string normalizeBaseUrl(const std::string& url) {
    std::string normalized = url;
    // Remove trailing slash
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        
        pos = amp_pos + 1;
    }
    
    // 处理最后一个参数
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    
    return result_url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         last_displayed_song_title_(), last_displayed_lyric_text_(), 
                         last_display_update_time_ms_(0),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false),
                         is_stopping_(false), play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false), aac_decoder_(nullptr), aac_stream_info_(),
                         aac_decoder_initialized_(false), aac_info_ready_(false),
                         stream_format_(AudioStreamFormat::Unknown), active_http_(nullptr) {
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // Cleanup HTTP handle nếu còn (tiết kiệm SRAM)
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            delete active_http_;
            active_http_ = nullptr;
        }
    }
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待下载线程结束
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    
    // 等待播放线程结束
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        lyric_thread_.join();
    }
    
    // 清理缓冲区和解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
    CleanupAacDecoder();
    
    // FFT spectrum đã bị xóa để giải phóng SRAM
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Searching for: %s", song_name.c_str());
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();
    
    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    
    // 第一步：请求stream_pcm接口获取音频信息
    // 从Settings读取音乐服务器地址
    Settings settings("wifi", false);
    std::string base_url_raw = settings.GetString("music_srv", "https://nhacminiz.minizjp.com/");
    // Normalize base URL (remove trailing slash if present)
    std::string base_url = normalizeBaseUrl(base_url_raw);
    ESP_LOGI(TAG, "Using music server: %s (normalized from: %s)", base_url.c_str(), base_url_raw.c_str());
    std::string full_url = base_url + "/stream_pcm?song=" + url_encode(song_name) + "&artist=" + url_encode(artist_name);
    
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    // 使用Board提供的HTTP客户端 - với retry logic cho DNS errors
    auto network = Board::GetInstance().GetNetwork();
    
    const int max_retries = 3;
    int retry_count = 0;
    bool connected = false;
    std::unique_ptr<Http> http;
    
    while (retry_count < max_retries && !connected) {
        if (retry_count > 0) {
            ESP_LOGW(TAG, "Retrying connection (attempt %d/%d)...", retry_count + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second before retry
        }
        
        http = network->CreateHttp(0);
        
        // 设置超时时间（60秒）
        http->SetTimeout(60000);
        
        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "application/json");
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        // 打开GET连接
        if (http->Open("GET", full_url)) {
            connected = true;
        } else {
            ESP_LOGW(TAG, "Connection attempt %d failed (DNS or network error)", retry_count + 1);
            retry_count++;
        }
    }
    
    if (!connected) {
        ESP_LOGE(TAG, "Failed to connect to music API after %d retries", max_retries);
        return false;
    }

    // Check if stop requested (user pressed button) before continuing
    auto& app = Application::GetInstance();
    if (app.IsAudioStopRequested()) {
        ESP_LOGI(TAG, "Audio stop requested during Download(), canceling");
        http->Close();
        return false;
    }

    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }

    // Check again before reading response (user might have pressed button)
    if (app.IsAudioStopRequested()) {
        ESP_LOGI(TAG, "Audio stop requested before reading response, canceling");
        http->Close();
        return false;
    }

    // 读取响应数据
    last_downloaded_data_ = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, (int)last_downloaded_data_.length());
    ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    // 简单的认证响应检查（可选）
    if (last_downloaded_data_.find("ESP32动态密钥验证失败") != std::string::npos) {
        ESP_LOGE(TAG, "Authentication failed for song: %s", song_name.c_str());
        return false;
    }
    
    if (!last_downloaded_data_.empty()) {
        // 解析响应JSON以提取音频URL
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            // 提取关键信息
            cJSON* artist = cJSON_GetObjectItem(response_json, "artist");
            cJSON* title = cJSON_GetObjectItem(response_json, "title");
            cJSON* audio_url = cJSON_GetObjectItem(response_json, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(response_json, "lyric_url");
            
            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Artist: %s", artist->valuestring);
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Title: %s", title->valuestring);
            }
            
            // 检查audio_url是否有效
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                ESP_LOGI(TAG, "Audio URL path: %s", audio_url->valuestring);
                
                // 第二步：拼接完整的音频下载URL，确保对audio_url进行URL编码
                std::string audio_path = audio_url->valuestring;
                
                // Ensure audio_path starts with /
                if (!audio_path.empty() && audio_path[0] != '/') {
                    audio_path = "/" + audio_path;
                }
                
                // 使用统一的URL构建功能
                if (audio_path.find("?") != std::string::npos) {
                    size_t query_pos = audio_path.find("?");
                    std::string path = audio_path.substr(0, query_pos);
                    std::string query = audio_path.substr(query_pos + 1);
                    
                    current_music_url_ = buildUrlWithParams(base_url, path, query);
                } else {
                    current_music_url_ = base_url + audio_path;
                }
                
                // Check if stop requested before starting streaming (user might have pressed button)
                if (app.IsAudioStopRequested()) {
                    ESP_LOGI(TAG, "Audio stop requested before StartStreaming(), canceling");
                    cJSON_Delete(response_json);
                    return false;
                }
                
                ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
                ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
                song_name_displayed_ = false;  // 重置歌名显示标志
                StartStreaming(current_music_url_);
                
                // 处理歌词URL - 只有在歌词显示模式下且未启用低SRAM模式才启动歌词
                bool low_sram_mode = Application::GetInstance().IsMediaLowSramMode();
                if (!low_sram_mode && cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    // 拼接完整的歌词下载URL，使用相同的URL构建逻辑
                    std::string lyric_path = lyric_url->valuestring;
                    
                    // Ensure lyric_path starts with /
                    if (!lyric_path.empty() && lyric_path[0] != '/') {
                        lyric_path = "/" + lyric_path;
                    }
                    
                    if (lyric_path.find("?") != std::string::npos) {
                        size_t query_pos = lyric_path.find("?");
                        std::string path = lyric_path.substr(0, query_pos);
                        std::string query = lyric_path.substr(query_pos + 1);
                        
                        current_lyric_url_ = buildUrlWithParams(base_url, path, query);
                    } else {
                        current_lyric_url_ = base_url + lyric_path;
                    }
                    
                    // 根据显示模式决定是否启动歌词
                    if (display_mode_ == DISPLAY_MODE_LYRICS) {
                        ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());
                        
                        // 启动歌词下载和显示
                        if (is_lyric_running_) {
                            is_lyric_running_ = false;
                            if (lyric_thread_.joinable()) {
                                lyric_thread_.join();
                            }
                        }
                        
                        is_lyric_running_ = true;
                        current_lyric_index_ = -1;
                        lyrics_.clear();
                        
                        auto default_cfg = esp_pthread_get_default_config();
                        esp_pthread_cfg_t lyric_cfg = default_cfg;
                        lyric_cfg.stack_size = 4096;  // 4KB stack cho lyric parsing (cần đủ cho parse file lyrics lớn)
                        lyric_cfg.prio = 4;
                        lyric_cfg.thread_name = "lyric_disp";
                        esp_pthread_set_cfg(&lyric_cfg);
                        try {
                            lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                        } catch (const std::system_error& e) {
                            ESP_LOGE(TAG, "Failed to create lyric display thread: %s", e.what());
                            is_lyric_running_ = false;
                        }
                        esp_pthread_set_cfg(&default_cfg);
                    } else {
                        ESP_LOGI(TAG, "Lyric URL found but spectrum display mode is active, skipping lyrics");
                    }
                } else {
                    if (low_sram_mode) {
                        ESP_LOGI(TAG, "Low-SRAM media mode: skip lyrics to save SRAM");
                    } else {
                        // Only log warning if lyric URL is actually missing (not due to low-SRAM mode)
                        if (!cJSON_IsString(lyric_url) || !lyric_url->valuestring || strlen(lyric_url->valuestring) == 0) {
                            ESP_LOGD(TAG, "No lyric URL found for this song (this is normal for some songs)");
                        }
                    }
                }
                
                cJSON_Delete(response_json);
                return true;
            } else {
                // audio_url为空或无效
                ESP_LOGE(TAG, "Audio URL not found or empty for song: %s", song_name.c_str());
                ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    
    return false;
}



std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    // Enable low-SRAM media mode while streaming
    Application::GetInstance().SetMediaLowSramMode(true);
    
    // 🔇 Disable wake word detection to free up SRAM for SSL/TLS operations
    // Wake word uses ~15-20KB SRAM which is needed for HTTPS download
    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.EnableWakeWordDetection(false);
    ESP_LOGI(TAG, "🔇 Disabled wake word detection to free SRAM for music streaming");
    
    // Reset stopping flag before starting new stream
    is_stopping_.store(false, std::memory_order_release);
    
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // 等待之前的线程完全结束（包括歌词线程）
    if (lyric_thread_.joinable()) {
        lyric_thread_.join();
        lyric_thread_ = std::thread();
    }
    
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        download_thread_.join();
        download_thread_ = std::thread();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        play_thread_.join();
        play_thread_ = std::thread();
    }
    
    // 清空歌词状态
    lyrics_.clear();
    current_lyric_index_ = -1;
    
    // 清空缓冲区和解码器状态
    ClearAudioBuffer();
    CleanupMp3Decoder();
    CleanupAacDecoder();
    stream_format_.store(AudioStreamFormat::Unknown, std::memory_order_relaxed);
    aac_info_ready_ = false;
    
    // 重置显示标志
    song_name_displayed_ = false;
    last_displayed_song_title_.clear();
    last_displayed_lyric_text_.clear();
    last_display_update_time_ms_ = 0;
    
    // 等待一小段时间确保资源被完全释放
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 在创建线程前打印/检查可用内存
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI("Memory", "Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "Free PSRAM: %d bytes", (int)free_psram);
    ESP_LOGI(TAG, "Free heap: %u, Free PSRAM: %u", (unsigned)free_heap, (unsigned)free_psram);
    
    // Clear the buffer before starting new stream
    ClearAudioBuffer();
    
    // Configure thread stack size to avoid stack overflow (reference: TienHuyIoT)
    // Using 5KB stack size - increased from 3KB to prevent stack overflow during playback
    // Stack is needed for: std::unique_lock, std::vector, local variables, decoder calls
    // Use PSRAM for stack to save internal SRAM (only ~20KB available)
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 1024 * 5;  // 5KB stack size - safe for decoder operations
    cfg.prio = 5;               // Medium priority
    cfg.thread_name = "audio_stream";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // Use PSRAM for stack
    esp_pthread_set_cfg(&cfg);
    
    // 开始下载线程
    is_downloading_ = true;
    ESP_LOGI(TAG, "Creating download thread with 5KB stack");
    try {
        download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    } catch (const std::system_error& e) {
        ESP_LOGE(TAG, "Failed to create download thread: %s", e.what());
        is_downloading_ = false;
        return false;
    }
    
    // 开始播放线程 (will wait for buffer to have enough data)
    is_playing_ = true;
    ESP_LOGI(TAG, "Creating play thread with 5KB stack");
    try {
        play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    } catch (const std::system_error& e) {
        ESP_LOGE(TAG, "Failed to create play thread: %s", e.what());
        is_playing_ = false;
        // Stop download thread
        is_downloading_ = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    return true;
}

void Esp32Music::SetExternalSongTitle(const std::string& title) {
    current_song_name_ = title;
    song_name_displayed_ = false;
}

// 停止流式播放
bool Esp32Music::StopStreaming(bool send_notification) {
    // Guard: prevent spam calls - if already stopping or stopped, return early
    bool expected = false;
    if (!is_stopping_.compare_exchange_strong(expected, true)) {
        // Already stopping or stopped, skip
        return true;
    }
    
    // Debounce: Add 100ms delay to prevent rapid repeated stops
    // This prevents "súng đột" (rapid fire) when button is pressed multiple times quickly
    static uint64_t last_stop_time = 0;
    uint64_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
    if (last_stop_time > 0 && (current_time - last_stop_time) < 100) {
        // Less than 100ms since last stop, reset guard and return
        is_stopping_.store(false, std::memory_order_release);
        ESP_LOGD(TAG, "StopStreaming() debounced - too soon after last stop");
        return true;
    }
    last_stop_time = current_time;
    
    ESP_LOGI(TAG, "StopStreaming() called - starting fast stop (notify=%d)", send_notification);
    
    // Phase 1: Stop immediately (<100ms)
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;
    
    // ⚡ Close HTTP connection immediately to abort download
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            ESP_LOGI(TAG, "Closing HTTP connection immediately");
            active_http_->Close();
            delete active_http_;  // Cleanup HTTP object
            active_http_ = nullptr;
        }
    }
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 清空歌名显示 - sync call for immediate feedback
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // Sync call instead of Schedule
    }
    // Disable low-SRAM media mode when stopping
    Application::GetInstance().SetMediaLowSramMode(false);
    
    // 🔊 Re-enable wake word detection after music stops
    // This restores normal voice assistant functionality
    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "🔊 Re-enabled wake word detection after music stopped");
    
    // 重置采样率到原始值
    ResetSampleRate();
    
    current_song_name_.clear();
    song_name_displayed_ = false;

    // Gửi MCP notification lên server để AI biết đã stop nhạc/radio
    // Chỉ gửi khi thực sự stop (không phải khi chuyển bài)
    if (send_notification) {
        auto& app = Application::GetInstance();
        app.Schedule([]() {
            // Gửi MCP notification: music stopped
            std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/music_stopped\",\"params\":{}}";
            Application::GetInstance().SendMcpMessage(payload);
            ESP_LOGI(TAG, "Sent MCP notification: music_stopped to server");
        });
    }

    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        return true;
    }
    
    // 记录停止前的内存状态
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("Memory", "Before stop - Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "Before stop - Free PSRAM: %d bytes", (int)free_psram);
    
    // Phase 2: Cleanup threads (non-blocking with timeout)
    // 使用detach避免长时间等待，cleanup在background进行
    if (download_thread_.joinable()) {
        uintptr_t current_val = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
        uintptr_t download_val = static_cast<uintptr_t>(download_thread_.native_handle());
        if (download_val == current_val) {
            download_thread_.detach();
        } else {
            // Try join with timeout (non-blocking)
            auto start = std::chrono::steady_clock::now();
            bool joined = false;
            while (download_thread_.joinable() && 
                   (std::chrono::steady_clock::now() - start) < std::chrono::milliseconds(100)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (!download_thread_.joinable()) {
                    joined = true;
                    break;
                }
            }
            if (!joined && download_thread_.joinable()) {
                // Timeout - detach instead of waiting
                ESP_LOGW(TAG, "Download thread join timeout, detaching");
                download_thread_.detach();
            } else if (download_thread_.joinable()) {
                download_thread_.join();
            }
        }
        download_thread_ = std::thread();
    }
    
    if (play_thread_.joinable()) {
        uintptr_t current_val = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
        uintptr_t play_val = static_cast<uintptr_t>(play_thread_.native_handle());
        if (play_val == current_val) {
            play_thread_.detach();
        } else {
            // Try join with timeout (non-blocking)
            auto start = std::chrono::steady_clock::now();
            bool joined = false;
            while (play_thread_.joinable() && 
                   (std::chrono::steady_clock::now() - start) < std::chrono::milliseconds(100)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (!play_thread_.joinable()) {
                    joined = true;
                    break;
                }
            }
            if (!joined && play_thread_.joinable()) {
                // Timeout - detach instead of waiting
                ESP_LOGW(TAG, "Play thread join timeout, detaching");
                play_thread_.detach();
            } else if (play_thread_.joinable()) {
                play_thread_.join();
            }
        }
        play_thread_ = std::thread();
    }
    
    // FFT spectrum đã bị xóa để giải phóng SRAM, không cần stopFft() nữa

    CleanupMp3Decoder();
    CleanupAacDecoder();
    stream_format_.store(AudioStreamFormat::Unknown, std::memory_order_relaxed);
    aac_info_ready_ = false;

    // 清理FFT buffer PSRAM khi chuyển bài
    // 记录停止后的内存状态
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("Memory", "After stop - Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "After stop - Free PSRAM: %d bytes", (int)free_psram);
    
    // Reset stopping flag to allow next stream
    is_stopping_.store(false, std::memory_order_release);
    
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // ⚡ Lưu HTTP handle để có thể abort ngay khi stop - dùng raw pointer tiết kiệm SRAM
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        active_http_ = http.release();  // Transfer ownership từ unique_ptr sang raw pointer
    }
    
    // 设置基本请求头和超时
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (!active_http_) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            is_downloading_ = false;
            return;
        }
        active_http_->SetTimeout(60000);  // 60秒超时
        active_http_->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        active_http_->SetHeader("Accept", "*/*");
        active_http_->SetHeader("Range", "bytes=0-");  // 支持断点续传
        
        // 添加ESP32认证头
        add_auth_headers(active_http_);
        
        if (!active_http_->Open("GET", music_url)) {
            ESP_LOGE(TAG, "Failed to connect to music stream URL");
            delete active_http_;  // Cleanup khi fail
            active_http_ = nullptr;
            is_downloading_ = false;
            return;
        }
    }
    
    int status_code = 0;
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (!active_http_) {
            is_downloading_ = false;
            return;
        }
        status_code = active_http_->GetStatusCode();
        if (status_code != 200 && status_code != 206) {  // 206 for partial content
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            active_http_->Close();
            delete active_http_;  // Cleanup khi fail
            active_http_ = nullptr;
            is_downloading_ = false;
            return;
        }
    }
    
    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
    
    // Chunk size theo repo gốc: 4KB
    const size_t chunk_size = 4096;  // 4KB mỗi khối (giống repo gốc để ổn định)

    auto allocate_psram = [&](size_t size, const char* label) -> uint8_t* {
        const int max_retries = 3;
        for (int attempt = 0; attempt < max_retries && is_downloading_; ++attempt) {
            uint8_t* ptr = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (ptr) {
                return ptr;
            }
            ESP_LOGW(TAG, "PSRAM allocation failed for %s (%u bytes), retry %d/%d", label, (unsigned)size, attempt + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return nullptr;
    };

    uint8_t* buffer = allocate_psram(chunk_size, "download buffer");
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer in PSRAM (%u bytes)", (unsigned)chunk_size);
        http->Close();
        is_downloading_ = false;
        return;
    }
    size_t total_downloaded = 0;
    
    while (is_downloading_ && is_playing_) {
        // Stack safety log every ~512 iterations
        static int __dl_cnt = 0;
        if (((++__dl_cnt) & 0x1FF) == 0) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            if (hw < 512) { ESP_LOGW(TAG, "audio_dl low stack: %u words", (unsigned)hw); }
        }

        int bytes_read = 0;
        {
            std::lock_guard<std::mutex> lock(http_mutex_);
            if (!active_http_) {
                break;  // HTTP đã bị close
            }
            bytes_read = active_http_->Read((char*)buffer, chunk_size);
        }
        if (bytes_read < 0) {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        
        // 检测文件格式
        if (bytes_read >= 4) {
            auto current_format = stream_format_.load(std::memory_order_relaxed);
            if (current_format == AudioStreamFormat::Unknown) {
                auto detected = DetermineStreamFormat(buffer, bytes_read);
                if (detected != AudioStreamFormat::Unknown) {
                    stream_format_.store(detected, std::memory_order_release);
                    if (detected == AudioStreamFormat::AAC_ADTS) {
                        ESP_LOGI(TAG, "Detected AAC (ADTS) stream");
                    } else if (detected == AudioStreamFormat::MP3) {
                        ESP_LOGI(TAG, "Detected MP3 stream");
                    }
                } else if (total_downloaded == 0) {
                    ESP_LOGI(TAG, "Unknown initial format: %02X %02X %02X %02X",
                             (unsigned char)buffer[0], (unsigned char)buffer[1],
                             (unsigned char)buffer[2], (unsigned char)buffer[3]);
                }
            }
        }
        
        // 创建音频数据块 - 优先使用PSRAM
        uint8_t* chunk_data = allocate_psram(bytes_read, "audio chunk");
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk in PSRAM (size: %d bytes)", bytes_read);
            ESP_LOGE(TAG, "Chunk size: %d bytes, buffer_size: %d", bytes_read, buffer_size_);
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
        
        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            
            // 监控memory mỗi 50 chunks để tránh spam log
            if (total_downloaded % (chunk_size * 50) == 0) {
                MonitorPsramUsage();
            }
            
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                
                // 通知播放线程有新数据
                buffer_cv_.notify_one();
                
                if (total_downloaded % (1024 * 1024) == 0) {  // 每1MB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %u MB, buffer: %u KB", (unsigned int)(total_downloaded / (1024*1024)), (unsigned int)(buffer_size_ / 1024));
                    // 定期监控内存使用情况
                    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    ESP_LOGI("Memory", "During download - Free Internal SRAM: %d bytes", (int)free_sram);
                    ESP_LOGI("Memory", "During download - Free PSRAM: %d bytes", (int)free_psram);
                }
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
        // nhường CPU nhẹ để tránh WDT khi tải liên tục
        vTaskDelay(1);
    }

    if (buffer) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    
    // Cleanup HTTP handle
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            delete active_http_;  // Cleanup HTTP object
            active_http_ = nullptr;
        }
    }
    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        is_playing_ = false;
        return;
    }
    if (!codec->output_enabled()) {
        // Ensure speaker output is enabled before playback
        codec->EnableOutput(true);
    }
    
    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }

    if (stream_format_.load(std::memory_order_acquire) == AudioStreamFormat::Unknown) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (!audio_buffer_.empty()) {
            const AudioChunk& front = audio_buffer_.front();
            if (front.data && front.size > 0) {
                auto detected = DetermineStreamFormat(front.data, front.size);
                if (detected != AudioStreamFormat::Unknown) {
                    stream_format_.store(detected, std::memory_order_release);
                }
            }
        }
    }

    AudioStreamFormat format = stream_format_.load(std::memory_order_acquire);
    if (format == AudioStreamFormat::Unknown) {
        format = AudioStreamFormat::MP3;
        stream_format_.store(format, std::memory_order_release);
        ESP_LOGW(TAG, "Stream format not detected from data, defaulting to MP3 decoder");
    }

    if (format == AudioStreamFormat::AAC_ADTS) {
        if (!InitializeAacDecoder()) {
            ESP_LOGE(TAG, "Failed to initialize AAC decoder");
            is_playing_ = false;
            return;
        }
    } else {
        if (!mp3_decoder_initialized_) {
            if (!InitializeMp3Decoder()) {
                ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
                is_playing_ = false;
                return;
            }
        }
    }
    
    ESP_LOGI(TAG, "Starting playback, buffer: %u KB", (unsigned int)(buffer_size_ / 1024));
    
    // 监控memory trước khi bắt đầu phát
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("Memory", "Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "Free PSRAM: %d bytes", (int)free_psram);
    MonitorPsramUsage();

    if (format == AudioStreamFormat::AAC_ADTS) {
        AacPlaybackLoop();
        return;
    }
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    size_t mp3_buffer_size = 0;  // 记录buffer size
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区 - 必须使用PSRAM để tránh tiêu tốn SRAM
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // 8KB giống repo gốc
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer in PSRAM (8192 bytes)");
        is_playing_ = false;
        return;
    }
    mp3_buffer_size = 8192;
    
    // 记录分配缓冲区后的内存状态
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("Memory", "After buffer allocation - Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "After buffer allocation - Free PSRAM: %d bytes", (int)free_psram);
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    
    // PCM accumulation để giảm giật/rè - threshold 70ms
    // Reserve capacity để tránh reallocation và giảm SRAM fragmentation
    std::vector<int16_t> pcm_accum;
    {
        bool low_sram_mode = Application::GetInstance().IsMediaLowSramMode();
        pcm_accum.reserve(low_sram_mode ? 800 : 4000);
    }
    int accum_sample_rate = 0;
    
    // 🎵 Resampler config (using linear resampling for 44100Hz which silk doesn't support)
    int resampler_output_rate = codec->output_sample_rate();
    std::vector<int16_t> resample_buffer;  // Buffer cho PCM đã resample
    
    // Allocate PCM heap buffer once to avoid large stack usage - chỉ dùng PSRAM
    int16_t* pcm_buffer_heap = (int16_t*)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // 2304 samples giống repo gốc
    if (!pcm_buffer_heap) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer heap in PSRAM (%d bytes)", (int)(2304 * sizeof(int16_t)));
        is_playing_ = false;
        heap_caps_free(mp3_input_buffer);
        mp3_input_buffer = nullptr;
        return;
    }
    
    // 记录所有缓冲区分配完成后的内存状态
    free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("Memory", "All buffers allocated - Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "All buffers allocated - Free PSRAM: %d bytes", (int)free_psram);

    // 立即显示歌曲名称和歌词（如果有）
    UpdateLyricDisplay(0);

    while (is_playing_) {
        // Stack high-water mark logging (every ~512 iterations)
        static int __hw_cnt = 0;
        if (((++__hw_cnt) & 0x1FF) == 0) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            if (hw < 512) {
                ESP_LOGW(TAG, "audio_play low stack: %u words", (unsigned)hw);
            }
        }
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            bool prev_suppressed = app.IsAudioStopSuppressed();
            app.SetAudioStopSuppressed(true);
            app.ToggleChatState();
            app.SetAudioStopSuppressed(prev_suppressed);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            std::string formatted_song_name = "Đang phát 《" + current_song_name_ + "》...";
            auto& app_sched = Application::GetInstance();
            app_sched.Schedule([formatted_song_name]() {
                auto disp = Board::GetInstance().GetDisplay();
                if (disp) { disp->SetMusicInfo(formatted_song_name.c_str()); }
            });
            song_name_displayed_ = true;

            // Spectrum visualization disabled
        }
        
        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码 (8KB buffer, giống repo gốc)
            AudioChunk chunk;
            
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间 - 使用动态buffer size
                size_t space_available = mp3_buffer_size - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
    // 解码MP3帧（使用堆缓冲，tránh chiếm stack lớn）
    if (!pcm_buffer_heap) { break; }
    int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer_heap, 0);
        
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer_heap;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    // Reserve để tránh reallocation
                    mono_buffer.reserve(mono_samples);
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer_heap[i * 2];      // 左声道
                        int right = pcm_buffer_heap[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                }
                
                // PCM accumulation với threshold 70ms
                if (accum_sample_rate == 0) {
                    accum_sample_rate = mp3_frame_info_.samprate;
                }
                
                // Kiểm tra sample rate consistency
                if (accum_sample_rate != mp3_frame_info_.samprate) {
                    accum_sample_rate = mp3_frame_info_.samprate; // Update to current frame rate
                }
                pcm_accum.insert(pcm_accum.end(), final_pcm_data, final_pcm_data + final_sample_count);

                // Threshold 70ms: sample_rate / 14.3 (3087 samples @ 44.1kHz) - cân bằng chất lượng và hiệu suất
                int threshold_samples = accum_sample_rate > 0 ? (accum_sample_rate * 7 / 100) : 3087;
                if ((int)pcm_accum.size() >= threshold_samples) {
                    // 🔊 Resample PCM data then output to codec
                    // MP3 typically decodes at 44100Hz, codec output is usually 24000Hz/16000Hz
                    // Using linear resampling since silk resampler doesn't support 44100Hz
                    if (accum_sample_rate != resampler_output_rate) {
                        int output_samples = get_resampled_samples(pcm_accum.size(), accum_sample_rate, resampler_output_rate);
                        resample_buffer.resize(output_samples);
                        linear_resample(pcm_accum.data(), pcm_accum.size(), 
                                       resample_buffer.data(), output_samples,
                                       accum_sample_rate, resampler_output_rate);
                        ESP_LOGD(TAG, "Resampled: %d Hz (%d samples) -> %d Hz (%d samples)", 
                                accum_sample_rate, (int)pcm_accum.size(), 
                                resampler_output_rate, output_samples);
                        codec->OutputData(resample_buffer);
                        total_played += resample_buffer.size() * sizeof(int16_t);
                    } else {
                        // Same sample rate, no resampling needed
                        codec->OutputData(pcm_accum);
                        total_played += pcm_accum.size() * sizeof(int16_t);
                    }

                    pcm_accum.clear();
                    // Giữ capacity để tránh reallocation - không shrink_to_fit ở đây
                    
                    // 🔄 Yield CPU to prevent watchdog timeout
                    // MP3 decode loop can be CPU-intensive, need to let other tasks run
                    vTaskDelay(1);
                }
                
                // 打印播放进度
                if (total_played % (1024 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %u MB, buffer: %u KB", (unsigned int)(total_played / (1024*1024)), (unsigned int)(buffer_size_ / 1024));
                    // 定期监控内存使用情况
                    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    ESP_LOGI("Memory", "During playback - Free Internal SRAM: %d bytes", (int)free_sram);
                    ESP_LOGI("Memory", "During playback - Free PSRAM: %d bytes", (int)free_psram);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            
            // 跳过一些字节继续尝试
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
            }
        }
    }
    
    // Gửi phần PCM còn lại nếu có - resample và output qua codec
    if (!pcm_accum.empty()) {
        // Resample remaining PCM using linear resampling
        if (accum_sample_rate != resampler_output_rate && accum_sample_rate > 0) {
            int output_samples = get_resampled_samples(pcm_accum.size(), accum_sample_rate, resampler_output_rate);
            resample_buffer.resize(output_samples);
            linear_resample(pcm_accum.data(), pcm_accum.size(), 
                           resample_buffer.data(), output_samples,
                           accum_sample_rate, resampler_output_rate);
            codec->OutputData(resample_buffer);
            total_played += resample_buffer.size() * sizeof(int16_t);
        } else {
            codec->OutputData(pcm_accum);
            total_played += pcm_accum.size() * sizeof(int16_t);
        }
        pcm_accum.clear();
    }

    // Cleanup allocated buffers
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
        mp3_input_buffer = nullptr;
    }
    if (pcm_buffer_heap) {
        heap_caps_free(pcm_buffer_heap);
        pcm_buffer_heap = nullptr;
    }
    // 清理PCM accumulation buffer
    pcm_accum.clear();
    pcm_accum.shrink_to_fit(); // Giải phóng memory

    FinishPlaybackCleanup(total_played);
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
}

bool Esp32Music::InitializeAacDecoder() {
    if (aac_decoder_initialized_) {
        return true;
    }

    static std::atomic<bool> aac_registered{false};
    if (!aac_registered.load(std::memory_order_acquire)) {
        esp_audio_err_t reg_ret = esp_aac_dec_register();
        if (reg_ret != ESP_AUDIO_ERR_OK && reg_ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "Failed to register AAC decoder: %d", reg_ret);
            return false;
        }
        aac_registered.store(true, std::memory_order_release);
    }

    esp_audio_dec_cfg_t config = {
        .type = ESP_AUDIO_TYPE_AAC,
        .cfg = nullptr,
        .cfg_sz = 0,
    };

    esp_audio_dec_handle_t handle = nullptr;
    esp_audio_err_t open_ret = esp_audio_dec_open(&config, &handle);
    if (open_ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open AAC decoder: %d", open_ret);
        return false;
    }

    aac_decoder_ = handle;
    aac_decoder_initialized_ = true;
    aac_info_ready_ = false;
    memset(&aac_stream_info_, 0, sizeof(aac_stream_info_));
    return true;
}

void Esp32Music::CleanupAacDecoder() {
    if (aac_decoder_) {
        esp_audio_dec_close(aac_decoder_);
        aac_decoder_ = nullptr;
    }
    aac_decoder_initialized_ = false;
    aac_info_ready_ = false;
    memset(&aac_stream_info_, 0, sizeof(aac_stream_info_));
}

void Esp32Music::FinishPlaybackCleanup(size_t total_played) {
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Playback finished, played: %d MB", (int)(total_played / (1024 * 1024)));
    ESP_LOGI("Memory", "After cleanup - Free Internal SRAM: %d bytes", (int)free_sram);
    ESP_LOGI("Memory", "After cleanup - Free PSRAM: %d bytes", (int)free_psram);
    MonitorPsramUsage();

    is_playing_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        auto& app_sched = Application::GetInstance();
        app_sched.Schedule([]() {
            auto disp = Board::GetInstance().GetDisplay();
            if (disp) {
                disp->SetMusicInfo("");
            }
        });
    }
}

void Esp32Music::AacPlaybackLoop() {
    ESP_LOGI(TAG, "Using AAC decoder for playback");

    // Get codec for direct PCM output
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available for AAC playback");
        is_playing_ = false;
        return;
    }

    const size_t input_buffer_capacity = 8192;  // 8KB giống repo gốc để ổn định
    uint8_t* input_buffer = (uint8_t*)heap_caps_malloc(input_buffer_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate AAC input buffer (%u bytes)", (unsigned)input_buffer_capacity);
        is_playing_ = false;
        return;
    }

    uint8_t* read_ptr = input_buffer;
    int bytes_left = 0;

    size_t pcm_capacity_bytes = 4096 * sizeof(int16_t);  // 4096 samples giống repo gốc
    int16_t* pcm_buffer = (int16_t*)heap_caps_malloc(pcm_capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate AAC PCM buffer (%u bytes)", (unsigned)pcm_capacity_bytes);
        heap_caps_free(input_buffer);
        is_playing_ = false;
        return;
    }

    std::vector<int16_t> pcm_accum;
    pcm_accum.reserve(4000);  // Giống repo gốc để ổn định
    std::vector<int16_t> mono_buffer;
    mono_buffer.reserve(2048);  // Giống repo gốc

    // 🎵 Resampler config (using linear resampling for sample rates silk doesn't support)
    int resampler_output_rate = codec->output_sample_rate();
    std::vector<int16_t> resample_buffer;

    size_t total_played = 0;
    int accum_sample_rate = 0;

    // 立即显示歌曲名称和歌词（如果有）
    UpdateLyricDisplay(0);

    while (is_playing_) {
        static int __hw_cnt = 0;
        if (((++__hw_cnt) & 0x1FF) == 0) {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            if (hw < 512) {
                ESP_LOGW(TAG, "audio_play(AAC) low stack: %u words", (unsigned)hw);
            }
        }

        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            bool prev_suppressed = app.IsAudioStopSuppressed();
            app.SetAudioStopSuppressed(true);
            app.ToggleChatState();
            app.SetAudioStopSuppressed(prev_suppressed);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!song_name_displayed_ && !current_song_name_.empty()) {
            std::string formatted_song_name = "Đang phát 《" + current_song_name_ + "》...";
            auto& app_sched = Application::GetInstance();
            app_sched.Schedule([formatted_song_name]() {
                auto disp = Board::GetInstance().GetDisplay();
                if (disp) {
                    disp->SetMusicInfo(formatted_song_name.c_str());
                }
            });
            song_name_displayed_ = true;
        }

        if (bytes_left < 4096) {  // 4KB giống repo gốc để ổn định
            AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        break;
                    }
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        if (!is_downloading_) {
                            break;
                        }
                        continue;
                    }
                }

                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                buffer_cv_.notify_one();
            }

            if (chunk.data && chunk.size > 0) {
                if (bytes_left > 0 && read_ptr != input_buffer) {
                    memmove(input_buffer, read_ptr, bytes_left);
                    read_ptr = input_buffer;
                }

                size_t space_available = input_buffer_capacity - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                memcpy(input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = input_buffer;
                heap_caps_free(chunk.data);
            }
        }

        if (bytes_left <= 0) {
            if (!is_downloading_) {
                break;
            }
            vTaskDelay(1);
            continue;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = read_ptr;
        raw.len = bytes_left;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t out_frame = {};
        out_frame.buffer = reinterpret_cast<uint8_t*>(pcm_buffer);
        out_frame.len = pcm_capacity_bytes;
        out_frame.decoded_size = 0;

        esp_audio_err_t dec_ret = esp_audio_dec_process(aac_decoder_, &raw, &out_frame);

        if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            size_t new_size = out_frame.needed_size ? out_frame.needed_size : pcm_capacity_bytes * 2;
            int16_t* new_buffer = (int16_t*)heap_caps_realloc(pcm_buffer, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!new_buffer) {
                ESP_LOGE(TAG, "Failed to expand AAC PCM buffer to %u bytes", (unsigned)new_size);
                break;
            }
            pcm_buffer = new_buffer;
            pcm_capacity_bytes = new_size;
            continue;
        }

        if (dec_ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (raw.consumed > 0) {
                read_ptr += raw.consumed;
                bytes_left -= raw.consumed;
            }
            continue;
        }

        if (dec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AAC decode failed: %d", dec_ret);
            if (raw.consumed > 0) {
                read_ptr += raw.consumed;
                bytes_left -= raw.consumed;
            } else if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            }
            continue;
        }

        if (raw.consumed > 0) {
            read_ptr += raw.consumed;
            bytes_left -= raw.consumed;
        }

        if (!aac_info_ready_) {
            if (esp_audio_dec_get_info(aac_decoder_, &aac_stream_info_) == ESP_AUDIO_ERR_OK) {
                aac_info_ready_ = true;
                ESP_LOGI(TAG, "AAC stream: sample_rate=%u, channels=%u", aac_stream_info_.sample_rate, aac_stream_info_.channel);
            }
        } else {
            esp_audio_dec_get_info(aac_decoder_, &aac_stream_info_);
        }

        if (out_frame.decoded_size == 0) {
            continue;
        }

        uint8_t channels = aac_stream_info_.channel ? aac_stream_info_.channel : 1;
        uint32_t sample_rate = aac_stream_info_.sample_rate ? aac_stream_info_.sample_rate : 44100;
        int total_samples = out_frame.decoded_size / sizeof(int16_t);
        int16_t* final_pcm_data = pcm_buffer;

        if (channels > 1) {
            int mono_samples = total_samples / channels;
            mono_buffer.resize(mono_samples);
            for (int i = 0; i < mono_samples; ++i) {
                int32_t mixed = 0;
                for (int ch = 0; ch < channels; ++ch) {
                    mixed += pcm_buffer[i * channels + ch];
                }
                mono_buffer[i] = static_cast<int16_t>(mixed / channels);
            }
            final_pcm_data = mono_buffer.data();
            total_samples = mono_buffer.size();
        }

        if (accum_sample_rate == 0) {
            accum_sample_rate = sample_rate;
        }
        if (accum_sample_rate != static_cast<int>(sample_rate)) {
            accum_sample_rate = sample_rate;
        }

        pcm_accum.insert(pcm_accum.end(), final_pcm_data, final_pcm_data + total_samples);

        int frame_duration_ms = sample_rate > 0 ? (total_samples * 1000) / sample_rate : 0;
        current_play_time_ms_ += frame_duration_ms;
        total_frames_decoded_++;

        int buffer_latency_ms = 600;
        UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);

        int threshold_samples = accum_sample_rate > 0 ? (accum_sample_rate * 7 / 100) : 3087;
        if ((int)pcm_accum.size() >= threshold_samples) {
            // 🔊 Resample PCM data then output to codec using linear resampling
            if (accum_sample_rate != resampler_output_rate) {
                int output_samples = get_resampled_samples(pcm_accum.size(), accum_sample_rate, resampler_output_rate);
                resample_buffer.resize(output_samples);
                linear_resample(pcm_accum.data(), pcm_accum.size(), 
                               resample_buffer.data(), output_samples,
                               accum_sample_rate, resampler_output_rate);
                ESP_LOGD(TAG, "AAC Resampled: %d Hz (%d samples) -> %d Hz (%d samples)", 
                        accum_sample_rate, (int)pcm_accum.size(), 
                        resampler_output_rate, output_samples);
                codec->OutputData(resample_buffer);
                total_played += resample_buffer.size() * sizeof(int16_t);
            } else {
                codec->OutputData(pcm_accum);
                total_played += pcm_accum.size() * sizeof(int16_t);
            }
            pcm_accum.clear();
            
            // 🔄 Yield CPU to prevent watchdog timeout
            vTaskDelay(1);
        }
    }

    // Output remaining PCM if any
    if (!pcm_accum.empty()) {
        // Resample remaining PCM using linear resampling
        if (accum_sample_rate != resampler_output_rate && accum_sample_rate > 0) {
            int output_samples = get_resampled_samples(pcm_accum.size(), accum_sample_rate, resampler_output_rate);
            resample_buffer.resize(output_samples);
            linear_resample(pcm_accum.data(), pcm_accum.size(), 
                           resample_buffer.data(), output_samples,
                           accum_sample_rate, resampler_output_rate);
            codec->OutputData(resample_buffer);
            total_played += resample_buffer.size() * sizeof(int16_t);
        } else {
            codec->OutputData(pcm_accum);
            total_played += pcm_accum.size() * sizeof(int16_t);
        }
        pcm_accum.clear();
    }

    if (input_buffer) {
        heap_caps_free(input_buffer);
    }
    if (pcm_buffer) {
        heap_caps_free(pcm_buffer);
    }

    FinishPlaybackCleanup(total_played);
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    // The current AudioCodec does not expose original_output_sample_rate() or SetOutputSampleRate().
    // Keep device output sample rate unchanged and rely on AudioService resampler when needed.
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        ESP_LOGD(TAG, "Keep codec output sample rate: %d Hz", codec->output_sample_rate());
    }
}

// 监控PSRAM和SRAM使用情况
void Esp32Music::MonitorPsramUsage() {
    // PSRAM monitoring
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t used_psram = total_psram > 0 ? (total_psram - free_psram) : 0;
    
    // SRAM monitoring (internal RAM)
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_sram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t used_sram = total_sram > 0 ? (total_sram - free_sram) : 0;
    
    ESP_LOGI(TAG, "PSRAM: %d/%d KB (%.1f%%), SRAM: %d/%d KB (%.1f%%)", 
            (int)(used_psram / 1024), (int)(total_psram / 1024), 
            total_psram > 0 ? (float)used_psram * 100.0f / total_psram : 0.0f,
            (int)(used_sram / 1024), (int)(total_sram / 1024),
            total_sram > 0 ? (float)used_sram * 100.0f / total_sram : 0.0f);
    
    // PSRAM cảnh báo nếu >80%
    if (used_psram > total_psram * 0.8) {
        ESP_LOGW(TAG, "PSRAM usage high: %.1f%% - consider stopping playback", 
                (float)used_psram * 100.0f / total_psram);
    }
    
    // SRAM cảnh báo nếu >90%
    if (used_sram > total_sram * 0.9) {
        ESP_LOGW(TAG, "SRAM usage high: %.1f%% - critical", 
                (float)used_sram * 100.0f / total_sram);
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    return total_skip;
}

Esp32Music::AudioStreamFormat Esp32Music::DetermineStreamFormat(const uint8_t* data, size_t size) const {
    if (data == nullptr || size < 2) {
        return AudioStreamFormat::Unknown;
    }

    if (size >= 3 && memcmp(data, "ID3", 3) == 0) {
        return AudioStreamFormat::MP3;
    }

    if (IsLikelyAacAdts(data, size)) {
        return AudioStreamFormat::AAC_ADTS;
    }

    if (IsLikelyMp3Frame(data, size)) {
        return AudioStreamFormat::MP3;
    }

    return AudioStreamFormat::Unknown;
}

bool Esp32Music::IsLikelyMp3Frame(const uint8_t* data, size_t size) const {
    if (data == nullptr || size < 4) {
        return false;
    }

    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        return false;
    }

    uint8_t layer = (data[1] >> 1) & 0x03;
    if (layer == 0x00 || layer == 0x03) {
        return false;
    }

    uint8_t bitrate_index = (data[2] >> 4) & 0x0F;
    if (bitrate_index == 0x0F || bitrate_index == 0x00) {
        return false;
    }

    uint8_t sampling_rate_index = (data[2] >> 2) & 0x03;
    if (sampling_rate_index == 0x03) {
        return false;
    }

    return true;
}

bool Esp32Music::IsLikelyAacAdts(const uint8_t* data, size_t size) const {
    if (data == nullptr || size < 7) {
        return false;
    }

    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return false;
    }

    uint8_t layer = (data[1] >> 1) & 0x03;
    if (layer != 0x00) {
        return false;
    }

    uint16_t frame_length = ((static_cast<uint16_t>(data[3] & 0x03) << 11) |
                             (static_cast<uint16_t>(data[4]) << 3) |
                             ((data[5] & 0xE0) >> 5));

    if (frame_length < 7) {
        return false;
    }

    return true;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    // 检查URL是否为空
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // 最多允许5次重定向
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }
        
        // 设置超时和基本请求头
        http->SetTimeout(60000);  // 60秒超时
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        
        // 添加ESP32认证头
        add_auth_headers(http.get());
        
        // 打开GET连接
        ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }
        
        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);
        
        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");
        
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0) {  // Giống repo gốc
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            } else if (bytes_read == 0) {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
    }

    http->Close();

        if (read_error) {
            retry_count++;
            continue;
        }
        
        // 如果成功读取数据，跳出重试循环
        if (success) {
            break;
        }
    }
    
    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }
    
    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }

    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    // Shrink to fit để giải phóng memory không dùng - giảm SRAM usage
    lyrics_.shrink_to_fit();
    
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    // Spectrum mode đã bị xóa, luôn hiển thị lyrics
    
    // 节流：避免更新太频繁，至少间隔200ms
    if (current_time_ms - last_display_update_time_ms_ < 200) {
        return;
    }
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (!display) {
        return;
    }
    
    // 构建歌曲名称显示文本
    std::string song_title_display;
    if (!current_song_name_.empty()) {
        song_title_display = "Đang phát 《" + current_song_name_ + "》...";
    }
    
    std::string lyric_text;
    
    // 如果有歌词，查找当前应该显示的歌词
    if (!lyrics_.empty()) {
        int new_lyric_index = -1;
        
        // 从当前歌词索引开始查找，提高效率
        int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
        
        // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
        for (int i = start_index; i < (int)lyrics_.size(); i++) {
            if (lyrics_[i].first <= current_time_ms) {
                new_lyric_index = i;
            } else {
                break;  // 时间戳已超过当前时间
            }
        }
        
        // 如果歌词索引发生变化，更新显示
        if (new_lyric_index != current_lyric_index_) {
            current_lyric_index_ = new_lyric_index;
        }
        
        // 获取当前歌词文本
        if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
            lyric_text = lyrics_[current_lyric_index_].second;
        }
    }
    
    // 只更新显示当内容发生变化
    auto& app_sched = Application::GetInstance();
    
    // 更新歌曲名称（仅在首次或名称改变时）
    if (!song_title_display.empty() && song_title_display != last_displayed_song_title_) {
        last_displayed_song_title_ = song_title_display;
        app_sched.Schedule([song_title_display]() {
            auto disp = Board::GetInstance().GetDisplay();
            if (disp) {
                disp->SetMusicInfo(song_title_display.c_str());
            }
        });
    }
    
    // 更新歌词（仅在歌词改变时）
    if (lyric_text != last_displayed_lyric_text_) {
        last_displayed_lyric_text_ = lyric_text;
        if (!lyric_text.empty()) {
            app_sched.Schedule([lyric_text]() {
                auto disp = Board::GetInstance().GetDisplay();
                if (disp) {
                    disp->SetChatMessage("lyric", lyric_text.c_str());
                }
            });
            
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
                    current_time_ms, lyric_text.c_str());
        } else {
            // 如果歌词为空，也更新（清除显示）
            app_sched.Schedule([]() {
                auto disp = Board::GetInstance().GetDisplay();
                if (disp) {
                    disp->SetChatMessage("lyric", "");
                }
            });
        }
    }
    
    last_display_update_time_ms_ = current_time_ms;
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 * 
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    // Chỉ hỗ trợ LYRICS mode, spectrum đã bị xóa để giải phóng SRAM
    if (mode == DISPLAY_MODE_SPECTRUM) {
        ESP_LOGW(TAG, "SPECTRUM mode is disabled to save SRAM, using LYRICS mode instead");
        mode = DISPLAY_MODE_LYRICS;
    }
    display_mode_.store(mode, std::memory_order_relaxed);
    
    ESP_LOGI(TAG, "Display mode: LYRICS (SPECTRUM disabled to save SRAM)");
}

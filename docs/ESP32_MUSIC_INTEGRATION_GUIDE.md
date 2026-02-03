# 🎵 ESP32 Music Streaming Integration Guide

Hướng dẫn tích hợp module ESP32 Music Streaming vào project khác.

---

## 📋 MỤC LỤC

1. [Files Cần Copy](#1-files-cần-copy)
2. [Dependencies](#2-dependencies)
3. [Stub Implementation](#3-stub-implementation-tối-thiểu)
4. [CMakeLists Configuration](#4-cmakelists-configuration)
5. [Khởi Tạo và Sử Dụng](#5-khởi-tạo-và-sử-dụng)
6. [Complete Example](#6-complete-example-project)

---

## 1. FILES CẦN COPY

### Cấu trúc thư mục:
```
your_project/
├── main/
│   ├── music/
│   │   ├── music.h              ← Base interface
│   │   ├── esp32_music.h        ← Main header
│   │   ├── esp32_music.cc       ← Implementation (2223 lines)
│   │   └── stubs.h              ← Stub implementations (create new)
│   └── CMakeLists.txt
└── CMakeLists.txt
```

### Files gốc cần copy:
- `main/boards/common/music.h`
- `main/boards/common/esp32_music.h`
- `main/boards/common/esp32_music.cc`

---

## 2. DEPENDENCIES

### A. ESP-IDF Components (Có sẵn)
```cmake
REQUIRES:
- esp_http_client
- mbedtls
- json
- nvs_flash
- esp_timer
- freertos
```

### B. Third-party Libraries

#### MP3 Decoder (minimp3):
```c
// Thêm vào components/minimp3/mp3dec.h
#ifndef MP3DEC_H
#define MP3DEC_H

typedef void* HMP3Decoder;

HMP3Decoder MP3InitDecoder(void);
void MP3FreeDecoder(HMP3Decoder hMP3Decoder);
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, 
              int *bytesLeft, short *outbuf, int useSize);

typedef struct {
    int bitrate;
    int nChans;
    int samprate;
    int outputSamps;
} MP3FrameInfo;

void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo);

#endif
```

#### AAC Decoder (ESP Audio):
Download từ ESP Component Registry:
```bash
idf.py add-dependency "espressif/esp_audio_codec^1.0.0"
```

### C. Custom Classes Cần Implement

---

## 3. STUB IMPLEMENTATION TỐI THIỂU

Tạo file `main/music/stubs.h`:

```cpp
#ifndef MUSIC_STUBS_H
#define MUSIC_STUBS_H

#include <string>
#include <memory>
#include <functional>
#include <esp_log.h>
#include <esp_http_client.h>
#include <driver/i2s.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_mac.h>

// ============= HTTP CLIENT WRAPPER =============
class Http {
private:
    esp_http_client_handle_t client_;
    std::string url_;
    std::string response_body_;
    
public:
    Http() : client_(nullptr) {}
    
    ~Http() {
        Close();
    }
    
    bool Open(const char* method, const std::string& url) {
        url_ = url;
        
        esp_http_client_config_t config = {};
        config.url = url_.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 5000;
        config.buffer_size = 4096;
        
        client_ = esp_http_client_init(&config);
        if (!client_) return false;
        
        esp_err_t err = esp_http_client_open(client_, 0);
        return (err == ESP_OK);
    }
    
    void SetHeader(const std::string& key, const std::string& value) {
        if (client_) {
            esp_http_client_set_header(client_, key.c_str(), value.c_str());
        }
    }
    
    int Read(void* buffer, size_t size) {
        if (!client_) return -1;
        return esp_http_client_read(client_, (char*)buffer, size);
    }
    
    std::string ReadAll() {
        if (!client_) return "";
        
        response_body_.clear();
        char buffer[1024];
        int read_len;
        
        while ((read_len = esp_http_client_read(client_, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[read_len] = '\0';
            response_body_ += buffer;
        }
        
        return response_body_;
    }
    
    void Close() {
        if (client_) {
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }
    }
    
    int GetStatusCode() {
        return client_ ? esp_http_client_get_status_code(client_) : 0;
    }
    
    int64_t GetContentLength() {
        return client_ ? esp_http_client_get_content_length(client_) : 0;
    }
    
    std::string GetHeader(const std::string& key) {
        if (!client_) return "";
        char* value = nullptr;
        if (esp_http_client_get_header(client_, key.c_str(), &value) == ESP_OK && value) {
            return std::string(value);
        }
        return "";
    }
};

// ============= NETWORK INTERFACE =============
class Network {
public:
    std::unique_ptr<Http> CreateHttp(int timeout_ms = 0) {
        return std::make_unique<Http>();
    }
};

// ============= AUDIO CODEC (I2S) =============
class AudioCodec {
private:
    i2s_port_t i2s_num_;
    int sample_rate_;
    bool output_enabled_;
    
public:
    AudioCodec(i2s_port_t num = I2S_NUM_0) : i2s_num_(num), sample_rate_(16000), output_enabled_(true) {
        InitI2S();
    }
    
    void InitI2S() {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = (uint32_t)sample_rate_,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8,
            .dma_buf_len = 512,
            .use_apll = false,
            .tx_desc_auto_clear = true,
            .fixed_mclk = 0
        };
        
        i2s_driver_install(i2s_num_, &i2s_config, 0, NULL);
        
        // Pin configuration (adjust for your board)
        i2s_pin_config_t pin_config = {
            .bck_io_num = GPIO_NUM_26,
            .ws_io_num = GPIO_NUM_25,
            .data_out_num = GPIO_NUM_22,
            .data_in_num = I2S_PIN_NO_CHANGE
        };
        i2s_set_pin(i2s_num_, &pin_config);
    }
    
    bool OutputI2sWrite(void* data, size_t size, size_t* written) {
        return i2s_write(i2s_num_, data, size, written, portMAX_DELAY) == ESP_OK;
    }
    
    int output_sample_rate() const {
        return sample_rate_;
    }
    
    void set_sample_rate(int rate) {
        sample_rate_ = rate;
        i2s_set_sample_rates(i2s_num_, rate);
    }
    
    bool output_enabled() const {
        return output_enabled_;
    }
    
    void EnableOutput(bool enable) {
        output_enabled_ = enable;
    }
};

// ============= DISPLAY (STUB) =============
class Display {
public:
    void SetChatMessage(const std::string& role, const std::string& message) {
        ESP_LOGI("Display", "[%s] %s", role.c_str(), message.c_str());
    }
    
    void ShowSongInfo(const char* title, const char* artist) {
        ESP_LOGI("Display", "♪ %s - %s", title, artist);
    }
};

// ============= SETTINGS (NVS WRAPPER) =============
class Settings {
private:
    nvs_handle_t handle_;
    bool readonly_;
    
public:
    Settings(const char* namespace_name, bool readonly) : handle_(0), readonly_(readonly) {
        nvs_open_mode_t mode = readonly ? NVS_READONLY : NVS_READWRITE;
        nvs_open(namespace_name, mode, &handle_);
    }
    
    ~Settings() {
        if (handle_) {
            nvs_close(handle_);
        }
    }
    
    std::string GetString(const std::string& key, const std::string& default_value) {
        size_t required_size = 0;
        esp_err_t err = nvs_get_str(handle_, key.c_str(), NULL, &required_size);
        
        if (err == ESP_OK && required_size > 0) {
            char* value = new char[required_size];
            nvs_get_str(handle_, key.c_str(), value, &required_size);
            std::string result(value);
            delete[] value;
            return result;
        }
        
        return default_value;
    }
    
    bool SetString(const char* key, const char* value) {
        if (readonly_) return false;
        esp_err_t err = nvs_set_str(handle_, key, value);
        if (err == ESP_OK) {
            nvs_commit(handle_);
            return true;
        }
        return false;
    }
};

// ============= PROTOCOL (STUB) =============
class Protocol {
public:
    void SendMcpNotification(const std::string& event, const std::string& data) {
        ESP_LOGI("Protocol", "MCP Notification: %s - %s", event.c_str(), data.c_str());
    }
};

// ============= APPLICATION (STUB) =============
class Application {
private:
    static Application* instance_;
    bool audio_stop_requested_;
    bool media_low_sram_mode_;
    Protocol* protocol_;
    
    Application() : audio_stop_requested_(false), media_low_sram_mode_(false), protocol_(nullptr) {
        protocol_ = new Protocol();
    }
    
public:
    static Application& GetInstance() {
        if (!instance_) {
            instance_ = new Application();
        }
        return *instance_;
    }
    
    bool IsAudioStopRequested() const {
        return audio_stop_requested_;
    }
    
    void RequestAudioStop() {
        audio_stop_requested_ = true;
    }
    
    void ClearAudioStopRequest() {
        audio_stop_requested_ = false;
    }
    
    void Schedule(std::function<void()> callback) {
        // Execute immediately for stub
        if (callback) {
            callback();
        }
    }
    
    void SetMediaLowSramMode(bool enable) {
        media_low_sram_mode_ = enable;
    }
    
    bool IsMediaLowSramMode() const {
        return media_low_sram_mode_;
    }
    
    Protocol* GetProtocol() {
        return protocol_;
    }
};

// Static member definition - add this in ONE .cpp file
// Application* Application::instance_ = nullptr;

// ============= BOARD SINGLETON =============
class Board {
private:
    static Board* instance_;
    AudioCodec* codec_;
    Display* display_;
    Network* network_;
    
    Board() {
        codec_ = new AudioCodec();
        display_ = new Display();
        network_ = new Network();
    }
    
public:
    static Board& GetInstance() {
        if (!instance_) {
            instance_ = new Board();
        }
        return *instance_;
    }
    
    AudioCodec* GetAudioCodec() {
        return codec_;
    }
    
    Display* GetDisplay() {
        return display_;
    }
    
    Network* GetNetwork() {
        return network_;
    }
    
    void SetPowerSaveMode(bool enable) {
        ESP_LOGI("Board", "Power save mode: %s", enable ? "ON" : "OFF");
    }
};

// Static member definition - add this in ONE .cpp file
// Board* Board::instance_ = nullptr;

// ============= SYSTEM INFO (STUB) =============
class SystemInfo {
public:
    static std::string GetMacAddress() {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(mac_str);
    }
};

#endif // MUSIC_STUBS_H
```

---

## 4. CMAKELISTS CONFIGURATION

### `main/CMakeLists.txt`:
```cmake
set(SRCS 
    "main.cpp"
    "music/esp32_music.cc"
)

set(INCLUDE_DIRS 
    "."
    "music"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES 
        esp_http_client
        mbedtls
        json
        nvs_flash
        driver
    PRIV_REQUIRES
        esp_timer
)

# Link minimp3 library (if available)
# target_link_libraries(${COMPONENT_LIB} PRIVATE minimp3)
```

---

## 5. KHỞI TẠO VÀ SỬ DỤNG

### `main/main.cpp`:
```cpp
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "protocol_examples_common.h"

// Include stubs and music
#include "music/stubs.h"
#include "music/esp32_music.h"

// Define static instances
Application* Application::instance_ = nullptr;
Board* Board::instance_ = nullptr;

#define TAG "MAIN"

extern "C" void app_main(void) {
    // 1. Initialize NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // 2. Initialize network
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect()); // Or your WiFi init
    
    // 3. Configure music server URL
    Settings settings("wifi", false);
    settings.SetString("music_srv", "http://192.168.1.100:5005");
    
    // 4. Create music player
    Esp32Music* music = new Esp32Music();
    
    // 5. Test streaming
    ESP_LOGI(TAG, "Starting music stream...");
    std::string url = "http://192.168.1.100:8080/test.mp3";
    
    if (music->StartStreaming(url)) {
        ESP_LOGI(TAG, "✅ Music streaming started!");
        
        // Wait while playing
        while (music->IsPlaying()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "Playing... (buffer: %zu bytes)", 
                     music->GetBufferSize());
        }
        
        ESP_LOGI(TAG, "Music finished");
    } else {
        ESP_LOGE(TAG, "❌ Failed to start music stream");
    }
    
    // 6. Test download by song name
    ESP_LOGI(TAG, "\nTesting song download...");
    if (music->Download("Shape of You", "Ed Sheeran")) {
        ESP_LOGI(TAG, "✅ Song downloaded and playing");
    }
    
    // 7. Stop music
    vTaskDelay(pdMS_TO_TICKS(10000)); // Play for 10 seconds
    music->StopStreaming();
    ESP_LOGI(TAG, "Music stopped");
    
    // Cleanup
    delete music;
}
```

---

## 6. COMPLETE EXAMPLE PROJECT

### Cấu trúc hoàn chỉnh:
```
esp32_music_example/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── Kconfig.projbuild
│   └── music/
│       ├── music.h              (copy from original)
│       ├── esp32_music.h        (copy from original)
│       ├── esp32_music.cc       (copy from original)
│       └── stubs.h              (create as above)
└── components/
    └── minimp3/               (optional, for MP3 support)
        ├── CMakeLists.txt
        ├── mp3dec.h
        └── mp3dec.c
```

### Root `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_music_example)
```

### `main/Kconfig.projbuild`:
```kconfig
menu "Music Player Configuration"

config MUSIC_SERVER_URL
    string "Default Music Server URL"
    default "http://192.168.1.100:5005"
    help
        Default music streaming server URL

config I2S_BCK_PIN
    int "I2S BCK Pin"
    default 26
    
config I2S_WS_PIN
    int "I2S WS Pin"
    default 25
    
config I2S_DATA_PIN
    int "I2S DATA Pin"
    default 22

endmenu
```

---

## 📊 MEMORY REQUIREMENTS

```
Flash: ~200KB (code + libs)
SRAM: ~50-80KB runtime
  - Audio buffer: 32KB
  - HTTP buffer: 8KB
  - Decoder working: 10-20KB
  - Threads stack: 10KB

PSRAM: Optional but recommended
  - Can increase buffer sizes
  - Better for long streaming
```

---

## 🔧 CUSTOMIZATION

### Thay đổi buffer size:
```cpp
// In esp32_music.h
static constexpr size_t MAX_BUFFER_SIZE = 64 * 1024;  // Adjust this
static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024;  // Minimum for smooth playback
```

### Thêm các codec khác:
```cpp
// Add FLAC, WAV, etc.
enum class AudioStreamFormat {
    Unknown = 0,
    MP3,
    AAC_ADTS,
    FLAC,      // Add new
    WAV        // Add new
};
```

### Custom display updates:
```cpp
// Override in stubs.h Display class
void ShowSongInfo(const char* title, const char* artist) {
    // Your LCD/OLED display code here
    lcd_printf("Now playing: %s", title);
}
```

---

## ⚠️ TROUBLESHOOTING

### Error: `undefined reference to MP3InitDecoder`
→ Add minimp3 library to components or use esp-libhelix-mp3

### Error: `HTTP client timeout`
→ Check network connection and server URL

### Error: `I2S write failed`
→ Verify I2S pin configuration in stubs.h

### Music choppy/stuttering
→ Increase buffer size or DMA buffer count

### Error: `esp_audio_dec_open failed`
→ Ensure esp_audio_codec component is added correctly

### Out of memory (heap)
→ Enable PSRAM support in menuconfig, or reduce buffer sizes

---

## 📚 API REFERENCE

### Esp32Music Methods:

```cpp
// Constructor/Destructor
Esp32Music();
~Esp32Music();

// Streaming control
bool StartStreaming(const std::string& url);  // Start streaming from URL
bool StopStreaming();                          // Stop current stream

// Download and play by song name
bool Download(const std::string& song_name, const std::string& artist_name);
std::string GetDownloadResult();               // Get raw API response

// Status queries
bool IsPlaying() const;                        // Check if currently playing
bool IsDownloading() const;                    // Check if downloading
size_t GetBufferSize() const;                  // Get current buffer size
int16_t* GetAudioData();                       // Get raw audio data (returns nullptr)

// Display control
void SetDisplayMode(DisplayMode mode);         // DISPLAY_MODE_LYRICS only
DisplayMode GetDisplayMode() const;            // Get current display mode
void SetExternalSongTitle(const std::string& title);  // Set song title for display
```

### DisplayMode enum:
```cpp
enum DisplayMode {
    DISPLAY_MODE_SPECTRUM = 0,  // Disabled (SRAM optimization)
    DISPLAY_MODE_LYRICS = 1     // Default: show lyrics/messages
};
```

---

## 🎯 FEATURES

- ✅ MP3 streaming playback
- ✅ AAC/ADTS streaming playback
- ✅ HTTP streaming with buffering
- ✅ Lyrics download and display (LRC format)
- ✅ Song search by name/artist
- ✅ Memory-optimized for ESP32 (PSRAM support)
- ✅ Thread-safe design
- ✅ Graceful stop handling
- ✅ ID3 tag skipping

---

## 📞 SUPPORT

- QQ群: 826072986
- Website: kytuoi.com
- Phone: 0345995569

---

**Created: 2026-01-31**  
**Version: 1.0**  
**Based on: kikichatwiath_ai-master**

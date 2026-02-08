#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define PREVIEW_IMAGE_DURATION_MS 5000

// FFT Configuration
#define LCD_FFT_SIZE 256
#define BAR_COL_NUM 16


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    void InitializeLcdThemes();
    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    
    // FFT processing methods
    void processAudioData();
    void periodicUpdateTask();
    static void periodicUpdateTaskWrapper(void* arg);
    void compute(float* real, float* imag, int n, bool forward);
    void drawSpectrum();
    void draw_spectrum(float* power_spectrum, int fft_size);
    void draw_bar(int x, int y, int bar_width, int bar_height, uint16_t color, int bar_index);
    void create_fft_canvas();
    
    // FFT buffers (allocated in PSRAM)
    int16_t* final_pcm_data_fft_ = nullptr;
    int16_t* audio_data_ = nullptr;
    int16_t* frame_audio_data_ = nullptr;
    float* fft_real_ = nullptr;
    float* fft_imag_ = nullptr;
    float* hanning_window_ = nullptr;
    float avg_power_spectrum_[LCD_FFT_SIZE / 2] = {0};
    int current_heights_[BAR_COL_NUM] = {0};
    
    // FFT state
    int audio_display_last_update_ = 0;
    bool fft_data_ready_ = false;
    std::atomic<bool> fft_task_should_stop_{false};
    TaskHandle_t fft_task_handle_ = nullptr;
    
    // FFT canvas
    lv_obj_t* fft_canvas_ = nullptr;
    uint16_t* fft_canvas_buffer_ = nullptr;
    int fft_canvas_width_ = 0;
    int fft_canvas_height_ = 0;
    int bar_max_height_ = 0;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
    
    // FFT Spectrum methods
    virtual void StartFFT() override;
    virtual void StopFFT() override;
    virtual void FeedAudioDataFFT(int16_t* data, size_t sample_count) override;
    virtual int16_t* MakeAudioBuffFFT(size_t sample_count) override;
    virtual void ReleaseAudioBuffFFT(int16_t* buffer = nullptr) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H

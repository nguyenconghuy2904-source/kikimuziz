#include "otto_emoji_display.h"
#include "otto_gif_data.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <font_awesome.h>
#include <cbin_font.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <memory>

// QR Code generator library
extern "C" {
#include "qrcode/qrcodegen.h"
}

// External font declaration (PuHui supports Vietnamese)
extern "C" {
    LV_FONT_DECLARE(font_puhui_16_4);
}

#include "display/lcd_display.h"
#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "otto_webserver.h"

#define TAG "OttoEmojiDisplay"

// 表情映射表 - 将原版21种表情映射到现有6个GIF
const OttoEmojiDisplay::EmotionMap OttoEmojiDisplay::emotion_maps_[] = {
    // 中性/平静类表情 -> staticstate
    {"neutral", &staticstate},
    {"relaxed", &staticstate},
    {"sleepy", &staticstate},

    // 积极/开心类表情 -> happy
    {"happy", &happy},
    {"laughing", &happy},
    {"funny", &happy},
    {"loving", &happy},
    {"confident", &happy},
    {"winking", &happy},
    {"cool", &happy},
    {"delicious", &happy},
    {"kissy", &happy},
    {"silly", &happy},

    // 悲伤类表情 -> sad
    {"sad", &sad},
    {"crying", &sad},

    // 愤怒类表情 -> anger
    {"angry", &anger},

    // 惊讶类表情 -> scare
    {"surprised", &scare},
    {"shocked", &scare},

    // 思考/困惑类表情 -> buxue
    {"thinking", &buxue},
    {"confused", &buxue},
    {"embarrassed", &buxue},

    {nullptr, nullptr}  // 结束标记
};

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
      emotion_gif_(nullptr), 
      use_otto_emoji_(true),  // Start with Otto GIF mode by default
      drawing_canvas_(nullptr),
      drawing_canvas_buf_(nullptr),
      drawing_canvas_enabled_(false),
      display_on_(true),      // Display starts on
      auto_off_enabled_(true), // Auto-off enabled by default
      auto_off_timer_(nullptr),
      emoji_overlay_mode_(false),
      qr_canvas_(nullptr),
      qr_canvas_buf_(nullptr),
      qr_timer_(nullptr),
      qr_displaying_(false),  // QR code not displayed initially
      clock_container_(nullptr),
      clock_time_label_(nullptr),
      clock_date_label_(nullptr),
      clock_hour_label_(nullptr),
      clock_min_label_(nullptr),
      clock_arc_red_(nullptr),
      clock_arc_green_(nullptr),
      clock_arc_blue_(nullptr),
      clock_timer_(nullptr),
      clock_update_timer_(nullptr),
      clock_displaying_(false),
      idle_clock_enabled_(false) { // Clock not displayed initially, idle clock disabled
    
    // Create auto-off timer (5 minutes = 300000 ms)
    const esp_timer_create_args_t timer_args = {
        .callback = &OttoEmojiDisplay::AutoOffTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "display_auto_off",
        .skip_unhandled_events = false
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &auto_off_timer_);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Auto-off timer created (1 hour idle timeout)");
        // Start timer immediately
        ResetAutoOffTimer();
    } else {
        ESP_LOGE(TAG, "❌ Failed to create auto-off timer: %s", esp_err_to_name(err));
    }
    
    SetupGifContainer();
};

OttoEmojiDisplay::~OttoEmojiDisplay() {
    // Cleanup timers to prevent memory leaks
    if (auto_off_timer_ != nullptr) {
        esp_timer_stop(auto_off_timer_);
        esp_timer_delete(auto_off_timer_);
        auto_off_timer_ = nullptr;
    }
    if (qr_timer_ != nullptr) {
        esp_timer_stop(qr_timer_);
        esp_timer_delete(qr_timer_);
        qr_timer_ = nullptr;
    }
    if (clock_timer_ != nullptr) {
        esp_timer_stop(clock_timer_);
        esp_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }
    if (clock_update_timer_ != nullptr) {
        esp_timer_stop(clock_update_timer_);
        esp_timer_delete(clock_update_timer_);
        clock_update_timer_ = nullptr;
    }
    
    // Cleanup canvas buffers
    CleanupDrawingCanvas();
    if (qr_canvas_buf_ != nullptr) {
        free(qr_canvas_buf_);
        qr_canvas_buf_ = nullptr;
    }
    
    ESP_LOGI(TAG, "🧹 OttoEmojiDisplay resources cleaned up");
}

void OttoEmojiDisplay::SetupGifContainer() {
    DisplayLockGuard lock(this);

    if (emoji_label_) {
        lv_obj_del(emoji_label_);
        emoji_label_ = nullptr;
    }

    if (emoji_image_) {
        lv_obj_del(emoji_image_);
        emoji_image_ = nullptr;
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
        chat_message_label_ = nullptr;
    }

    if (preview_image_) {
        lv_obj_del(preview_image_);
        preview_image_ = nullptr;
    }

    if (emoji_box_) {
        lv_obj_del(emoji_box_);
        emoji_box_ = nullptr;
    }

    if (content_) {
        lv_obj_del(content_);
        content_ = nullptr;
    }

    emotion_gif_ = nullptr;

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emoji_box_ = lv_obj_create(content_);
    lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_flex_flow(emoji_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(emoji_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(emoji_box_);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_set_style_border_width(emoji_label_, 0, 0);
    lv_obj_center(emoji_label_);
    
    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    emotion_gif_ = lv_img_create(emoji_box_);
    int gif_size = LV_HOR_RES;
    lv_obj_set_size(emotion_gif_, gif_size, gif_size);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_gif_, LV_OPA_TRANSP, 0);
    
    // PERFORMANCE OPTIMIZATIONS for GIF rendering
    lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_ADV_HITTEST);  // Faster hit testing
    lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_SCROLLABLE); // Disable scrolling
    lv_obj_set_style_radius(emotion_gif_, 0, 0);             // No rounded corners
    lv_obj_set_style_shadow_width(emotion_gif_, 0, 0);       // No shadows
    
    lv_obj_center(emotion_gif_);
    
    // Set visibility based on initial mode
    if (use_otto_emoji_) {
        // Otto GIF mode: show GIF, hide label
        gif_controller_ = std::make_unique<LvglGif>(&happy);
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback([this]() {
                lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
            });
            lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
            gif_controller_->Start();
        }
        lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Twemoji text mode: hide GIF, show label
        gif_controller_ = std::make_unique<LvglGif>(&staticstate);
        if (gif_controller_->IsLoaded()) {
            lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
        }
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(chat_message_label_, &BUILTIN_TEXT_FONT, 0); // Font PuHui hỗ trợ tiếng Việt (16px)
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);

    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 8, 0); // Tăng padding
    lv_obj_set_style_pad_hor(chat_message_label_, 10, 0); // Thêm padding ngang

    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -20); // Lùi xuống thấp hơn: 20px từ đáy
    lv_obj_move_foreground(chat_message_label_); // Đưa lên trên cùng

    preview_image_ = lv_image_create(content_);
    lv_obj_set_size(preview_image_, LV_HOR_RES / 2, LV_VER_RES / 2);
    lv_obj_center(preview_image_);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme != nullptr) {
        LcdDisplay::SetTheme(theme);
    }
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return;
    
    // Block emotion changes while clock is displayed
    if (clock_displaying_) {
        ESP_LOGD(TAG, "⏰ Blocked emotion change - clock is displayed");
        return;
    }
    
    // Block emotion changes while QR code is displayed
    if (qr_displaying_) {
        ESP_LOGD(TAG, "🚫 Blocked emotion change - QR code is displayed");
        return;
    }
    
    // Check if we're forcing special emoji that blocks all other emoji changes
    const char* emotion_to_use = emotion;
#ifdef CONFIG_BOARD_TYPE_KIKI
    auto& app = Application::GetInstance();
    
    // Check for "shocked" emoji lock (shoot command)
    if (app.IsForcingShockedEmoji() && strcmp(emotion, "shocked") != 0) {
        ESP_LOGI(TAG, "🚫 Blocked emotion change to '%s' - keeping 'shocked' emoji for shoot command", emotion);
        emotion_to_use = "shocked";  // Force "shocked" emoji
    }
    // Check for "silly" emoji lock (celebration)
    else if (app.IsForcingSillyEmoji() && strcmp(emotion, "silly") != 0) {
        ESP_LOGI(TAG, "🚫 Blocked emotion change to '%s' - keeping 'silly' emoji for celebration", emotion);
        emotion_to_use = "silly";  // Force "silly" emoji
    }
    // Check for "delicious" emoji lock (custom keyword)
    else if (app.IsForcingDeliciousEmoji() && strcmp(emotion, "delicious") != 0) {
        ESP_LOGI(TAG, "🚫 Blocked emotion change to '%s' - keeping 'delicious' emoji for custom keyword", emotion);
        emotion_to_use = "delicious";  // Force "delicious" emoji
    }
#endif
    
    // Turn on display and reset auto-off timer on activity
    TurnOn();
    
    // For text emoji mode, directly call parent class without rate limiting
    if (!use_otto_emoji_) {
        DisplayLockGuard lock(this);
        LcdDisplay::SetEmotion(emotion_to_use);
        ESP_LOGI(TAG, "📝 Text表情: %s", emotion_to_use);
        return;
    }
    
    // Otto GIF emoji mode requires emotion_gif_
    if (!emotion_gif_) return;
    
    static std::string cached_emotion;
    static const lv_img_dsc_t* cached_gif = nullptr;
    
    // Rate limiting: only change emotion every 200ms (5 FPS max)
    static uint64_t last_emotion_time = 0;
    uint64_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    if (current_time - last_emotion_time < 200) {
        return; // Skip if too frequent
    }
    
    // Check if emotion is the same to avoid unnecessary processing
    if (emotion_to_use && cached_emotion == emotion_to_use && cached_gif) {
        return; // Skip if same emotion
    }
    
    DisplayLockGuard lock(this);
    
    // Find emotion in emoji maps first (with cache check)
    if (emotion_to_use && cached_emotion == emotion_to_use && cached_gif) {
        gif_controller_ = std::make_unique<LvglGif>(cached_gif);
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback([this]() {
                lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
            });
            lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
            gif_controller_->Start();
        }
        ESP_LOGI(TAG, "🤖 Otto表情(缓存): %s", emotion);
        return;
    }
    
    // Find emotion in map
    for (const auto& map : emotion_maps_) {
        if (map.name && strcmp(map.name, emotion_to_use) == 0) {
            gif_controller_ = std::make_unique<LvglGif>(map.gif);
            if (gif_controller_->IsLoaded()) {
                gif_controller_->SetFrameCallback([this]() {
                    lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
                });
                lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
                gif_controller_->Start();
            }
            // Cache the result
            cached_emotion = map.name;
            cached_gif = map.gif;
            last_emotion_time = current_time;
            ESP_LOGI(TAG, "🤖 Otto表情: %s", emotion);
            return;
        }
    }
    
    // Default fallback
    gif_controller_ = std::make_unique<LvglGif>(&staticstate);
    if (gif_controller_->IsLoaded()) {
        lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
    }
    cached_emotion = "default";
    cached_gif = &staticstate;
    last_emotion_time = current_time;
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    // Block chat messages while clock is displaying
    if (clock_displaying_) {
        ESP_LOGI(TAG, "⏰ Chat message blocked - clock is displaying");
        return;
    }
    
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        ESP_LOGW(TAG, "❌ chat_message_label_ is NULL!");
        return;
    }

    if (content == nullptr || strlen(content) == 0) {
        ESP_LOGI(TAG, "🙈 Hiding chat message (empty content)");
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_message_label_, content);
    lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    
    // Only move chat message to foreground if NOT in emoji overlay mode
    if (!emoji_overlay_mode_) {
        lv_obj_move_foreground(chat_message_label_); // Đảm bảo luôn ở trên cùng
    }

    ESP_LOGI(TAG, "💬 Chat message [%s]: %s (overlay_mode=%d)", role ? role : "unknown", content, emoji_overlay_mode_);

    // 🔫 Keyword detection: If user says "súng nè" or "bằng bằng", trigger defend action
    if (strcmp(role, "user") == 0 && content != nullptr) {
                // Keyword detection moved to application.cc to prevent text display
        // When keywords like "bằng bằng" are detected, application.cc now:
        // 1. Shows emoji ONLY (no chat message)
        // 2. Triggers defend action sequence
        // This avoids duplicate processing here
    }
}

void OttoEmojiDisplay::UpdateStatusBar(bool update_all) {
    // SUPER aggressive status bar rate limiting to eliminate all lag
    static uint32_t last_status_update = 0;
    uint32_t now = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    // Only update status bar every 2 seconds unless forced
    if (!update_all && (now - last_status_update < 2000)) { // 0.5 FPS limit
        return;
    }
    last_status_update = now;

    // 调用父类的UpdateStatusBar但禁用低电量弹窗
    DisplayLockGuard lock(this);
    
    // 先调用父类方法更新其他状态栏信息 - but limit frequency
    static uint32_t last_parent_update = 0;
    if (update_all || (now - last_parent_update > 5000)) { // Parent update every 5 seconds max
        SpiLcdDisplay::UpdateStatusBar(update_all);
        last_parent_update = now;
    }
}

void OttoEmojiDisplay::SetEmojiMode(bool use_otto_emoji) {
    DisplayLockGuard lock(this);
    
    if (use_otto_emoji_ == use_otto_emoji) {
        return; // 没有变化，不需要处理
    }
    
    use_otto_emoji_ = use_otto_emoji;
    
    // Don't show emoji if clock is displayed
    if (clock_displaying_) {
        ESP_LOGI(TAG, "⏰ SetOttoEmojiMode: skipping emoji show - clock is displayed");
        return;
    }
    
    if (use_otto_emoji_) {
        // 切换到Otto emoji模式
        ESP_LOGI(TAG, "切换到Otto GIF表情模式");
        
        // 显示GIF容器，隐藏默认emoji
        if (emotion_gif_) {
            lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
            // CRITICAL: Re-activate the GIF by resetting the source
            gif_controller_ = std::make_unique<LvglGif>(&staticstate);
            if (gif_controller_->IsLoaded()) {
                lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
            }
            ESP_LOGI(TAG, "🔄 GIF重新激活");
        }
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        // Also make sure we set a valid Otto emotion to draw immediately
        SetEmotion("neutral");
    } else {
        // 切换到默认emoji模式 (Twemoji text)
        ESP_LOGI(TAG, "切换到Twemoji文本表情模式");
        
        // 隐藏GIF容器，显示文本emoji label
        if (emotion_gif_) {
            lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        // Set default happy Twemoji
        SetEmotion("happy");
    }
}

// UDP Drawing Canvas Implementation
void OttoEmojiDisplay::EnableDrawingCanvas(bool enable) {
    DisplayLockGuard lock(this);
    
    if (enable == drawing_canvas_enabled_) {
        return;
    }
    
    drawing_canvas_enabled_ = enable;
    
    if (enable) {
        InitializeDrawingCanvas();
        ESP_LOGI(TAG, "🎨 Drawing canvas ENABLED");
    } else {
        CleanupDrawingCanvas();
        ESP_LOGI(TAG, "🎨 Drawing canvas DISABLED");
    }
}

void OttoEmojiDisplay::InitializeDrawingCanvas() {
    CleanupDrawingCanvas();  // Clean up any existing canvas
    
    // Hide normal UI elements
    if (content_) {
        lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Allocate canvas buffer (RGB565 format)
    size_t buf_size = width_ * height_ * sizeof(lv_color_t);
    drawing_canvas_buf_ = malloc(buf_size);
    if (!drawing_canvas_buf_) {
        ESP_LOGE(TAG, "Failed to allocate drawing canvas buffer (%zu bytes)", buf_size);
        return;
    }
    memset(drawing_canvas_buf_, 0, buf_size);  // Clear to black
    
    // Create LVGL canvas
    drawing_canvas_ = lv_canvas_create(container_);
    if (!drawing_canvas_) {
        ESP_LOGE(TAG, "Failed to create LVGL canvas");
        free(drawing_canvas_buf_);
        drawing_canvas_buf_ = nullptr;
        return;
    }
    
    lv_canvas_set_buffer(drawing_canvas_, drawing_canvas_buf_, width_, height_, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(drawing_canvas_, width_, height_);
    lv_obj_set_pos(drawing_canvas_, 0, 0);
    lv_canvas_fill_bg(drawing_canvas_, lv_color_black(), LV_OPA_COVER);
    
    ESP_LOGI(TAG, "✅ Drawing canvas initialized (%dx%d)", width_, height_);
}

void OttoEmojiDisplay::CleanupDrawingCanvas() {
    if (drawing_canvas_) {
        lv_obj_del(drawing_canvas_);
        drawing_canvas_ = nullptr;
    }
    
    if (drawing_canvas_buf_) {
        free(drawing_canvas_buf_);
        drawing_canvas_buf_ = nullptr;
    }
    
    // Show normal UI elements again
    if (content_) {
        lv_obj_remove_flag(content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_) {
        lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void OttoEmojiDisplay::ClearDrawingCanvas() {
    if (!drawing_canvas_) {
        ESP_LOGW(TAG, "No drawing canvas to clear");
        return;
    }
    
    DisplayLockGuard lock(this);
    lv_canvas_fill_bg(drawing_canvas_, lv_color_black(), LV_OPA_COVER);
    ESP_LOGI(TAG, "Drawing canvas cleared");
}

void OttoEmojiDisplay::DrawPixel(int x, int y, bool state) {
    if (!drawing_canvas_) {
        return;
    }
    
    // Validate coordinates
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return;
    }
    
    DisplayLockGuard lock(this);
    lv_color_t color = state ? lv_color_white() : lv_color_black();
    lv_canvas_set_px(drawing_canvas_, x, y, color, LV_OPA_COVER);
}

void OttoEmojiDisplay::SetDrawingImage(const uint16_t* rgb565_data, int width, int height) {
    if (!rgb565_data) {
        ESP_LOGE(TAG, "NULL image data");
        return;
    }
    
    // Enable drawing canvas if not already enabled
    if (!drawing_canvas_enabled_) {
        EnableDrawingCanvas(true);
    }
    
    if (!drawing_canvas_ || !drawing_canvas_buf_) {
        ESP_LOGE(TAG, "Drawing canvas not initialized");
        return;
    }
    
    DisplayLockGuard lock(this);
    
    // Copy RGB565 data directly to canvas buffer
    // Canvas is 240x240, input may be different size - scale or crop as needed
    int copy_width = (width < width_) ? width : width_;
    int copy_height = (height < height_) ? height : height_;
    
    uint16_t* buf = (uint16_t*)drawing_canvas_buf_;
    
    // Clear buffer first
    memset(buf, 0xFF, width_ * height_ * sizeof(uint16_t));  // White background
    
    // Copy image data - center if smaller than canvas
    int offset_x = (width_ - copy_width) / 2;
    int offset_y = (height_ - copy_height) / 2;
    
    for (int y = 0; y < copy_height; y++) {
        for (int x = 0; x < copy_width; x++) {
            int src_idx = y * width + x;
            int dst_idx = (y + offset_y) * width_ + (x + offset_x);
            buf[dst_idx] = rgb565_data[src_idx];
        }
    }
    
    // Force canvas to refresh
    lv_obj_invalidate(drawing_canvas_);
    
    ESP_LOGI(TAG, "✅ Drawing image set (%dx%d)", width, height);
}

// ==================== Display Power Management ====================

void OttoEmojiDisplay::AutoOffTimerCallback(void* arg) {
    OttoEmojiDisplay* display = static_cast<OttoEmojiDisplay*>(arg);
    if (display && display->auto_off_enabled_) {
        ESP_LOGI(TAG, "⏱️ Auto-off triggered after 5 min idle");
        display->TurnOff();
    }
}

void OttoEmojiDisplay::ResetAutoOffTimer() {
    if (!auto_off_timer_ || !auto_off_enabled_) {
        return;
    }
    
    // Stop existing timer
    esp_timer_stop(auto_off_timer_);
    
    // Start timer for 1 hour (3600000000 microseconds)
    esp_err_t err = esp_timer_start_once(auto_off_timer_, 3600000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restart auto-off timer: %s", esp_err_to_name(err));
    }
}

void OttoEmojiDisplay::TurnOn() {
    if (display_on_) {
        // Already on, just reset timer
        ResetAutoOffTimer();
        return;
    }
    
    ESP_LOGI(TAG, "🔆 Turning display ON");
    
    // Turn on LCD panel
    if (panel_) {
        esp_lcd_panel_disp_on_off(panel_, true);
    }
    
    // Restore backlight brightness
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight) {
        backlight->RestoreBrightness();
        ESP_LOGI(TAG, "💡 Backlight restored");
    }
    
    display_on_ = true;
    
    // Reset auto-off timer
    ResetAutoOffTimer();
}

void OttoEmojiDisplay::TurnOff() {
    if (!display_on_) {
        return;  // Already off
    }
    
    ESP_LOGI(TAG, "🌙 Turning display OFF (idle timeout)");
    
    // Turn off LCD panel first
    if (panel_) {
        esp_lcd_panel_disp_on_off(panel_, false);
    }
    
    // Also turn off backlight for maximum power saving
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight) {
        backlight->SetBrightness(0);
        ESP_LOGI(TAG, "💡 Backlight OFF for power saving");
    }
    
    display_on_ = false;
    
    // Stop auto-off timer
    if (auto_off_timer_) {
        esp_timer_stop(auto_off_timer_);
    }
}

void OttoEmojiDisplay::SetAutoOffEnabled(bool enabled) {
    auto_off_enabled_ = enabled;
    
    if (enabled && display_on_) {
        ESP_LOGI(TAG, "✅ Auto-off enabled (1 hour idle timeout)");
        ResetAutoOffTimer();
    } else {
        ESP_LOGI(TAG, "⏸️ Auto-off disabled");
        if (auto_off_timer_) {
            esp_timer_stop(auto_off_timer_);
        }
    }
}

void OttoEmojiDisplay::SetEmojiOverlayMode(bool enable) {
    DisplayLockGuard lock(this);
    
    if (emoji_overlay_mode_ == enable) {
        return; // No change needed
    }
    
    emoji_overlay_mode_ = enable;
    
    if (enable) {
        // Move emoji_box_ to foreground (above chat_message_label_)
        if (emoji_box_) {
            lv_obj_move_foreground(emoji_box_);
            ESP_LOGI(TAG, "📱 Emoji overlay mode ENABLED - emoji now on top of chat message");
        }
    } else {
        // Restore normal order: chat message on top
        if (chat_message_label_) {
            lv_obj_move_foreground(chat_message_label_);
            ESP_LOGI(TAG, "📱 Emoji overlay mode DISABLED - chat message restored to top");
        }
    }
}

void OttoEmojiDisplay::SetChatMessageHidden(bool hidden) {
    DisplayLockGuard lock(this);
    
    if (chat_message_label_ == nullptr) {
        ESP_LOGW(TAG, "❌ chat_message_label_ is NULL!");
        return;
    }
    
    if (hidden) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "🙈 Chat message HIDDEN for QR display");
    } else {
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "👁️ Chat message SHOWN after QR display");
    }
}

// ==================== QR Code Display ====================

static void qr_timer_callback(void* arg) {
    OttoEmojiDisplay* display = static_cast<OttoEmojiDisplay*>(arg);
    if (display) {
        display->HideQRCode();
    }
}

void OttoEmojiDisplay::ShowQRCode(const char* url, int duration_ms) {
    if (!url || strlen(url) == 0) {
        ESP_LOGW(TAG, "❌ Empty URL for QR code");
        return;
    }
    
    ESP_LOGI(TAG, "📱 Generating QR code for: %s", url);
    
    // Hide existing QR first (outside lock to avoid deadlock)
    HideQRCode();
    
    // Use static buffers for QR generation to avoid stack overflow
    // Version 3 can hold up to 77 alphanumeric chars, enough for IP addresses
    static uint8_t tempBuffer[qrcodegen_BUFFER_LEN_FOR_VERSION(3)];
    static uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(3)];
    
    // Generate QR code with low error correction (smaller size)
    bool ok = qrcodegen_encodeText(url, tempBuffer, qrcode,
        qrcodegen_Ecc_LOW, 1, 3, qrcodegen_Mask_AUTO, true);
    
    if (!ok) {
        ESP_LOGE(TAG, "❌ Failed to generate QR code");
        return;
    }
    
    int qr_size = qrcodegen_getSize(qrcode);
    ESP_LOGI(TAG, "✅ QR code generated, size: %dx%d modules", qr_size, qr_size);
    
    DisplayLockGuard lock(this);
    
    // Calculate scale and position to center QR code
    // Keep canvas small: max 100x100 pixels to save RAM
    int max_canvas = 100;
    int scale = (max_canvas - 10) / qr_size;  // Leave 5px margin each side
    if (scale < 2) scale = 2;
    if (scale > 3) scale = 3;  // Limit scale to keep buffer under 20KB
    
    int qr_pixel_size = qr_size * scale;
    int canvas_size = qr_pixel_size + 10;  // 5px margin each side
    if (canvas_size > max_canvas) canvas_size = max_canvas;
    
    // Allocate canvas buffer first (RGB565 = 2 bytes per pixel)
    size_t buf_size = canvas_size * canvas_size * 2;
    ESP_LOGI(TAG, "📦 Allocating QR canvas: %dx%d, scale=%d, buf=%d bytes", 
             canvas_size, canvas_size, scale, buf_size);
    
    // Try PSRAM first (plenty of memory there)
    qr_canvas_buf_ = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!qr_canvas_buf_) {
        // Fallback to internal RAM
        qr_canvas_buf_ = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    }
    if (!qr_canvas_buf_) {
        ESP_LOGE(TAG, "❌ Failed to allocate QR canvas buffer (%d bytes)", buf_size);
        return;
    }
    
    // Set flag BEFORE hiding emoji to block any updates
    qr_displaying_ = true;
    
    // Hide ALL emoji elements and chat during QR display
    // Hide Otto GIF emoji
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    // Hide Twemoji text label
    if (emoji_label_) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    // Hide emoji image
    if (emoji_image_) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    // Hide emoji container box
    if (emoji_box_) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    // Hide chat message
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Create QR canvas
    qr_canvas_ = lv_canvas_create(container_);
    if (!qr_canvas_) {
        ESP_LOGE(TAG, "❌ Failed to create QR canvas");
        heap_caps_free(qr_canvas_buf_);
        qr_canvas_buf_ = nullptr;
        if (emotion_gif_) lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        if (chat_message_label_) lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    
    lv_canvas_set_buffer(qr_canvas_, qr_canvas_buf_, canvas_size, canvas_size, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(qr_canvas_, canvas_size, canvas_size);
    // Center the canvas
    lv_obj_set_pos(qr_canvas_, (width_ - canvas_size) / 2, (height_ - canvas_size) / 2);
    
    // Fill with white background
    lv_canvas_fill_bg(qr_canvas_, lv_color_white(), LV_OPA_COVER);
    
    // Recalculate offset for centered canvas
    int canvas_offset = (canvas_size - qr_pixel_size) / 2;
    
    // Draw QR code modules
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                // Draw black module (scaled)
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = canvas_offset + x * scale + dx;
                        int py = canvas_offset + y * scale + dy;
                        if (px >= 0 && px < canvas_size && py >= 0 && py < canvas_size) {
                            lv_canvas_set_px(qr_canvas_, px, py, lv_color_black(), LV_OPA_COVER);
                        }
                    }
                }
            }
        }
    }
    
    lv_obj_move_foreground(qr_canvas_);
    ESP_LOGI(TAG, "✅ QR code displayed (scale: %d, canvas: %dx%d, duration: %dms)", 
             scale, canvas_size, canvas_size, duration_ms);
    
    // Set timer to auto-hide
    if (duration_ms > 0) {
        if (!qr_timer_) {
            const esp_timer_create_args_t timer_args = {
                .callback = qr_timer_callback,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "qr_hide_timer",
                .skip_unhandled_events = false
            };
            esp_err_t err = esp_timer_create(&timer_args, &qr_timer_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "❌ Failed to create QR timer: %s", esp_err_to_name(err));
                return;
            }
        }
        esp_timer_stop(qr_timer_);
        esp_timer_start_once(qr_timer_, (uint64_t)duration_ms * 1000);
    }
}

void OttoEmojiDisplay::HideQRCode() {
    // Stop timer first (outside lock)
    if (qr_timer_) {
        esp_timer_stop(qr_timer_);
    }
    
    // Check if there's anything to hide
    if (!qr_displaying_ && !qr_canvas_ && !qr_canvas_buf_) {
        return;
    }
    
    DisplayLockGuard lock(this);
    
    // Delete canvas object FIRST before restoring emoji
    if (qr_canvas_) {
        lv_obj_del(qr_canvas_);
        qr_canvas_ = nullptr;
    }
    
    // Free buffer (we saved it ourselves)
    if (qr_canvas_buf_) {
        heap_caps_free(qr_canvas_buf_);
        qr_canvas_buf_ = nullptr;
    }
    
    // Restore emoji container FIRST (contains all emoji elements)
    if (emoji_box_) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Restore appropriate emoji based on current mode
    if (use_otto_emoji_) {
        // Otto GIF mode: show GIF, keep label hidden
        if (emotion_gif_) {
            lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emotion_gif_);
        }
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Twemoji text mode: show label, keep GIF hidden
        if (emoji_label_) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_gif_) {
            lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Restore chat message
    if (chat_message_label_) {
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Reset flag AFTER restoring UI to allow emotion updates again
    qr_displaying_ = false;
    
    ESP_LOGI(TAG, "🧹 QR code hidden, emoji restored");
}

// ============================================================================
// Clock Display Implementation
// ============================================================================

void OttoEmojiDisplay::ClockHideTimerCallback(void* arg) {
    OttoEmojiDisplay* display = static_cast<OttoEmojiDisplay*>(arg);
    if (display) {
        display->HideClock();
    }
}

void OttoEmojiDisplay::ClockUpdateTimerCallback(void* arg) {
    OttoEmojiDisplay* display = static_cast<OttoEmojiDisplay*>(arg);
    if (display && display->clock_displaying_) {
        display->UpdateClockDisplay();
    }
}

void OttoEmojiDisplay::UpdateClockDisplay() {
    if (!clock_displaying_ || !clock_hour_label_) {
        return;
    }
    
    // Get current time - system time is already set with timezone from server (ota.cc)
    // No need to set TZ env var as server already sends adjusted timestamp
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    
    // Check if system time is valid (year >= 2025)
    if (tm->tm_year < 2025 - 1900) {
        ESP_LOGW(TAG, "⏰ System time not set yet, tm_year: %d", tm->tm_year);
        // Show placeholder
        DisplayLockGuard lock(this);
        if (clock_hour_label_) {
            lv_label_set_text(clock_hour_label_, "--:--");
        }
        if (clock_date_label_) {
            lv_label_set_text(clock_date_label_, "--/--/--");
        }
        if (clock_time_label_) {
            lv_label_set_text(clock_time_label_, "---");
        }
        return;
    }
    
    // Format time as HH:MM
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm);
    
    // Format date (DD-MM-YY)
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%d-%m-%y", tm);
    
    // Day of week (short English names)
    const char* weekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    
    // Calculate seconds arc angle (0-360 degrees based on seconds)
    int sec_angle = (tm->tm_sec * 360) / 60;  // 0-360 degrees
    
    DisplayLockGuard lock(this);
    
    // Update main time label (HH:MM)
    if (clock_hour_label_) {
        lv_label_set_text(clock_hour_label_, time_str);
    }
    
    // Update date label
    if (clock_date_label_) {
        lv_label_set_text(clock_date_label_, date_str);
    }
    
    // Update weekday label
    if (clock_time_label_) {
        lv_label_set_text(clock_time_label_, weekdays[tm->tm_wday]);
    }
    
    // Animate red arc based on seconds (full rotation in 60 seconds)
    if (clock_arc_red_) {
        lv_arc_set_angles(clock_arc_red_, 0, sec_angle);
    }
    
    // Slowly rotate green arc (based on minutes)
    if (clock_arc_green_) {
        int green_angle = 180 + (tm->tm_min * 3);  // Slow rotation
        lv_arc_set_angles(clock_arc_green_, 0, green_angle % 360);
    }
    
    // Slowly rotate blue arc (based on hours)
    if (clock_arc_blue_) {
        int blue_angle = 120 + (tm->tm_hour * 15);  // Very slow rotation
        lv_arc_set_angles(clock_arc_blue_, 0, blue_angle % 360);
    }
}

void OttoEmojiDisplay::ShowClock(int duration_ms) {
    ESP_LOGI(TAG, "⏰ ShowClock called (duration: %d ms)", duration_ms);
    
    // Turn on display first
    TurnOn();
    
    // Cleanup any existing clock WITHOUT restoring emoji (special cleanup for re-show)
    // Stop timers first (outside lock)
    if (clock_timer_) {
        esp_timer_stop(clock_timer_);
    }
    if (clock_update_timer_) {
        esp_timer_stop(clock_update_timer_);
    }
    
    {
        DisplayLockGuard lock(this);
        
        // Delete old clock container if exists (without restoring emoji)
        if (clock_container_) {
            lv_obj_del(clock_container_);
            clock_container_ = nullptr;
            clock_time_label_ = nullptr;
            clock_date_label_ = nullptr;
            clock_hour_label_ = nullptr;
            clock_min_label_ = nullptr;
            clock_arc_red_ = nullptr;
            clock_arc_green_ = nullptr;
            clock_arc_blue_ = nullptr;
        }
        
        // Set flag to block emoji updates BEFORE hiding
        clock_displaying_ = true;
        
        // Hide emoji container
        if (emoji_box_) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_gif_) {
            lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Hide chat message
        if (chat_message_label_) {
            lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Get display dimensions (240x240 for ST7789)
        int disp_w = 240;
        int disp_h = 240;
        int arc_radius = 110;  // Radius for arcs
        int arc_width = 12;    // Width of arc lines
        (void)disp_w; (void)disp_h;  // Suppress unused warnings
        
        // Create clock container (full screen, black background)
        clock_container_ = lv_obj_create(content_);
        lv_obj_set_size(clock_container_, disp_w, disp_h);
        lv_obj_center(clock_container_);
        lv_obj_set_style_bg_color(clock_container_, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(clock_container_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(clock_container_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(clock_container_, 0, LV_PART_MAIN);
        lv_obj_clear_flag(clock_container_, LV_OBJ_FLAG_SCROLLABLE);
        
        // === Create colorful arcs (like ELECROW C3 watch) ===
        
        // Red arc (right side, seconds indicator - will animate)
        clock_arc_red_ = lv_arc_create(clock_container_);
        lv_obj_set_size(clock_arc_red_, arc_radius * 2, arc_radius * 2);
        lv_obj_center(clock_arc_red_);
        lv_arc_set_rotation(clock_arc_red_, 270);  // Start from top
        lv_arc_set_bg_angles(clock_arc_red_, 0, 360);
        lv_arc_set_angles(clock_arc_red_, 0, 180);  // Half circle initially
        lv_obj_set_style_arc_width(clock_arc_red_, arc_width, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(clock_arc_red_, lv_color_hex(0xFF3333), LV_PART_INDICATOR);  // Red
        lv_obj_set_style_arc_width(clock_arc_red_, arc_width, LV_PART_MAIN);
        lv_obj_set_style_arc_color(clock_arc_red_, lv_color_hex(0x331111), LV_PART_MAIN);  // Dark red bg
        lv_obj_remove_style(clock_arc_red_, NULL, LV_PART_KNOB);  // Remove knob
        lv_obj_clear_flag(clock_arc_red_, LV_OBJ_FLAG_CLICKABLE);
        
        // Green arc (left side - decorative)  
        clock_arc_green_ = lv_arc_create(clock_container_);
        lv_obj_set_size(clock_arc_green_, (arc_radius - 15) * 2, (arc_radius - 15) * 2);
        lv_obj_center(clock_arc_green_);
        lv_arc_set_rotation(clock_arc_green_, 180);
        lv_arc_set_bg_angles(clock_arc_green_, 0, 360);
        lv_arc_set_angles(clock_arc_green_, 0, 240);  // 2/3 circle
        lv_obj_set_style_arc_width(clock_arc_green_, arc_width - 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(clock_arc_green_, lv_color_hex(0x33FF33), LV_PART_INDICATOR);  // Green
        lv_obj_set_style_arc_width(clock_arc_green_, arc_width - 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(clock_arc_green_, lv_color_hex(0x113311), LV_PART_MAIN);  // Dark green bg
        lv_obj_remove_style(clock_arc_green_, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(clock_arc_green_, LV_OBJ_FLAG_CLICKABLE);
        
        // Blue arc (bottom - decorative)
        clock_arc_blue_ = lv_arc_create(clock_container_);
        lv_obj_set_size(clock_arc_blue_, (arc_radius - 30) * 2, (arc_radius - 30) * 2);
        lv_obj_center(clock_arc_blue_);
        lv_arc_set_rotation(clock_arc_blue_, 90);
        lv_arc_set_bg_angles(clock_arc_blue_, 0, 360);
        lv_arc_set_angles(clock_arc_blue_, 0, 200);
        lv_obj_set_style_arc_width(clock_arc_blue_, arc_width - 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(clock_arc_blue_, lv_color_hex(0x3399FF), LV_PART_INDICATOR);  // Blue
        lv_obj_set_style_arc_width(clock_arc_blue_, arc_width - 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(clock_arc_blue_, lv_color_hex(0x112233), LV_PART_MAIN);  // Dark blue bg
        lv_obj_remove_style(clock_arc_blue_, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(clock_arc_blue_, LV_OBJ_FLAG_CLICKABLE);
        
        // === Create time labels in center ===
        
        // Date label at top (smaller) - "03-12-25" style
        clock_date_label_ = lv_label_create(clock_container_);
        lv_label_set_text(clock_date_label_, "03-12-25");
        lv_obj_set_style_text_font(clock_date_label_, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(0x888888), LV_PART_MAIN);  // Gray
        lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, 0, -45);
        
        // Main time label (HH:MM) - large, centered, white
        clock_hour_label_ = lv_label_create(clock_container_);
        lv_label_set_text(clock_hour_label_, "00:00");
        lv_obj_set_style_text_font(clock_hour_label_, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(clock_hour_label_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White
        lv_obj_set_style_transform_scale(clock_hour_label_, 512, LV_PART_MAIN);  // 2x scale for bigger text
        lv_obj_set_style_text_letter_space(clock_hour_label_, 3, LV_PART_MAIN);  // Letter spacing
        // Set transform pivot to center of label for proper scaling alignment
        lv_obj_set_style_transform_pivot_x(clock_hour_label_, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(clock_hour_label_, LV_PCT(50), LV_PART_MAIN);
        lv_obj_align(clock_hour_label_, LV_ALIGN_CENTER, 0, 0);  // Center of screen
        
        // clock_min_label_ not used for seconds anymore, set to empty/hidden
        clock_min_label_ = lv_label_create(clock_container_);
        lv_label_set_text(clock_min_label_, "");
        lv_obj_add_flag(clock_min_label_, LV_OBJ_FLAG_HIDDEN);  // Hidden - not showing seconds
        
        // Day of week label at bottom - "WED"
        clock_time_label_ = lv_label_create(clock_container_);  // Reusing for weekday
        lv_label_set_text(clock_time_label_, "WED");
        lv_obj_set_style_text_font(clock_time_label_, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(clock_time_label_, lv_color_hex(0x888888), LV_PART_MAIN);  // Gray
        lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, 0, 50);
        
        // Bring clock to front
        lv_obj_move_foreground(clock_container_);
    }
    
    // Update clock display immediately (outside lock)
    UpdateClockDisplay();
    
    // Create clock update timer (every 1 second)
    if (!clock_update_timer_) {
        const esp_timer_create_args_t update_timer_args = {
            .callback = &OttoEmojiDisplay::ClockUpdateTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "clock_update_timer",
            .skip_unhandled_events = false
        };
        esp_err_t err = esp_timer_create(&update_timer_args, &clock_update_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "❌ Failed to create clock update timer: %s", esp_err_to_name(err));
        }
    }
    
    // Start update timer (every 1 second)
    if (clock_update_timer_) {
        esp_timer_stop(clock_update_timer_);
        esp_timer_start_periodic(clock_update_timer_, 1000000);  // 1 second in microseconds
    }
    
    // Set auto-hide timer
    if (duration_ms > 0) {
        if (!clock_timer_) {
            const esp_timer_create_args_t timer_args = {
                .callback = &OttoEmojiDisplay::ClockHideTimerCallback,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "clock_hide_timer",
                .skip_unhandled_events = false
            };
            esp_err_t err = esp_timer_create(&timer_args, &clock_timer_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "❌ Failed to create clock hide timer: %s", esp_err_to_name(err));
                return;
            }
        }
        esp_timer_stop(clock_timer_);
        esp_timer_start_once(clock_timer_, (uint64_t)duration_ms * 1000);
    }
    
    ESP_LOGI(TAG, "⏰ Clock displayed");
}

void OttoEmojiDisplay::HideClock() {
    // Stop timers first (outside lock)
    if (clock_timer_) {
        esp_timer_stop(clock_timer_);
    }
    if (clock_update_timer_) {
        esp_timer_stop(clock_update_timer_);
    }
    
    // Check if there's anything to hide
    if (!clock_displaying_ && !clock_container_) {
        return;
    }
    
    DisplayLockGuard lock(this);
    
    // Delete clock container (this also deletes children including arcs)
    if (clock_container_) {
        lv_obj_del(clock_container_);
        clock_container_ = nullptr;
        clock_time_label_ = nullptr;
        clock_date_label_ = nullptr;
        clock_hour_label_ = nullptr;
        clock_min_label_ = nullptr;
        clock_arc_red_ = nullptr;
        clock_arc_green_ = nullptr;
        clock_arc_blue_ = nullptr;
    }
    
    // Restore emoji container
    if (emoji_box_) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Restore appropriate emoji based on current mode
    if (use_otto_emoji_) {
        if (emotion_gif_) {
            lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emotion_gif_);
        }
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (emoji_label_) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_gif_) {
            lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Restore chat message
    if (chat_message_label_) {
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Reset flag
    clock_displaying_ = false;
    
    ESP_LOGI(TAG, "⏰ Clock hidden, emoji restored");
}

void OttoEmojiDisplay::SetIdleClockEnabled(bool enabled) {
    idle_clock_enabled_ = enabled;
    ESP_LOGI(TAG, "⏰ Idle clock %s", enabled ? "ENABLED" : "DISABLED");
    
    if (enabled) {
        // When enabled, show clock immediately with no timeout (0 = permanent)
        ShowClock(0);
    } else {
        // When disabled, hide clock
        HideClock();
    }
}

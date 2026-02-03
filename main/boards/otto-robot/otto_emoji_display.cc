#include "otto_emoji_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <font_awesome.h>
#include <cbin_font.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <memory>

// External font declaration (PuHui supports Vietnamese)
extern "C" {
    LV_FONT_DECLARE(font_puhui_16_4);
    LV_FONT_DECLARE(font_awesome_30_4);
    LV_FONT_DECLARE(BUILTIN_TEXT_FONT);  // For Unicode emoji support
}

#include "display/lcd_display.h"
#include "application.h"
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
      emoji_overlay_mode_(false) { // Emoji overlay disabled by default
    
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
    // Set large size for emoji label to display Unicode emoji properly
    lv_obj_set_size(emoji_label_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_text_align(emoji_label_, LV_TEXT_ALIGN_CENTER, 0);
    // Set large font size for Unicode emoji display (Twemoji mode)
    // Use text font for Unicode emoji support (Font Awesome doesn't support Unicode emoji)
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme) {
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        auto text_font = lvgl_theme->text_font();
        if (text_font) {
            // Use text font which supports Unicode emoji
            lv_obj_set_style_text_font(emoji_label_, text_font->font(), 0);
        } else {
            // Fallback to BUILTIN_TEXT_FONT
            lv_obj_set_style_text_font(emoji_label_, &BUILTIN_TEXT_FONT, 0);
        }
    } else {
        // Fallback to BUILTIN_TEXT_FONT if theme not available
        lv_obj_set_style_text_font(emoji_label_, &BUILTIN_TEXT_FONT, 0);
    }
    // Set large font size for emoji (increase line height and letter spacing)
    lv_obj_set_style_text_letter_space(emoji_label_, 0, 0);
    lv_obj_set_style_text_line_space(emoji_label_, 0, 0);
    // Make emoji larger by setting style
    lv_obj_set_style_text_font(emoji_label_, &BUILTIN_TEXT_FONT, 0);
    
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
        // Otto GIF mode: show GIF, hide emoji_image (parent class handles emoji_image for Twemoji)
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
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Twemoji mode: hide GIF, parent class LcdDisplay will handle emoji_image display
        gif_controller_ = std::make_unique<LvglGif>(&staticstate);
        if (gif_controller_->IsLoaded()) {
            lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
        }
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        // emoji_image_ visibility will be managed by parent LcdDisplay::SetEmotion()
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
    
    // Check if we're forcing special emoji that blocks all other emoji changes
    const char* emotion_to_use = emotion;
#ifdef CONFIG_BOARD_TYPE_OTTO_ROBOT
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
#endif
    
    // Turn on display and reset auto-off timer on activity
    TurnOn();
    
    // For text emoji mode (Twemoji), directly call parent class without rate limiting
    // Parent class LcdDisplay::SetEmotion() handles Twemoji32/64 images via emoji_collection
    if (!use_otto_emoji_) {
        LcdDisplay::SetEmotion(emotion_to_use);
        
        // IMPORTANT: In WeChat message style, parent class hides "neutral" emoji when content has children
        // We want to ALWAYS show emoji in Twemoji mode, so force unhide after parent call
        #if CONFIG_USE_WECHAT_MESSAGE_STYLE
        if (strcmp(emotion_to_use, "neutral") == 0) {
            DisplayLockGuard lock(this);
            // Check if emoji_image_ was set by parent (has valid source)
            const void* src = lv_image_get_src(emoji_image_);
            if (src != nullptr) {
                lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG, "📝 Twemoji表情 (forced visible): %s", emotion_to_use);
            } else {
                ESP_LOGI(TAG, "📝 Twemoji表情: %s", emotion_to_use);
            }
        } else {
            ESP_LOGI(TAG, "📝 Twemoji表情: %s", emotion_to_use);
        }
        #else
        ESP_LOGI(TAG, "📝 Twemoji表情: %s", emotion_to_use);
        #endif
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
        ESP_LOGI(TAG, "🤖 Otto表情(缓存): %s", emotion_to_use);
        return;
    }
    
    // Find emotion in map
    for (const auto& map : emotion_maps_) {
        if (map.name && emotion_to_use && strcmp(map.name, emotion_to_use) == 0) {
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
            ESP_LOGI(TAG, "🤖 Otto表情: %s", emotion_to_use);
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
    
    if (use_otto_emoji_) {
        // 切换到Otto emoji模式
        ESP_LOGI(TAG, "切换到Otto GIF表情模式");
        
        // 显示GIF容器，隐藏默认emoji
        if (emotion_gif_) {
            lv_obj_remove_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
            // CRITICAL: Re-activate the GIF by resetting the source
            gif_controller_ = std::make_unique<LvglGif>(&staticstate);
            if (gif_controller_->IsLoaded()) {
                gif_controller_->SetFrameCallback([this]() {
                    lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
                });
                lv_img_set_src(emotion_gif_, gif_controller_->image_dsc());
                gif_controller_->Start();
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
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        // In Twemoji mode, we use emoji_image_ (from emoji_collection) instead of emoji_label_
        // Hide label, show image
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_) {
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            // Set initial size for Twemoji images
            int emoji_size = std::min(LV_HOR_RES, LV_VER_RES) * 0.8; // 80% of screen size
            lv_obj_set_size(emoji_image_, emoji_size, emoji_size);
            lv_obj_center(emoji_image_);
        }
        // Set default happy Twemoji (will use emoji_collection image)
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
    
    display_on_ = true;
    
    // Reset auto-off timer
    ResetAutoOffTimer();
}

void OttoEmojiDisplay::TurnOff() {
    if (!display_on_) {
        return;  // Already off
    }
    
    ESP_LOGI(TAG, "🌙 Turning display OFF (idle timeout)");
    
    // Turn off LCD panel
    if (panel_) {
        esp_lcd_panel_disp_on_off(panel_, false);
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

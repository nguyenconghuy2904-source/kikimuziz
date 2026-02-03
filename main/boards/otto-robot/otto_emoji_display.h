#pragma once

#include "display/lcd_display.h"
#include "display/lvgl_display/gif/lvgl_gif.h"
#include "otto_emoji_gif.h"
#include <memory>

/**
 * @brief Otto机器人GIF表情显示类
 * 继承LcdDisplay，添加GIF表情支持
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;

    // 重写表情设置方法
    virtual void SetEmotion(const char* emotion) override;

    // 重写聊天消息设置方法
    virtual void SetChatMessage(const char* role, const char* content) override;

    // 音乐信息显示方法
    // TODO: void SetMusicInfo(const char* song_name);

    // 重写状态栏更新方法，禁用低电量弹窗显示
    virtual void UpdateStatusBar(bool update_all = false) override;

    // 重写图片预览方法（继承父类实现）
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        SpiLcdDisplay::SetPreviewImage(std::move(image));
    }

    //切换emoji模式方法
    void SetEmojiMode(bool use_otto_emoji);
    bool IsUsingOttoEmoji() const { return use_otto_emoji_; }

    // UDP Drawing support methods
    void EnableDrawingCanvas(bool enable);
    void ClearDrawingCanvas();
    void DrawPixel(int x, int y, bool state);
    bool IsDrawingCanvasEnabled() const { return drawing_canvas_enabled_; }

    // Display power management
    void TurnOn();  // Turn on display and reset auto-off timer
    void TurnOff(); // Turn off display
    bool IsOn() const { return display_on_; }
    void SetAutoOffEnabled(bool enabled); // Enable/disable auto-off after 5 minutes idle
    bool IsAutoOffEnabled() const { return auto_off_enabled_; }

    // Emoji overlay mode (for QR code display - emoji on top of chat message)
    void SetEmojiOverlayMode(bool enable); // Enable/disable emoji overlay mode
    bool IsEmojiOverlayMode() const { return emoji_overlay_mode_; }
    
    // Hide/show chat message (for QR code display)
    void SetChatMessageHidden(bool hidden); // Hide or show chat message

private:
    void SetupGifContainer();
    void InitializeDrawingCanvas();
    void CleanupDrawingCanvas();

    lv_obj_t* emotion_gif_;  ///< GIF表情组件 (lv_img object)
    std::unique_ptr<LvglGif> gif_controller_;  ///< GIF controller for animation
    bool use_otto_emoji_;    ///< 是否使用Otto emoji (true) 还是默认emoji (false)

    // UDP Drawing canvas
    lv_obj_t* drawing_canvas_;       ///< Drawing canvas object
    void* drawing_canvas_buf_;       ///< Canvas buffer
    bool drawing_canvas_enabled_;    ///< Is drawing mode enabled

    // Display power management
    bool display_on_;                ///< Display power state
    bool auto_off_enabled_;          ///< Auto-off feature enabled/disabled
    esp_timer_handle_t auto_off_timer_; ///< Timer for auto-off after 5 min idle
    static void AutoOffTimerCallback(void* arg);
    void ResetAutoOffTimer();        ///< Reset the 5-minute idle timer

    // Emoji overlay mode
    bool emoji_overlay_mode_;        ///< When true, emoji is displayed on top of chat message

    // 表情映射
    struct EmotionMap {
        const char* name;
        const lv_img_dsc_t* gif;
    };

    static const EmotionMap emotion_maps_[];
};
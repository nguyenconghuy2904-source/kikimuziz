#include "drawing_display.h"
#include <esp_log.h>
#include <cstring>

#define TAG "DrawingDisplay"

DrawingDisplay::DrawingDisplay(int width, int height)
    : Display(),
      width_(width),
      height_(height),
      canvas_(nullptr),
      canvas_buf_(nullptr),
      canvas_enabled_(false),
      brightness_(100) {
}

DrawingDisplay::~DrawingDisplay() {
    CleanupCanvas();
}

bool DrawingDisplay::Lock(int timeout_ms) {
    // No actual locking needed for canvas overlay
    return true;
}

void DrawingDisplay::Unlock() {
    // No actual unlocking needed
}

void DrawingDisplay::StartDisplay() {
    // Canvas drawing DISABLED for testing
    ESP_LOGI(TAG, "🚀 Starting DrawingDisplay - Canvas DISABLED");
    // EnableCanvas(true);  // Commented out
}

void DrawingDisplay::SetBrightness(int brightness) {
    brightness_ = brightness;
}

int DrawingDisplay::GetBrightness() const {
    return brightness_;
}

void DrawingDisplay::EnableCanvas(bool enable) {
    DisplayLockGuard lock(this);
    
    if (enable == canvas_enabled_) {
        return;
    }
    
    canvas_enabled_ = enable;
    
    if (enable) {
        InitializeCanvas();
        ESP_LOGI(TAG, "🎨 Drawing canvas ENABLED (%dx%d)", width_, height_);
    } else {
        CleanupCanvas();
        ESP_LOGI(TAG, "🎨 Drawing canvas DISABLED");
    }
}

void DrawingDisplay::InitializeCanvas() {
    CleanupCanvas();  // Clean up any existing canvas
    
    // Allocate canvas buffer - RGB565 format
    size_t buf_size = width_ * height_ * sizeof(lv_color_t);
    canvas_buf_ = malloc(buf_size);
    if (!canvas_buf_) {
        ESP_LOGE(TAG, "❌ Failed to allocate canvas buffer (%zu bytes)", buf_size);
        return;
    }
    
    // Initialize buffer to black
    memset(canvas_buf_, 0, buf_size);
    
    // Create LVGL canvas on active screen
    canvas_ = lv_canvas_create(lv_scr_act());
    if (!canvas_) {
        ESP_LOGE(TAG, "❌ Failed to create LVGL canvas");
        free(canvas_buf_);
        canvas_buf_ = nullptr;
        return;
    }
    
    // Use RGB565 format
    lv_canvas_set_buffer(canvas_, canvas_buf_, width_, height_, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas_, width_, height_);
    lv_obj_set_pos(canvas_, 0, 0);
    
    // Fill with black
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    
    // Make canvas visible and clickable
    lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    
    // Move canvas to FOREGROUND (on top of emoji) so drawings are visible
    lv_obj_move_foreground(canvas_);
    lv_obj_move_to_index(canvas_, -1);
    
    ESP_LOGI(TAG, "✅ Canvas initialized: %dx%d, buffer=%u bytes (RGB565, foreground layer - will hide emoji)", width_, height_, (unsigned int)buf_size);
}

void DrawingDisplay::CleanupCanvas() {
    if (canvas_) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
    }
    
    if (canvas_buf_) {
        free(canvas_buf_);
        canvas_buf_ = nullptr;
    }
}

void DrawingDisplay::ClearCanvas() {
    // Canvas drawing DISABLED - do nothing
    ESP_LOGI(TAG, "🧹 ClearCanvas called but canvas is DISABLED");
    return;
    
    /*
    if (!canvas_) {
        ESP_LOGW(TAG, "⚠️ No canvas to clear");
        return;
    }
    
    // DISABLE canvas to show emoji again
    EnableCanvas(false);
    
    ESP_LOGI(TAG, "🧹 Canvas cleared - disabled to show emoji");
    */
}

void DrawingDisplay::DrawPixel(int x, int y, bool state) {
    // Drawing DISABLED - do nothing
    ESP_LOGW(TAG, "⚠️ DrawPixel called but canvas drawing is DISABLED");
    return;
    
    /*
    if (!canvas_) {
        ESP_LOGW(TAG, "⚠️ Cannot draw pixel - canvas not initialized!");
        return;
    }
    
    // Validate coordinates
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        ESP_LOGW(TAG, "⚠️ Pixel out of bounds: (%d,%d) vs (%d,%d)", x, y, width_, height_);
        return;
    }
    
    DisplayLockGuard lock(this);
    
    if (state) {
        // Draw: White pixel with full opacity - VISIBLE!
        lv_canvas_set_px(canvas_, x, y, lv_color_white(), LV_OPA_COVER);
        // Debug: Log first few pixels
        static int pixel_count = 0;
        if (pixel_count++ < 10) {
            ESP_LOGI(TAG, "✏️ Drew WHITE pixel at (%d,%d)", x, y);
        }
    } else {
        // Erase: Make pixel fully transparent so emoji shows through
        lv_canvas_set_px(canvas_, x, y, lv_color_black(), LV_OPA_TRANSP);
    }
    
    // Invalidate ENTIRE canvas to force immediate redraw - fixes choppy lines!
    lv_obj_invalidate(canvas_);
    
    // Force immediate refresh every 5 pixels to make drawing smooth
    static int draw_count = 0;
    if (++draw_count % 5 == 0) {
        lv_refr_now(NULL);
    }
    */
}

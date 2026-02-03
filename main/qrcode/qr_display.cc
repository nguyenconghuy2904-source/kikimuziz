#include "qr_display.h"
#include <esp_log.h>

#ifdef HAVE_LVGL
#include <lvgl.h>
#include <cbin_font.h>

// External font declaration (same as chat message)
extern "C" {
    LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
}
#endif

#define TAG "QRDisplay"

#ifdef HAVE_LVGL
static lv_obj_t* ip_label = NULL;
#endif

bool qr_display_show(Display* display, const char* text) {
    if (!display || !text) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

#ifdef HAVE_LVGL
    ESP_LOGI(TAG, "Displaying IP address: %s", text);
    
    // Check if LVGL screen is available
    lv_obj_t* screen = lv_scr_act();
    if (!screen) {
        ESP_LOGE(TAG, "LVGL screen not available, cannot display IP");
        return false;
    }
    
    // Clean up old display if exists
    qr_display_clear(display);
    
    // Create label to show IP address directly in screen
    // This ensures it's not hidden by container_ or content_ objects
    ip_label = lv_label_create(screen);
    if (!ip_label) {
        ESP_LOGE(TAG, "Failed to create IP label");
        return false;
    }
    ESP_LOGI(TAG, "✅ IP label created successfully");
    
    // Ensure label is not hidden
    lv_obj_clear_flag(ip_label, LV_OBJ_FLAG_HIDDEN);
    
    // Set label text and style (same as chat message)
    lv_label_set_text(ip_label, text);
    ESP_LOGI(TAG, "✅ IP label text set: %s", text);
    
    // Set color to yellow (RGB: 255, 255, 0) - use hex format for better compatibility
    lv_obj_set_style_text_color(ip_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(ip_label, &BUILTIN_TEXT_FONT, 0); // Same font as chat message
    lv_obj_set_style_text_align(ip_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Set width and background similar to chat message
    lv_obj_set_width(ip_label, LV_HOR_RES * 0.9);
    lv_obj_set_style_bg_opa(ip_label, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(ip_label, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(ip_label, 8, 0);
    lv_obj_set_style_pad_hor(ip_label, 10, 0);
    ESP_LOGI(TAG, "✅ IP label style configured");
    
    // Position at same location as chat message (bottom, 20px from bottom)
    lv_obj_align(ip_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    // Ensure label is clickable and not clipped (important for visibility)
    lv_obj_clear_flag(ip_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ip_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // Move label to foreground to ensure it's on top
    lv_obj_move_foreground(ip_label);
    
    // Mark the label for redraw (thread-safe way)
    lv_obj_invalidate(ip_label);
    
    ESP_LOGI(TAG, "✅ IP label created and moved to foreground");
    ESP_LOGI(TAG, "IP address displayed: %s (will remain until next content update)", text);
    return true;
#else
    ESP_LOGW(TAG, "LVGL not available, cannot display IP address");
    return false;
#endif
}

void qr_display_clear(Display* display) {
    (void)display; // Unused parameter
    
#ifdef HAVE_LVGL
    if (ip_label) {
        lv_obj_del(ip_label);
        ip_label = NULL;
        ESP_LOGI(TAG, "IP address display cleared");
    }
#endif
}

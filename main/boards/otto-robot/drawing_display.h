#pragma once

#include "display/display.h"
#include <lvgl.h>

/**
 * @brief Drawing canvas display for UDP drawing functionality
 * This is a standalone display that can be overlayed on top of existing display
 */
class DrawingDisplay : public Display {
public:
    DrawingDisplay(int width, int height);
    virtual ~DrawingDisplay();

    // Display interface implementations (required by base class)
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    // Custom display methods
    void StartDisplay();
    void SetBrightness(int brightness);
    int GetBrightness() const;

    // Accessors
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    // Drawing canvas methods
    void EnableCanvas(bool enable);
    bool IsCanvasEnabled() const { return canvas_enabled_; }
    void ClearCanvas();
    void DrawPixel(int x, int y, bool state);
    
    // Get canvas object for integration
    lv_obj_t* GetCanvasObject() const { return canvas_; }

private:
    void InitializeCanvas();
    void CleanupCanvas();

    int width_;
    int height_;
    lv_obj_t* canvas_;
    void* canvas_buf_;
    bool canvas_enabled_;
    int brightness_;
};

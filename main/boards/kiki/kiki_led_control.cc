#include "kiki_led_control.h"
#include "config.h"
#include <led_strip.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

static const char *TAG = "KikiLED";

// LED strip handle
static led_strip_handle_t led_strip = NULL;

// Current LED state
static led_state_t current_state = {
    .r = 0,
    .g = 0,
    .b = 0,
    .brightness = 128,
    .mode = LED_MODE_OFF,
    .speed = 50
};

// Animation task handle
static TaskHandle_t animation_task_handle = NULL;
static bool animation_running = false;

// Helper: Apply brightness to color
static void apply_brightness(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t brightness) {
    *r = (*r * brightness) / 255;
    *g = (*g * brightness) / 255;
    *b = (*b * brightness) / 255;
}

// Helper: Set all LEDs to one color
static void set_all_leds(uint8_t r, uint8_t g, uint8_t b) {
    apply_brightness(&r, &g, &b, current_state.brightness);
    for (int i = 0; i < LED_8BIT_COUNT; i++) {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
    led_strip_refresh(led_strip);
}

// Helper: HSV to RGB conversion
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t region, remainder, p, q, t;
    
    if (s == 0) {
        *r = v;
        *g = v;
        *b = v;
        return;
    }
    
    region = h / 43;
    remainder = (h - (region * 43)) * 6;
    
    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

// Animation task
static void led_animation_task(void *pvParameters) {
    uint32_t counter = 0;
    
    while (animation_running) {
        switch (current_state.mode) {
            case LED_MODE_RAINBOW: {
                for (int i = 0; i < LED_8BIT_COUNT; i++) {
                    uint16_t hue = (counter + i * (255 / LED_8BIT_COUNT)) % 256;
                    uint8_t r, g, b;
                    hsv_to_rgb(hue, 255, 255, &r, &g, &b);
                    apply_brightness(&r, &g, &b, current_state.brightness);
                    led_strip_set_pixel(led_strip, i, r, g, b);
                }
                led_strip_refresh(led_strip);
                counter = (counter + 5) % 256;
                break;
            }
            
            case LED_MODE_BREATHING: {
                float breath = (sin(counter * 0.05) + 1.0) / 2.0;
                uint8_t brightness = (uint8_t)(breath * current_state.brightness);
                uint8_t r = current_state.r;
                uint8_t g = current_state.g;
                uint8_t b = current_state.b;
                apply_brightness(&r, &g, &b, brightness);
                set_all_leds(r, g, b);
                counter++;
                break;
            }
            
            case LED_MODE_CHASE: {
                int pos = (counter / 2) % LED_8BIT_COUNT;
                for (int i = 0; i < LED_8BIT_COUNT; i++) {
                    if (i == pos) {
                        uint8_t r = current_state.r;
                        uint8_t g = current_state.g;
                        uint8_t b = current_state.b;
                        apply_brightness(&r, &g, &b, current_state.brightness);
                        led_strip_set_pixel(led_strip, i, r, g, b);
                    } else {
                        led_strip_set_pixel(led_strip, i, 0, 0, 0);
                    }
                }
                led_strip_refresh(led_strip);
                counter++;
                break;
            }
            
            case LED_MODE_BLINK: {
                if ((counter / 10) % 2 == 0) {
                    set_all_leds(current_state.r, current_state.g, current_state.b);
                } else {
                    set_all_leds(0, 0, 0);
                }
                counter++;
                break;
            }
            
            default:
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(current_state.speed));
    }
    
    vTaskDelete(NULL);
}

// Stop animation task
static void stop_animation_task(void) {
    if (animation_task_handle != NULL) {
        animation_running = false;
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for task to exit
        animation_task_handle = NULL;
    }
}

// Start animation task
static void start_animation_task(void) {
    stop_animation_task();
    animation_running = true;
    xTaskCreate(led_animation_task, "led_anim", 2048, NULL, 5, &animation_task_handle);
}

// Initialize LED strip
void kiki_led_init(void) {
    ESP_LOGI(TAG, "🌈 Initializing 8-bit WS2812 LED strip on GPIO %d", LED_8BIT_PIN);
    
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_8BIT_PIN,
        .max_leds = LED_8BIT_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };
    
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        }
    };
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    ESP_LOGI(TAG, "✅ LED strip hardware initialized");
    
    // Play boot animation first (loading effect)
    kiki_led_boot_animation();
    
    // Load saved state from NVS
    kiki_led_load_from_nvs();
    
    ESP_LOGI(TAG, "✅ LED strip initialized with %d LEDs", LED_8BIT_COUNT);
    ESP_LOGI(TAG, "📊 State: mode=%d, RGB=(%d,%d,%d), brightness=%d", 
             current_state.mode, current_state.r, current_state.g, current_state.b, current_state.brightness);
    
    // Apply the loaded state immediately after boot animation
    ESP_LOGI(TAG, "🎨 Applying saved LED state...");
    kiki_led_update();
}

// Set LED color
void kiki_led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    current_state.r = r;
    current_state.g = g;
    current_state.b = b;
}

// Set LED mode
void kiki_led_set_mode(led_mode_t mode) {
    current_state.mode = mode;
    
    // Stop animation for static modes
    if (mode == LED_MODE_OFF || mode == LED_MODE_SOLID) {
        stop_animation_task();
    }
}

// Set LED brightness
void kiki_led_set_brightness(uint8_t brightness) {
    current_state.brightness = brightness;
}

// Set animation speed
void kiki_led_set_speed(uint16_t speed_ms) {
    if (speed_ms < 10) speed_ms = 10;
    if (speed_ms > 500) speed_ms = 500;
    current_state.speed = speed_ms;
}

// Get current LED state
const led_state_t* kiki_led_get_state(void) {
    return &current_state;
}

// Update LEDs
void kiki_led_update(void) {
    switch (current_state.mode) {
        case LED_MODE_OFF:
            stop_animation_task();
            set_all_leds(0, 0, 0);
            break;
            
        case LED_MODE_SOLID:
            stop_animation_task();
            set_all_leds(current_state.r, current_state.g, current_state.b);
            break;
            
        case LED_MODE_RAINBOW:
        case LED_MODE_BREATHING:
        case LED_MODE_CHASE:
        case LED_MODE_BLINK:
            start_animation_task();
            break;
            
        default:
            break;
    }
}

// Turn off all LEDs
void kiki_led_off(void) {
    current_state.mode = LED_MODE_OFF;
    kiki_led_update();
}

// Boot animation - LED sáng dần từng bóng như thanh loading
void kiki_led_boot_animation(void) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED strip not initialized!");
        return;
    }
    
    ESP_LOGI(TAG, "🚀 Starting boot animation...");
    
    // Clear all LEDs first
    for (int i = 0; i < LED_8BIT_COUNT; i++) {
        led_strip_set_pixel(led_strip, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Light up each LED progressively (loading effect)
    uint8_t colors[][3] = {
        {255, 0, 0},     // Red
        {255, 128, 0},   // Orange
        {255, 255, 0},   // Yellow
        {128, 255, 0},   // Yellow-Green
        {0, 255, 0},     // Green
        {0, 255, 128},   // Cyan-Green
        {0, 128, 255},   // Light Blue
        {0, 0, 255}      // Blue
    };
    
    for (int i = 0; i < LED_8BIT_COUNT; i++) {
        // Light up current LED
        led_strip_set_pixel(led_strip, i, colors[i][0], colors[i][1], colors[i][2]);
        led_strip_refresh(led_strip);
        
        // Log progress
        int progress = ((i + 1) * 100) / LED_8BIT_COUNT;
        ESP_LOGI(TAG, "📊 Boot progress: %d%% (LED %d/%d)", progress, i+1, LED_8BIT_COUNT);
        
        // Delay between each LED - slower for sync with system boot (500ms per LED)
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Keep all LEDs on for a moment to show 100% completion
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Fade out slowly
    for (int brightness = 255; brightness >= 0; brightness -= 3) {
        for (int i = 0; i < LED_8BIT_COUNT; i++) {
            uint8_t r = (colors[i][0] * brightness) / 255;
            uint8_t g = (colors[i][1] * brightness) / 255;
            uint8_t b = (colors[i][2] * brightness) / 255;
            led_strip_set_pixel(led_strip, i, r, g, b);
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    
    ESP_LOGI(TAG, "✅ Boot animation complete!");
}

// Save LED state to NVS
void kiki_led_save_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("kiki_led", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    nvs_set_u8(nvs_handle, "r", current_state.r);
    nvs_set_u8(nvs_handle, "g", current_state.g);
    nvs_set_u8(nvs_handle, "b", current_state.b);
    nvs_set_u8(nvs_handle, "brightness", current_state.brightness);
    nvs_set_u8(nvs_handle, "mode", (uint8_t)current_state.mode);
    nvs_set_u16(nvs_handle, "speed", current_state.speed);
    
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "💾 LED state saved to NVS");
}

// Load LED state from NVS
void kiki_led_load_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("kiki_led", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved LED state in NVS (first boot?) - using default: WHITE 50%");
        // Set default state for first boot
        current_state.r = 255;
        current_state.g = 255;
        current_state.b = 255;
        current_state.brightness = 128;  // 50% brightness
        current_state.mode = LED_MODE_SOLID;
        current_state.speed = 50;
        return;
    }
    
    uint8_t mode_u8 = 0;
    nvs_get_u8(nvs_handle, "r", &current_state.r);
    nvs_get_u8(nvs_handle, "g", &current_state.g);
    nvs_get_u8(nvs_handle, "b", &current_state.b);
    nvs_get_u8(nvs_handle, "brightness", &current_state.brightness);
    nvs_get_u8(nvs_handle, "mode", &mode_u8);
    nvs_get_u16(nvs_handle, "speed", &current_state.speed);
    
    current_state.mode = (led_mode_t)mode_u8;
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "📂 LED state loaded from NVS");
}

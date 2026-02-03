#ifndef KIKI_LED_CONTROL_H
#define KIKI_LED_CONTROL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LED modes
typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_SOLID = 1,
    LED_MODE_RAINBOW = 2,
    LED_MODE_BREATHING = 3,
    LED_MODE_CHASE = 4,
    LED_MODE_BLINK = 5
} led_mode_t;

// LED state structure
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
    led_mode_t mode;
    uint16_t speed;  // Animation speed in milliseconds
} led_state_t;

// Initialize LED strip
void kiki_led_init(void);

// Set LED color (RGB 0-255)
void kiki_led_set_color(uint8_t r, uint8_t g, uint8_t b);

// Set LED mode
void kiki_led_set_mode(led_mode_t mode);

// Set LED brightness (0-255)
void kiki_led_set_brightness(uint8_t brightness);

// Set animation speed (10-500ms)
void kiki_led_set_speed(uint16_t speed_ms);

// Get current LED state
const led_state_t* kiki_led_get_state(void);

// Update LEDs (apply current settings)
void kiki_led_update(void);

// Turn off all LEDs
void kiki_led_off(void);

// Boot animation (loading effect)
void kiki_led_boot_animation(void);

// Save LED state to NVS
void kiki_led_save_to_nvs(void);

// Load LED state from NVS
void kiki_led_load_from_nvs(void);

#ifdef __cplusplus
}
#endif

#endif // KIKI_LED_CONTROL_H

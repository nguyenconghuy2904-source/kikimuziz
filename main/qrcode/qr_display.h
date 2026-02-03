#ifndef QR_DISPLAY_H
#define QR_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate and display QR code on the display
 * 
 * @param display Pointer to Display object
 * @param text Text to encode in QR code (e.g., URL, WiFi credentials)
 * @return true if successful, false otherwise
 */
bool qr_display_show(Display* display, const char* text);

/**
 * @brief Clear QR code from display
 * 
 * @param display Pointer to Display object
 */
void qr_display_clear(Display* display);

#ifdef __cplusplus
}
#endif

#endif // QR_DISPLAY_H

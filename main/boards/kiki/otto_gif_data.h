/**
 * @file otto_gif_data.h
 * @brief GIF image declarations for Otto emoji display
 * 
 * This header declares the GIF image descriptors for the 6 Otto emoji GIFs.
 * The GIF data is embedded from the otto-emoji-gif-component and converted
 * to lv_img_dsc_t format for use with LVGL.
 */

#ifndef OTTO_GIF_DATA_H
#define OTTO_GIF_DATA_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// GIF image descriptors - declared as extern, defined in otto_gif_data.c
extern const lv_img_dsc_t staticstate;
extern const lv_img_dsc_t happy;
extern const lv_img_dsc_t sad;
extern const lv_img_dsc_t anger;
extern const lv_img_dsc_t scare;
extern const lv_img_dsc_t buxue;

#ifdef __cplusplus
}
#endif

#endif /* OTTO_GIF_DATA_H */

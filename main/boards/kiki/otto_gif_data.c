/**
 * @file otto_gif_data.c
 * @brief GIF image data definitions for Otto emoji display
 * 
 * This file defines the GIF image descriptors that reference embedded binary
 * GIF data from the otto-emoji-gif-component.
 */

#include "otto_gif_data.h"
#include <stdint.h>

// External references to embedded GIF binary data
// These symbols are created by CMake's EMBED_FILES directive
extern const uint8_t staticstate_gif_start[] asm("_binary_staticstate_gif_start");
extern const uint8_t staticstate_gif_end[] asm("_binary_staticstate_gif_end");

extern const uint8_t happy_gif_start[] asm("_binary_happy_gif_start");
extern const uint8_t happy_gif_end[] asm("_binary_happy_gif_end");

extern const uint8_t sad_gif_start[] asm("_binary_sad_gif_start");
extern const uint8_t sad_gif_end[] asm("_binary_sad_gif_end");

extern const uint8_t anger_gif_start[] asm("_binary_anger_gif_start");
extern const uint8_t anger_gif_end[] asm("_binary_anger_gif_end");

extern const uint8_t scare_gif_start[] asm("_binary_scare_gif_start");
extern const uint8_t scare_gif_end[] asm("_binary_scare_gif_end");

extern const uint8_t buxue_gif_start[] asm("_binary_buxue_gif_start");
extern const uint8_t buxue_gif_end[] asm("_binary_buxue_gif_end");

// Define lv_img_dsc_t structures that point to the embedded GIF data
// Note: These use LV_IMAGE_SRC_DATA format - gifdec will parse the GIF header
const lv_img_dsc_t staticstate = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,  // Raw format for GIF data
        .flags = 0,
        .w = 0,  // Will be determined by gifdec
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,  // Will be calculated at runtime
    .data = staticstate_gif_start,
};

const lv_img_dsc_t happy = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = happy_gif_start,
};

const lv_img_dsc_t sad = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = sad_gif_start,
};

const lv_img_dsc_t anger = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = anger_gif_start,
};

const lv_img_dsc_t scare = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = scare_gif_start,
};

const lv_img_dsc_t buxue = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .flags = 0,
        .w = 0,
        .h = 0,
        .stride = 0,
    },
    .data_size = 0,
    .data = buxue_gif_start,
};

#ifndef OTTO_WEBSERVER_H
#define OTTO_WEBSERVER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
class UdpDrawService;
class DrawingDisplay;

// WiFi credentials - Update these for your network
#define WIFI_SSID      "Huywifi"
#define WIFI_PASS      "0389827643"
#define WIFI_MAXIMUM_RETRY  5

// No authentication required - direct control
extern bool webserver_enabled;

// Function declarations
esp_err_t otto_wifi_init_sta(void);
esp_err_t otto_start_webserver(void);
esp_err_t otto_stop_webserver(void);
esp_err_t otto_auto_start_webserver_if_wifi_connected(void);
esp_err_t otto_register_wifi_listener(void);
void otto_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void otto_system_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// HTTP handlers
esp_err_t otto_root_handler(httpd_req_t *req);
esp_err_t otto_action_handler(httpd_req_t *req);
esp_err_t otto_status_handler(httpd_req_t *req);
esp_err_t otto_emotion_handler(httpd_req_t *req);
esp_err_t otto_emoji_mode_handler(httpd_req_t *req);
esp_err_t otto_touch_sensor_handler(httpd_req_t *req);
esp_err_t otto_screen_toggle_handler(httpd_req_t *req);
esp_err_t otto_screen_rotation_handler(httpd_req_t *req);
esp_err_t otto_wake_up_handler(httpd_req_t *req);
esp_err_t otto_forget_wifi_handler(httpd_req_t *req);
esp_err_t otto_send_text_to_ai_handler(httpd_req_t *req);  // Send text to AI handler

// UDP Drawing handlers
esp_err_t otto_drawing_mode_handler(httpd_req_t *req);
esp_err_t otto_drawing_clear_handler(httpd_req_t *req);
esp_err_t otto_drawing_pixel_handler(httpd_req_t *req);
esp_err_t otto_drawing_status_handler(httpd_req_t *req);
esp_err_t otto_drawing_page_handler(httpd_req_t *req);

// LED control handlers
esp_err_t otto_led_color_handler(httpd_req_t *req);
esp_err_t otto_led_mode_handler(httpd_req_t *req);
esp_err_t otto_led_brightness_handler(httpd_req_t *req);
esp_err_t otto_led_speed_handler(httpd_req_t *req);
esp_err_t otto_led_state_handler(httpd_req_t *req);
esp_err_t otto_led_off_handler(httpd_req_t *req);
esp_err_t otto_led_save_handler(httpd_req_t *req);

// Memory slot handlers
esp_err_t otto_save_slot_handler(httpd_req_t *req);
esp_err_t otto_play_slot_handler(httpd_req_t *req);
esp_err_t otto_slot_info_handler(httpd_req_t *req);

// Play memory slot directly (can be called from other modules like application.cc)
// slot: 1-3 (slot number)
// Returns: number of actions played, 0 if slot empty or invalid
int otto_play_memory_slot(int slot);

// Otto action constants
#define ACTION_DOG_WALK            1
#define ACTION_DOG_WALK_BACK       2
#define ACTION_DOG_TURN_LEFT       3
#define ACTION_DOG_TURN_RIGHT      4
#define ACTION_DOG_SIT_DOWN        5
#define ACTION_DOG_LIE_DOWN        6
#define ACTION_DOG_JUMP            7
#define ACTION_DOG_BOW             8
#define ACTION_DOG_DANCE           9
#define ACTION_DOG_WAVE_RIGHT_FOOT 10
#define ACTION_DOG_DANCE_4_FEET    11
#define ACTION_DOG_SWING           12
#define ACTION_DOG_STRETCH         13
#define ACTION_DOG_SCRATCH         14  // New: Sit + BR leg wave (gãi ngứa)
#define ACTION_DOG_WAG_TAIL        22  // New: Wag tail movement
#define ACTION_DOG_ROLL_OVER       23  // New: Roll over movement
#define ACTION_DOG_PLAY_DEAD       24  // New: Play dead movement

// New poses (Priority 1 + 2)
#define ACTION_DOG_SHAKE_PAW       25  // New: Shake paw (bắt tay)
#define ACTION_DOG_SIDESTEP        26  // New: Sidestep (đi ngang)
#define ACTION_DOG_PUSHUP          27  // New: Pushup exercise
#define ACTION_DOG_BALANCE         28  // New: Balance on hind legs
#define ACTION_DOG_TOILET          29  // New: Toilet squat pose
#define ACTION_DOG_STAND_UP        30  // New: Stand up from sitting/lying position

#define ACTION_WALK                15
#define ACTION_TURN                16
#define ACTION_JUMP                17
// Keep IDs aligned with otto_controller.cc's ActionType enum
#define ACTION_BEND                18
#define ACTION_HOME                19
// Utility
#define ACTION_DELAY               20  // speed field as milliseconds delay
#define ACTION_DOG_JUMP_HAPPY      21  // special: jump with happy emoji (touch)

// Otto control interface
void otto_execute_web_action(const char* action, int param1, int param2);

// Otto controller access
esp_err_t otto_controller_queue_action(int action_type, int steps, int speed, int direction, int amount);
esp_err_t otto_controller_stop_all(void);  // Stop and clear all actions

// Servo calibration control
void otto_controller_set_servo_angle(int servo_id, int angle);
void otto_controller_get_servo_angles(int* angles);
void otto_controller_apply_servo_home(int lf, int rf, int lb, int rb);

// Touch sensor control
void otto_set_touch_sensor_enabled(bool enabled);
bool otto_is_touch_sensor_enabled(void);

// UDP Drawing Service control
void otto_set_udp_draw_service(UdpDrawService* service);
void otto_set_drawing_display(DrawingDisplay* display);

// Music player handlers
esp_err_t otto_music_page_handler(httpd_req_t *req);
esp_err_t otto_music_play_handler(httpd_req_t *req);
esp_err_t otto_music_stop_handler(httpd_req_t *req);
esp_err_t otto_music_status_handler(httpd_req_t *req);
esp_err_t otto_music_pose_handler(httpd_req_t *req);

// Music player control (implemented in otto_robot.cc)
void otto_set_music_player(void* player);

#ifdef __cplusplus
}
#endif

#endif // OTTO_WEBSERVER_H
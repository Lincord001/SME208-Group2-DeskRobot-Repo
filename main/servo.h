#ifndef SERVO_H
#define SERVO_H

#include <stdint.h>

#include <esp_err.h>

typedef enum {
    SERVO_AXIS_HORIZONTAL = 0,
    SERVO_AXIS_VERTICAL,
} servo_axis_t;

esp_err_t servo_init(void);
esp_err_t servo_move_relative(servo_axis_t axis, int delta_deg);
esp_err_t servo_start_listening_motion(void);
void servo_stop_listening_motion(void);
esp_err_t servo_start_thinking_motion(void);
void servo_stop_thinking_motion(void);
esp_err_t servo_start_orbit_motion(void);
void servo_stop_orbit_motion(void);
esp_err_t servo_start_playback_motion(void);
void servo_stop_playback_motion(void);
esp_err_t servo_start_asr_motion(void);
void servo_stop_asr_motion(void);
void servo_center(void);
void servo_look_down(void);

#endif // SERVO_H

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

#endif // SERVO_H

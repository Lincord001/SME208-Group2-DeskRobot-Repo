#include "servo.h"

#include <stdbool.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define SERVO_PWM_FREQ_HZ        50
#define SERVO_PWM_PERIOD_US      20000
#define SERVO_PWM_RESOLUTION     LEDC_TIMER_13_BIT
#define SERVO_PWM_DUTY_STEPS     (1U << 13)
#define SERVO_PWM_MAX_DUTY       (SERVO_PWM_DUTY_STEPS - 1U)

#define SERVO_MIN_ANGLE_DEG      0
#define SERVO_HORIZONTAL_CENTER_ANGLE_DEG 100
#define SERVO_VERTICAL_CENTER_ANGLE_DEG   120
#define SERVO_MAX_ANGLE_DEG      180
#define SERVO_LOOK_DOWN_ANGLE_DEG 90

#define SERVO_MIN_PULSE_US       500
#define SERVO_CENTER_PULSE_US    1500
#define SERVO_MAX_PULSE_US       2500

#define SERVO_STEP_DEG           10
#define SERVO_SMOOTH_STEP_DEG    1
#define SERVO_STEP_DELAY_MS      40

#define SERVO_CMD_QUEUE_LEN      8
#define SERVO_TASK_STACK         4096
#define SERVO_TASK_PRIO          4
#define SERVO_MOTION_TASK_STACK  3072
#define SERVO_MOTION_TASK_PRIO   3
#define SERVO_ORBIT_STEP_MS      120
#define SERVO_ORBIT_RADIUS_DEG   6
#define SERVO_PLAYBACK_STEP_MS    160
#define SERVO_LISTENING_STEP_MS   260
#define SERVO_THINKING_STEP_MS    140
#define SERVO_ASR_STEP_MS         110

#define SERVO_HORIZONTAL_GPIO    GPIO_NUM_16
#define SERVO_VERTICAL_GPIO      GPIO_NUM_17

#define SERVO_ANGLE_DELTA_DEG    10
#define SERVO_AXIS_COUNT         2

typedef enum {
    SERVO_CMD_RELATIVE = 0,
    SERVO_CMD_ABSOLUTE,
} servo_command_type_t;

typedef enum {
    SERVO_MOTION_NONE = 0,
    SERVO_MOTION_LISTENING,
    SERVO_MOTION_THINKING,
    SERVO_MOTION_ORBIT,
    SERVO_MOTION_PLAYBACK,
    SERVO_MOTION_ASR,
} servo_motion_mode_t;

typedef struct {
    servo_command_type_t type;
    servo_axis_t axis;
    int value_deg;
} servo_command_t;

typedef struct {
    int horizontal;
    int vertical;
} servo_motion_point_t;

typedef struct {
    const char *name;
    gpio_num_t gpio_num;
    ledc_channel_t channel;
    int current_angle;
    int target_angle;
    bool enabled;
    bool suppress_angle_logs;
} servo_axis_state_t;

static const char *TAG = "SERVO";

static servo_axis_state_t s_axes[SERVO_AXIS_COUNT] = {
    [SERVO_AXIS_HORIZONTAL] = {
        .name = "horizontal",
        .gpio_num = SERVO_HORIZONTAL_GPIO,
        .channel = LEDC_CHANNEL_0,
        .current_angle = SERVO_HORIZONTAL_CENTER_ANGLE_DEG,
        .target_angle = SERVO_HORIZONTAL_CENTER_ANGLE_DEG,
        .enabled = false,
    },
    [SERVO_AXIS_VERTICAL] = {
        .name = "vertical",
        .gpio_num = SERVO_VERTICAL_GPIO,
        .channel = LEDC_CHANNEL_1,
        .current_angle = SERVO_VERTICAL_CENTER_ANGLE_DEG,
        .target_angle = SERVO_VERTICAL_CENTER_ANGLE_DEG,
        .enabled = false,
    },
};

static QueueHandle_t s_command_queue;
static TaskHandle_t s_task_handle;
static TaskHandle_t s_motion_task_handle;
static bool s_initialized;
static volatile bool s_motion_running;
static volatile servo_motion_mode_t s_motion_mode = SERVO_MOTION_NONE;

static const servo_motion_point_t s_orbit_points[] = {
    {+SERVO_ORBIT_RADIUS_DEG, 0},
    {+5, +3},
    {+3, +5},
    {0, +SERVO_ORBIT_RADIUS_DEG},
    {-3, +5},
    {-5, +3},
    {-SERVO_ORBIT_RADIUS_DEG, 0},
    {-5, -3},
    {-3, -5},
    {0, -SERVO_ORBIT_RADIUS_DEG},
    {+3, -5},
    {+5, -3},
};

static const servo_motion_point_t s_playback_points[] = {
    {0, -8},
    {0, -4},
    {0, 0},
    {0, +5},
    {0, 0},
    {0, -6},
    {0, 0},
    {0, +4},
};

static const servo_motion_point_t s_listening_points[] = {
    {-12, +2},
    {-7, +1},
    {0, 0},
    {+7, +1},
    {+12, +2},
    {+7, +1},
    {0, 0},
    {-7, +1},
};

static const servo_motion_point_t s_thinking_points[] = {
    {-4, -2},
    {-2, +3},
    {+3, +4},
    {+5, 0},
    {+2, -4},
    {-3, -3},
};

static const servo_motion_point_t s_asr_points[] = {
    {-10, -7},
    {-4, +6},
    {+2, -5},
    {+9, +7},
    {+4, -6},
    {-2, +5},
};

static uint32_t servo_angle_to_duty(int angle_deg)
{
    if (angle_deg < SERVO_MIN_ANGLE_DEG) {
        angle_deg = SERVO_MIN_ANGLE_DEG;
    } else if (angle_deg > SERVO_MAX_ANGLE_DEG) {
        angle_deg = SERVO_MAX_ANGLE_DEG;
    }

    uint32_t pulse_width_us = SERVO_MIN_PULSE_US +
        ((uint32_t)angle_deg * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) /
         SERVO_MAX_ANGLE_DEG);

    uint32_t duty = (pulse_width_us * SERVO_PWM_DUTY_STEPS +
                     (SERVO_PWM_PERIOD_US / 2U)) / SERVO_PWM_PERIOD_US;
    if (duty > SERVO_PWM_MAX_DUTY) {
        duty = SERVO_PWM_MAX_DUTY;
    }
    return duty;
}

static esp_err_t servo_apply_angle(servo_axis_state_t *axis, int angle_deg)
{
    if (!axis->enabled) {
        ESP_RETURN_ON_ERROR(
            ledc_set_duty(LEDC_LOW_SPEED_MODE, axis->channel, 0),
            TAG, "disable %s duty failed", axis->name);
        ESP_RETURN_ON_ERROR(
            ledc_update_duty(LEDC_LOW_SPEED_MODE, axis->channel),
            TAG, "disable %s update failed", axis->name);
        return ESP_OK;
    }

    uint32_t duty = servo_angle_to_duty(angle_deg);

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, axis->channel, duty),
        TAG, "set %s duty failed", axis->name);
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(LEDC_LOW_SPEED_MODE, axis->channel),
        TAG, "update %s duty failed", axis->name);

    axis->current_angle = angle_deg;
    if (!axis->suppress_angle_logs) {
        ESP_LOGI(TAG, "%s current=%d commanded=%d",
                 axis->name, axis->current_angle, axis->target_angle);
    }
    return ESP_OK;
}

static void servo_update_target(servo_axis_state_t *axis, int delta_deg)
{
    axis->suppress_angle_logs = false;

    int unclamped_target = axis->target_angle + delta_deg;
    int next_target = unclamped_target;
    if (next_target > SERVO_MAX_ANGLE_DEG) {
        next_target = SERVO_MAX_ANGLE_DEG;
    } else if (next_target < SERVO_MIN_ANGLE_DEG) {
        next_target = SERVO_MIN_ANGLE_DEG;
    }

    axis->target_angle = next_target;
    if (next_target != unclamped_target) {
        ESP_LOGW(TAG, "%s commanded angle clipped to %d deg",
                 axis->name, axis->target_angle);
    }
    ESP_LOGI(TAG, "%s move=%+d commanded=%d",
             axis->name, delta_deg, axis->target_angle);
}

static void servo_set_target(servo_axis_state_t *axis, int target_angle)
{
    axis->suppress_angle_logs = true;

    int next_target = target_angle;
    if (next_target > SERVO_MAX_ANGLE_DEG) {
        next_target = SERVO_MAX_ANGLE_DEG;
    } else if (next_target < SERVO_MIN_ANGLE_DEG) {
        next_target = SERVO_MIN_ANGLE_DEG;
    }

    if (!axis->enabled) {
        axis->enabled = true;
        axis->current_angle = next_target;
        if (!axis->suppress_angle_logs) {
            ESP_LOGI(TAG, "%s enabled at %d deg", axis->name, axis->current_angle);
        }
    }

    axis->target_angle = next_target;
}

static void servo_process_command(const servo_command_t *cmd)
{
    if (cmd == NULL || cmd->axis >= SERVO_AXIS_COUNT) {
        return;
    }

    servo_axis_state_t *axis = &s_axes[cmd->axis];
    if (cmd->type == SERVO_CMD_ABSOLUTE) {
        bool was_disabled = !axis->enabled;
        servo_set_target(axis, cmd->value_deg);
        if (was_disabled && axis->current_angle == axis->target_angle) {
            (void)servo_apply_angle(axis, axis->target_angle);
        }
        return;
    }

    if (!axis->enabled) {
        axis->enabled = true;
        axis->current_angle = axis->target_angle;
        ESP_LOGW(TAG, "%s enabled at %d deg, first press does not move",
                 axis->name, axis->current_angle);
        return;
    }

    servo_update_target(axis, cmd->value_deg);
}

static void servo_task(void *arg)
{
    (void)arg;

    while (1) {
        bool moving = false;
        for (size_t i = 0; i < SERVO_AXIS_COUNT; ++i) {
            if (s_axes[i].current_angle != s_axes[i].target_angle) {
                moving = true;
                break;
            }
        }

        servo_command_t cmd;
        TickType_t wait_ticks = moving ? pdMS_TO_TICKS(SERVO_STEP_DELAY_MS) : portMAX_DELAY;
        if (xQueueReceive(s_command_queue, &cmd, wait_ticks) == pdPASS) {
            servo_process_command(&cmd);
            continue;
        }

        if (!moving) {
            continue;
        }

        for (size_t i = 0; i < SERVO_AXIS_COUNT; ++i) {
            servo_axis_state_t *axis = &s_axes[i];

            if (axis->current_angle == axis->target_angle) {
                continue;
            }

            int next_angle = axis->current_angle;
            if (axis->current_angle < axis->target_angle) {
                next_angle += SERVO_SMOOTH_STEP_DEG;
                if (next_angle > axis->target_angle) {
                    next_angle = axis->target_angle;
                }
            } else {
                next_angle -= SERVO_SMOOTH_STEP_DEG;
                if (next_angle < axis->target_angle) {
                    next_angle = axis->target_angle;
                }
            }

            esp_err_t err = servo_apply_angle(axis, next_angle);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to move %s servo", axis->name);
            } else if (axis->current_angle == axis->target_angle) {
                axis->suppress_angle_logs = false;
            }
        }
    }
}

esp_err_t servo_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "timer config failed");

    for (size_t i = 0; i < SERVO_AXIS_COUNT; ++i) {
        ledc_channel_config_t channel_cfg = {
            .gpio_num = s_axes[i].gpio_num,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = s_axes[i].channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags.output_invert = 0,
        };
        ESP_RETURN_ON_ERROR(
            ledc_channel_config(&channel_cfg),
            TAG, "channel config failed for %s", s_axes[i].name);
    }

    s_command_queue = xQueueCreate(SERVO_CMD_QUEUE_LEN, sizeof(servo_command_t));
    if (s_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(servo_task, "servo_task", SERVO_TASK_STACK, NULL,
                    SERVO_TASK_PRIO, &s_task_handle) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        ESP_LOGE(TAG, "Failed to create servo task");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "Initialized with startup center, relative step=%d deg, step_delay=%d ms",
             SERVO_ANGLE_DELTA_DEG, SERVO_STEP_DELAY_MS);
    ESP_LOGI(TAG,
             "First press enables an axis, later presses move it relatively");
    servo_center();
    return ESP_OK;
}

esp_err_t servo_move_relative(servo_axis_t axis, int delta_deg)
{
    if (!s_initialized || s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    servo_command_t cmd;
    if (axis >= SERVO_AXIS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (delta_deg == 0) {
        return ESP_OK;
    }

    cmd.type = SERVO_CMD_RELATIVE;
    cmd.axis = axis;
    cmd.value_deg = delta_deg;

    if (xQueueSend(s_command_queue, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "Command queue full, dropped axis=%d delta=%d",
                 (int)axis, delta_deg);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t servo_queue_absolute(servo_axis_t axis, int angle_deg)
{
    if (!s_initialized || s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (axis >= SERVO_AXIS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    servo_command_t cmd = {
        .type = SERVO_CMD_ABSOLUTE,
        .axis = axis,
        .value_deg = angle_deg,
    };

    if (xQueueSend(s_command_queue, &cmd, 0) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static const char *servo_motion_mode_name(servo_motion_mode_t mode)
{
    switch (mode) {
    case SERVO_MOTION_LISTENING:
        return "listening";
    case SERVO_MOTION_THINKING:
        return "thinking";
    case SERVO_MOTION_ORBIT:
        return "orbit";
    case SERVO_MOTION_PLAYBACK:
        return "playback";
    case SERVO_MOTION_ASR:
        return "asr";
    case SERVO_MOTION_NONE:
    default:
        return "none";
    }
}

static const servo_motion_point_t *servo_motion_points(servo_motion_mode_t mode,
                                                       size_t *out_count,
                                                       uint32_t *out_step_ms)
{
    switch (mode) {
    case SERVO_MOTION_LISTENING:
        *out_count = sizeof(s_listening_points) / sizeof(s_listening_points[0]);
        *out_step_ms = SERVO_LISTENING_STEP_MS;
        return s_listening_points;

    case SERVO_MOTION_THINKING:
        *out_count = sizeof(s_thinking_points) / sizeof(s_thinking_points[0]);
        *out_step_ms = SERVO_THINKING_STEP_MS;
        return s_thinking_points;

    case SERVO_MOTION_PLAYBACK:
        *out_count = sizeof(s_playback_points) / sizeof(s_playback_points[0]);
        *out_step_ms = SERVO_PLAYBACK_STEP_MS;
        return s_playback_points;

    case SERVO_MOTION_ASR:
        *out_count = sizeof(s_asr_points) / sizeof(s_asr_points[0]);
        *out_step_ms = SERVO_ASR_STEP_MS;
        return s_asr_points;

    case SERVO_MOTION_ORBIT:
    default:
        *out_count = sizeof(s_orbit_points) / sizeof(s_orbit_points[0]);
        *out_step_ms = SERVO_ORBIT_STEP_MS;
        return s_orbit_points;
    }
}

static void servo_motion_task(void *arg)
{
    servo_motion_mode_t mode = (servo_motion_mode_t)(intptr_t)arg;
    size_t idx = 0;
    size_t point_count = 0;
    uint32_t step_ms = 0;
    const servo_motion_point_t *points = servo_motion_points(mode, &point_count, &step_ms);

    ESP_LOGI(TAG, "%s motion started", servo_motion_mode_name(mode));

    while (s_motion_running && s_motion_mode == mode) {
        const servo_motion_point_t *point = &points[idx];
        (void)servo_queue_absolute(SERVO_AXIS_HORIZONTAL,
                                   SERVO_HORIZONTAL_CENTER_ANGLE_DEG + point->horizontal);
        (void)servo_queue_absolute(SERVO_AXIS_VERTICAL,
                                   SERVO_VERTICAL_CENTER_ANGLE_DEG + point->vertical);

        idx = (idx + 1) % point_count;
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    (void)servo_queue_absolute(SERVO_AXIS_HORIZONTAL, SERVO_HORIZONTAL_CENTER_ANGLE_DEG);
    (void)servo_queue_absolute(SERVO_AXIS_VERTICAL, SERVO_VERTICAL_CENTER_ANGLE_DEG);

    ESP_LOGI(TAG, "%s motion stopped", servo_motion_mode_name(mode));
    s_motion_mode = SERVO_MOTION_NONE;
    s_motion_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t servo_start_motion(servo_motion_mode_t mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == SERVO_MOTION_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_motion_task_handle != NULL) {
        if (s_motion_mode == mode) {
            return ESP_OK;
        }
        s_motion_running = false;
        for (int i = 0; i < 10 && s_motion_task_handle != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (s_motion_task_handle != NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_motion_mode = mode;
    s_motion_running = true;
    if (xTaskCreate(servo_motion_task,
                    "servo_motion",
                    SERVO_MOTION_TASK_STACK,
                    (void *)(intptr_t)mode,
                    SERVO_MOTION_TASK_PRIO,
                    &s_motion_task_handle) != pdPASS) {
        s_motion_task_handle = NULL;
        s_motion_mode = SERVO_MOTION_NONE;
        s_motion_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void servo_stop_motion(servo_motion_mode_t mode)
{
    if (mode == SERVO_MOTION_NONE || s_motion_mode == mode) {
        s_motion_running = false;
    }
}

esp_err_t servo_start_orbit_motion(void)
{
    return servo_start_motion(SERVO_MOTION_ORBIT);
}

void servo_stop_orbit_motion(void)
{
    servo_stop_motion(SERVO_MOTION_ORBIT);
}

esp_err_t servo_start_listening_motion(void)
{
    return servo_start_motion(SERVO_MOTION_LISTENING);
}

void servo_stop_listening_motion(void)
{
    servo_stop_motion(SERVO_MOTION_LISTENING);
}

esp_err_t servo_start_thinking_motion(void)
{
    return servo_start_motion(SERVO_MOTION_THINKING);
}

void servo_stop_thinking_motion(void)
{
    servo_stop_motion(SERVO_MOTION_THINKING);
}

esp_err_t servo_start_playback_motion(void)
{
    return servo_start_motion(SERVO_MOTION_PLAYBACK);
}

void servo_stop_playback_motion(void)
{
    servo_stop_motion(SERVO_MOTION_PLAYBACK);
}

esp_err_t servo_start_asr_motion(void)
{
    return servo_start_motion(SERVO_MOTION_ASR);
}

void servo_stop_asr_motion(void)
{
    servo_stop_motion(SERVO_MOTION_ASR);
}

void servo_center(void)
{
    servo_stop_motion(SERVO_MOTION_NONE);
    (void)servo_queue_absolute(SERVO_AXIS_HORIZONTAL, SERVO_HORIZONTAL_CENTER_ANGLE_DEG);
    (void)servo_queue_absolute(SERVO_AXIS_VERTICAL, SERVO_VERTICAL_CENTER_ANGLE_DEG);
}

void servo_look_down(void)
{
    servo_stop_motion(SERVO_MOTION_NONE);
    (void)servo_queue_absolute(SERVO_AXIS_HORIZONTAL, SERVO_HORIZONTAL_CENTER_ANGLE_DEG);
    (void)servo_queue_absolute(SERVO_AXIS_VERTICAL, SERVO_LOOK_DOWN_ANGLE_DEG);
}

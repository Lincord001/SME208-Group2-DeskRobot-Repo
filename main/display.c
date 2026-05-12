#include "display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_idf_version.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

#define DISPLAY_I2C_SDA_PIN      GPIO_NUM_3
#define DISPLAY_I2C_SCL_PIN      GPIO_NUM_8
#define DISPLAY_I2C_PORT         I2C_NUM_0
#define DISPLAY_I2C_HW_ADDR      0x3D
#define DISPLAY_I2C_ALT_HW_ADDR  0x3C
#define DISPLAY_I2C_CLK_HZ       (400 * 1000)
#define DISPLAY_I2C_PROBE_TIMEOUT_MS 100

#define DISPLAY_H_RES            128
#define DISPLAY_V_RES            64
#define DISPLAY_LCD_CMD_BITS     8
#define DISPLAY_LCD_PARAM_BITS   8

#define DISPLAY_TASK_STACK_SIZE  4096
#define DISPLAY_TASK_PRIORITY    4
#define DISPLAY_UPDATE_MS        250
#define DISPLAY_PANEL_OFF_UPDATE_MS 1000
#define DISPLAY_ROTATE_180       1
#define DISPLAY_WIFI_CONNECTED_HOLD_SEC 5

#define SSD1306_CMD_SEG_REMAP_NORMAL   0xA0
#define SSD1306_CMD_SEG_REMAP_REVERSE  0xA1
#define SSD1306_CMD_COM_SCAN_NORMAL    0xC0
#define SSD1306_CMD_COM_SCAN_REVERSE   0xC8

static const char *TAG = "display";

typedef struct {
    display_state_t state;
    uint32_t elapsed_sec;
    uint32_t total_sec;
    TickType_t state_start_tick;
    char wifi_status[32];
    char status_title[24];
    char status_detail[32];
} display_status_t;

static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_task_handle;
static bool s_initialized;
static bool s_task_started;
static volatile bool s_low_power_overlay;
static volatile bool s_panel_powered = true;

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lvgl_disp;
static lv_obj_t *s_title_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_meter_bg;
static lv_obj_t *s_meter_fill;
static lv_obj_t *s_eye;
static lv_obj_t *s_eye_glint;
static lv_obj_t *s_dots[3];
static uint8_t s_i2c_hw_addr = DISPLAY_I2C_HW_ADDR;

static const audio_buffer_t *s_audio_buf;
static uint32_t s_audio_sample_rate_hz;
static display_status_t s_status = {
    .state = DISPLAY_STATE_IDLE,
};

static void display_set_visible(lv_obj_t *obj, bool visible);
static void display_hide_motion_objects(void);

static uint32_t display_elapsed_from_start(const display_status_t *status)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t diff = now - status->state_start_tick;
    return status->elapsed_sec + (uint32_t)(diff / configTICK_RATE_HZ);
}

static void display_copy_status(display_status_t *out)
{
    if (s_state_mutex != NULL && xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_state_mutex);
    }
}

static void display_store_status(display_state_t state,
                                 uint32_t elapsed_sec,
                                 uint32_t total_sec)
{
    if (s_state_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        if (state != DISPLAY_STATE_IDLE) {
            s_low_power_overlay = false;
            display_set_panel_power(true);
        }
        s_status.state = state;
        s_status.elapsed_sec = elapsed_sec;
        s_status.total_sec = total_sec;
        s_status.state_start_tick = xTaskGetTickCount();
        xSemaphoreGive(s_state_mutex);
    }
}

static void display_render_low_power(uint32_t frame)
{
    if (!s_panel_powered || s_title_label == NULL || s_detail_label == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_title_label, "");
        lv_label_set_text(s_detail_label, "");
        display_hide_motion_objects();

        uint32_t cycle = frame % 28U;
        bool burst = cycle >= 23U;
        int x = 10 + (int)((cycle * 88U) / 27U);
        int y = 48 - (int)((cycle * 34U) / 27U);

        if (!burst) {
            const char *text = "z";
            if (cycle > 8U) {
                text = "Z";
            }
            if (cycle > 16U) {
                text = "ZZ";
            }

            int size = 10 + (int)(cycle / 2U);
            lv_label_set_text(s_status_label, text);
            display_set_visible(s_status_label, true);
            lv_obj_set_pos(s_status_label, x, y);

            display_set_visible(s_eye, true);
            lv_obj_set_pos(s_eye, x - 3, y - 3);
            lv_obj_set_size(s_eye, size, size);
        } else {
            lv_label_set_text(s_status_label, "");
            display_set_visible(s_status_label, false);
            for (size_t i = 0; i < 3; ++i) {
                display_set_visible(s_dots[i], true);
                lv_obj_set_pos(s_dots[i], 94 + (int)i * 8, 13 + ((i == 1) ? -4 : 4));
                lv_obj_set_size(s_dots[i], 3 + (int)i, 3 + (int)i);
            }
        }

        lvgl_port_unlock();
    }
}

static uint32_t display_audio_total_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    audio_buffer_state_t audio_state;
    audio_buffer_get_state(s_audio_buf, &audio_state);
    uint32_t total_samples = (uint32_t)(audio_state.recorded_size / sizeof(int16_t));
    if (total_samples == 0) {
        total_samples = (uint32_t)audio_state.current_write_pos;
    }

    return total_samples / s_audio_sample_rate_hz;
}

static uint32_t display_audio_read_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    audio_buffer_state_t audio_state;
    audio_buffer_get_state(s_audio_buf, &audio_state);
    return (uint32_t)(audio_state.current_read_pos / s_audio_sample_rate_hz);
}

static uint32_t display_audio_write_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    audio_buffer_state_t audio_state;
    audio_buffer_get_state(s_audio_buf, &audio_state);
    return (uint32_t)(audio_state.current_write_pos / s_audio_sample_rate_hz);
}

static void display_format_text(const display_status_t *status,
                                char *title,
                                size_t title_size,
                                char *detail,
                                size_t detail_size)
{
    uint32_t elapsed = display_elapsed_from_start(status);
    uint32_t total = status->total_sec;

    switch (status->state) {
    case DISPLAY_STATE_LISTENING:
        if (s_audio_buf != NULL) {
            audio_buffer_state_t audio_state;
            audio_buffer_get_state(s_audio_buf, &audio_state);
            if (audio_state.recording) {
                elapsed = display_audio_write_sec();
            }
        }
        snprintf(title, title_size, "Listening");
        snprintf(detail, detail_size, "%lu sec", (unsigned long)elapsed);
        break;

    case DISPLAY_STATE_THINKING:
        snprintf(title, title_size, "Thinking");
        snprintf(detail, detail_size, "%lu sec", (unsigned long)elapsed);
        break;

    case DISPLAY_STATE_ANSWERING:
        if (s_audio_buf != NULL) {
            elapsed = display_audio_read_sec();
            total = display_audio_total_sec();
        }
        snprintf(title, title_size, "Answering");
        if (total > 0) {
            snprintf(detail, detail_size, "%lu / %lu sec",
                     (unsigned long)elapsed, (unsigned long)total);
        } else {
            snprintf(detail, detail_size, "%lu sec", (unsigned long)elapsed);
        }
        break;

    case DISPLAY_STATE_WIFI:
        title[0] = '\0';
        snprintf(detail, detail_size, "%s",
                 status->wifi_status[0] != '\0' ? status->wifi_status : "init");
        break;

    case DISPLAY_STATE_STATUS:
        snprintf(title, title_size, "%s",
                 status->status_title[0] != '\0' ? status->status_title : "Status");
        snprintf(detail, detail_size, "%s",
                 status->status_detail[0] != '\0' ? status->status_detail : "");
        break;

    case DISPLAY_STATE_CLOCK: {
        time_t now;
        struct tm timeinfo;

        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year < (2023 - 1900)) {
            snprintf(title, title_size, "--:--:--");
            snprintf(detail, detail_size, "Syncing time");
        } else {
            snprintf(title, title_size, "%02d:%02d:%02d",
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            snprintf(detail, detail_size, "%04d-%02d-%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        }
        break;
    }

    case DISPLAY_STATE_ERROR:
        snprintf(title, title_size, "Error");
        snprintf(detail, detail_size, "Check logs");
        break;

    case DISPLAY_STATE_IDLE:
    default:
        snprintf(title, title_size, "Ready");
        snprintf(detail, detail_size, "Ask me anything");
        break;
    }
}

static void display_set_box_style(lv_obj_t *obj, bool filled, int radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(obj, filled ? 0 : 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void display_set_visible(lv_obj_t *obj, bool visible)
{
    if (obj == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_set_meter(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    display_set_visible(s_meter_bg, true);
    display_set_visible(s_meter_fill, true);
    lv_obj_set_pos(s_meter_bg, 16, 42);
    lv_obj_set_size(s_meter_bg, 96, 8);
    lv_obj_set_pos(s_meter_fill, 18, 44);
    lv_obj_set_size(s_meter_fill, (92 * percent) / 100, 4);
}

static void display_hide_motion_objects(void)
{
    display_set_visible(s_meter_bg, false);
    display_set_visible(s_meter_fill, false);
    display_set_visible(s_eye, false);
    display_set_visible(s_eye_glint, false);
    for (size_t i = 0; i < 3; ++i) {
        display_set_visible(s_dots[i], false);
    }
}

static uint8_t display_triangle_wave(uint32_t phase, uint32_t period, uint8_t max_value)
{
    uint32_t pos = phase % period;
    uint32_t half = period / 2;
    if (pos < half) {
        return (uint8_t)((pos * max_value) / half);
    }
    return (uint8_t)(((period - pos) * max_value) / half);
}

static void display_render_status(const char *title,
                                  const char *detail,
                                  const display_status_t *status,
                                  uint32_t elapsed,
                                  uint32_t total,
                                  uint32_t frame)
{
    if (s_title_label == NULL || s_detail_label == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_obj_set_style_text_font(s_title_label, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_font(s_detail_label, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_font(s_status_label, LV_FONT_DEFAULT, 0);
        lv_label_set_text(s_title_label, title);
        lv_label_set_text(s_detail_label, detail);
        lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 3);
        lv_obj_align(s_detail_label, LV_ALIGN_BOTTOM_MID, 0, -3);
        display_hide_motion_objects();

        if (status->state == DISPLAY_STATE_IDLE) {
            uint8_t eye_w = 26 + display_triangle_wave(frame, 12, 22);
            int eye_x = 64 - (eye_w / 2);
            int glint_x = eye_x + 6 + (int)display_triangle_wave(frame + 3, 10, 12);

            lv_label_set_text(s_status_label, "");
            display_set_visible(s_status_label, false);
            display_set_visible(s_eye, true);
            display_set_visible(s_eye_glint, true);
            lv_obj_set_pos(s_eye, eye_x, 27);
            lv_obj_set_size(s_eye, eye_w, 13);
            lv_obj_set_pos(s_eye_glint, glint_x, 31);
            lv_obj_set_size(s_eye_glint, 5, 5);
        } else if (status->state == DISPLAY_STATE_LISTENING) {
            uint8_t fill = (uint8_t)((elapsed * 100U) / 20U);
            if (fill > 100) {
                fill = 100;
            }

            lv_label_set_text(s_status_label, "REC");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -14);
            display_set_meter(fill);

            for (size_t i = 0; i < 3; ++i) {
                uint8_t height = 7 + display_triangle_wave(frame + (uint32_t)i * 2, 8, 15);
                display_set_visible(s_dots[i], true);
                lv_obj_set_pos(s_dots[i], 46 + (int)i * 16, 32 - (height / 2));
                lv_obj_set_size(s_dots[i], 7, height);
            }
        } else if (status->state == DISPLAY_STATE_THINKING) {
            lv_label_set_text(s_status_label, "AI");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -15);

            for (size_t i = 0; i < 3; ++i) {
                bool active = ((frame / 2U) % 3U) == i;
                display_set_visible(s_dots[i], true);
                lv_obj_set_pos(s_dots[i], 48 + (int)i * 14, active ? 29 : 33);
                lv_obj_set_size(s_dots[i], active ? 9 : 6, active ? 9 : 6);
            }
        } else if (status->state == DISPLAY_STATE_ANSWERING) {
            uint8_t fill = 0;
            if (total > 0) {
                fill = (uint8_t)((elapsed * 100U) / total);
                if (fill > 100) {
                    fill = 100;
                }
            } else {
                fill = display_triangle_wave(frame, 16, 100);
            }
            lv_label_set_text(s_status_label, "PLAY");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -14);
            display_set_meter(fill);
        } else if (status->state == DISPLAY_STATE_WIFI) {
            lv_label_set_text(s_status_label, "WIFI");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -16);
            for (size_t i = 0; i < 3; ++i) {
                bool active = ((frame / 2U) % 4U) > i;
                display_set_visible(s_dots[i], true);
                lv_obj_set_pos(s_dots[i], 50 + (int)i * 12, 35 - (int)i * 4);
                lv_obj_set_size(s_dots[i], active ? 8 : 5, active ? 8 : 5);
            }
        } else if (status->state == DISPLAY_STATE_STATUS) {
            bool complete = strcmp(status->status_title, "Recorded") == 0 ||
                            strcmp(status->status_title, "Recognized") == 0 ||
                            strcmp(status->status_title, "Voice ready") == 0;
            bool system_page = strncmp(status->status_title, "WiFi", 4) == 0 ||
                               strncmp(status->status_title, "Heap", 4) == 0 ||
                               strncmp(status->status_title, "ASR", 3) == 0 ||
                               strncmp(status->status_title, "LLM", 3) == 0 ||
                               strncmp(status->status_title, "TTS", 3) == 0;
            lv_label_set_text(s_status_label, complete ? "DONE" : (system_page ? "INFO" : "WORK"));
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -14);
            if (complete) {
                display_set_meter(100);
            } else if (system_page) {
                display_set_visible(s_meter_bg, true);
                display_set_visible(s_meter_fill, false);
                lv_obj_set_pos(s_meter_bg, 18, 42);
                lv_obj_set_size(s_meter_bg, 92, 8);
                for (size_t i = 0; i < 3; ++i) {
                    display_set_visible(s_dots[i], true);
                    lv_obj_set_pos(s_dots[i], 43 + (int)i * 18, 32);
                    lv_obj_set_size(s_dots[i], 8, 8);
                }
            } else {
                for (size_t i = 0; i < 3; ++i) {
                    bool active = ((frame / 2U) % 3U) == i;
                    display_set_visible(s_dots[i], true);
                    lv_obj_set_pos(s_dots[i], 48 + (int)i * 14, active ? 31 : 35);
                    lv_obj_set_size(s_dots[i], active ? 8 : 5, active ? 8 : 5);
                }
            }
        } else if (status->state == DISPLAY_STATE_CLOCK) {
            time_t now;
            struct tm timeinfo;
            uint8_t second_progress = 0;

            time(&now);
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_year >= (2023 - 1900)) {
                second_progress = (uint8_t)((timeinfo.tm_sec * 100U) / 59U);
            } else {
                second_progress = display_triangle_wave(frame, 24, 100);
            }

            lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
            lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, 4);
            lv_obj_align(s_detail_label, LV_ALIGN_BOTTOM_MID, 0, -2);

            lv_label_set_text(s_status_label, "CLOCK");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 1);

            display_set_visible(s_meter_bg, true);
            display_set_visible(s_meter_fill, true);
            lv_obj_set_pos(s_meter_bg, 8, 20);
            lv_obj_set_size(s_meter_bg, 112, 30);
            lv_obj_set_pos(s_meter_fill, 14, 46);
            lv_obj_set_size(s_meter_fill, (100 * second_progress) / 100, 3);
        } else if (status->state == DISPLAY_STATE_ERROR) {
            lv_label_set_text(s_status_label, "!");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -12);
            display_set_meter((frame % 2U) ? 100 : 12);
        } else {
            lv_label_set_text(s_status_label, "OK");
            display_set_visible(s_status_label, true);
            lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -14);
            display_set_meter(100);
        }

        lvgl_port_unlock();
    }
}

static void display_task(void *arg)
{
    (void)arg;

    char title[24];
    char detail[32];
    display_status_t status;
    uint32_t frame = 0;

    while (1) {
        display_copy_status(&status);

        if (status.state == DISPLAY_STATE_WIFI &&
            strcmp(status.wifi_status, "connected") == 0 &&
            display_elapsed_from_start(&status) >= DISPLAY_WIFI_CONNECTED_HOLD_SEC) {
            display_set_idle();
            display_copy_status(&status);
        }

        if (s_audio_buf != NULL) {
            audio_buffer_state_t audio_state;
            audio_buffer_get_state(s_audio_buf, &audio_state);
            if (status.state == DISPLAY_STATE_LISTENING && !audio_state.recording) {
                display_set_thinking(0);
                display_copy_status(&status);
            } else if (status.state == DISPLAY_STATE_ANSWERING && !audio_state.playing) {
                display_set_idle();
                display_copy_status(&status);
            }
        }

        if (s_low_power_overlay) {
            if (status.state == DISPLAY_STATE_IDLE) {
                display_render_low_power(frame);
            } else {
                s_low_power_overlay = false;
                display_set_panel_power(true);
            }
            frame++;
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
            continue;
        }

        if (!s_panel_powered) {
            frame++;
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_PANEL_OFF_UPDATE_MS));
            continue;
        }

        display_format_text(&status, title, sizeof(title), detail, sizeof(detail));

        uint32_t elapsed = display_elapsed_from_start(&status);
        uint32_t total = status.total_sec;
        if (status.state == DISPLAY_STATE_LISTENING && s_audio_buf != NULL) {
            elapsed = display_audio_write_sec();
        } else if (status.state == DISPLAY_STATE_ANSWERING && s_audio_buf != NULL) {
            elapsed = display_audio_read_sec();
            total = display_audio_total_sec();
        }

        display_render_status(title, detail, &status, elapsed, total, frame);

        frame++;
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

static esp_err_t display_create_lvgl_labels(void)
{
    ESP_RETURN_ON_FALSE(s_lvgl_disp != NULL, ESP_ERR_INVALID_STATE, TAG, "LVGL display is NULL");

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_display_set_rotation(s_lvgl_disp,
                            DISPLAY_ROTATE_180 ? LV_DISPLAY_ROTATION_180
                                               : LV_DISPLAY_ROTATION_0);

    lv_obj_t *screen = lv_display_get_screen_active(s_lvgl_disp);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    s_title_label = lv_label_create(screen);
    s_detail_label = lv_label_create(screen);
    s_status_label = lv_label_create(screen);
    s_meter_bg = lv_obj_create(screen);
    s_meter_fill = lv_obj_create(screen);
    s_eye = lv_obj_create(screen);
    s_eye_glint = lv_obj_create(screen);
    for (size_t i = 0; i < 3; ++i) {
        s_dots[i] = lv_obj_create(screen);
    }

    if (s_title_label == NULL || s_detail_label == NULL || s_status_label == NULL ||
        s_meter_bg == NULL || s_meter_fill == NULL || s_eye == NULL || s_eye_glint == NULL ||
        s_dots[0] == NULL || s_dots[1] == NULL || s_dots[2] == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_width(s_title_label, DISPLAY_H_RES);
    lv_obj_set_width(s_detail_label, DISPLAY_H_RES);
    lv_obj_set_width(s_status_label, DISPLAY_H_RES);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);

    display_set_box_style(s_meter_bg, false, 3);
    display_set_box_style(s_meter_fill, true, 2);
    display_set_box_style(s_eye, false, LV_RADIUS_CIRCLE);
    display_set_box_style(s_eye_glint, true, LV_RADIUS_CIRCLE);
    for (size_t i = 0; i < 3; ++i) {
        display_set_box_style(s_dots[i], true, LV_RADIUS_CIRCLE);
    }

    lv_label_set_text(s_title_label, "Ready");
    lv_label_set_text(s_detail_label, "Ask me anything");
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_align(s_detail_label, LV_ALIGN_BOTTOM_MID, 0, -3);
    display_hide_motion_objects();

    lvgl_port_unlock();
    return ESP_OK;
}

static esp_err_t display_probe_i2c_address(void)
{
    const uint8_t addrs[] = {
        DISPLAY_I2C_HW_ADDR,
        DISPLAY_I2C_ALT_HW_ADDR,
    };

    for (size_t i = 0; i < sizeof(addrs); ++i) {
        esp_err_t err = i2c_master_probe(s_i2c_bus, addrs[i],
                                         DISPLAY_I2C_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            s_i2c_hw_addr = addrs[i];
            ESP_LOGI(TAG, "Found OLED at I2C address 0x%02X", s_i2c_hw_addr);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "No OLED response at I2C address 0x%02X: %s",
                 addrs[i], esp_err_to_name(err));
    }

    ESP_LOGE(TAG, "OLED not found. Check SDA=%d, SCL=%d, VCC, GND, and pull-ups",
             DISPLAY_I2C_SDA_PIN, DISPLAY_I2C_SCL_PIN);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t display_apply_orientation(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io,
                                                  SSD1306_CMD_SEG_REMAP_NORMAL,
                                                  NULL, 0),
                        TAG, "SSD1306 segment remap normal failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io,
                                                  SSD1306_CMD_COM_SCAN_NORMAL,
                                                  NULL, 0),
                        TAG, "SSD1306 COM scan normal failed");
    return ESP_OK;
}

esp_err_t display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = DISPLAY_I2C_PORT,
        .sda_io_num = DISPLAY_I2C_SDA_PIN,
        .scl_io_num = DISPLAY_I2C_SCL_PIN,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "I2C bus init failed");

    ESP_RETURN_ON_ERROR(display_probe_i2c_address(), TAG, "I2C OLED probe failed");

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = s_i2c_hw_addr,
        .scl_speed_hz = DISPLAY_I2C_CLK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = DISPLAY_LCD_CMD_BITS,
        .lcd_param_bits = DISPLAY_LCD_PARAM_BITS,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &s_panel_io),
                        TAG, "panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
#if (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
        .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
#endif
    };

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0))
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = DISPLAY_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_panel_io, &panel_config, &s_panel),
                        TAG, "SSD1306 panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel driver init failed");
    ESP_RETURN_ON_ERROR(display_apply_orientation(), TAG, "panel orientation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");
    s_panel_powered = true;

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = DISPLAY_H_RES * DISPLAY_V_RES,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .monochrome = true,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .sw_rotate = DISPLAY_ROTATE_180,
        },
    };

    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_lvgl_disp != NULL, ESP_FAIL, TAG, "LVGL display add failed");

    ESP_RETURN_ON_ERROR(display_create_lvgl_labels(), TAG, "create labels failed");

    s_initialized = true;
    display_set_idle();
    ESP_LOGI(TAG, "Initialized SSD1306 OLED on SDA=%d SCL=%d addr=0x%02X",
             DISPLAY_I2C_SDA_PIN, DISPLAY_I2C_SCL_PIN, s_i2c_hw_addr);
    return ESP_OK;
}

esp_err_t display_start_task(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_started) {
        return ESP_OK;
    }

    if (xTaskCreate(display_task, "display_task", DISPLAY_TASK_STACK_SIZE,
                    NULL, DISPLAY_TASK_PRIORITY, &s_task_handle) != pdPASS) {
        return ESP_FAIL;
    }

    s_task_started = true;
    return ESP_OK;
}

void display_attach_audio_buffer(const audio_buffer_t *audio_buffer,
                                 uint32_t sample_rate_hz)
{
    s_audio_buf = audio_buffer;
    s_audio_sample_rate_hz = sample_rate_hz;
}

void display_update_state(display_state_t state,
                          uint32_t elapsed_sec,
                          uint32_t total_sec)
{
    if (!s_initialized) {
        return;
    }

    display_store_status(state, elapsed_sec, total_sec);
}

void display_set_idle(void)
{
    display_update_state(DISPLAY_STATE_IDLE, 0, 0);
}

void display_set_listening(uint32_t sec)
{
    display_update_state(DISPLAY_STATE_LISTENING, sec, 0);
}

void display_set_thinking(uint32_t sec)
{
    display_update_state(DISPLAY_STATE_THINKING, sec, 0);
}

void display_set_answering(uint32_t current_sec, uint32_t total_sec)
{
    display_update_state(DISPLAY_STATE_ANSWERING, current_sec, total_sec);
}

void display_set_status(const char *title, const char *detail)
{
    if (!s_initialized || title == NULL) {
        return;
    }

    if (s_state_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_status.state == DISPLAY_STATE_IDLE ||
            s_status.state == DISPLAY_STATE_LISTENING ||
            s_status.state == DISPLAY_STATE_WIFI ||
            s_status.state == DISPLAY_STATE_STATUS ||
            s_status.state == DISPLAY_STATE_THINKING ||
            s_status.state == DISPLAY_STATE_ERROR) {
            s_status.state = DISPLAY_STATE_STATUS;
            s_status.elapsed_sec = 0;
            s_status.total_sec = 0;
            s_status.state_start_tick = xTaskGetTickCount();
            snprintf(s_status.status_title, sizeof(s_status.status_title),
                     "%s", title);
            snprintf(s_status.status_detail, sizeof(s_status.status_detail),
                     "%s", detail != NULL ? detail : "");
        }
        xSemaphoreGive(s_state_mutex);
    }
}

void display_set_wifi_status(const char *status)
{
    if (!s_initialized || status == NULL) {
        return;
    }

    if (s_state_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_status.state == DISPLAY_STATE_IDLE ||
            s_status.state == DISPLAY_STATE_WIFI ||
            s_status.state == DISPLAY_STATE_ERROR) {
            s_status.state = DISPLAY_STATE_WIFI;
            s_status.elapsed_sec = 0;
            s_status.total_sec = 0;
            s_status.state_start_tick = xTaskGetTickCount();
            snprintf(s_status.wifi_status, sizeof(s_status.wifi_status),
                     "%s", status);
        }
        xSemaphoreGive(s_state_mutex);
    }
}

void display_set_clock_mode(bool enable)
{
    if (!s_initialized) {
        return;
    }

    if (enable) {
        display_update_state(DISPLAY_STATE_CLOCK, 0, 0);
    } else if (display_get_state() == DISPLAY_STATE_CLOCK) {
        display_set_idle();
    }
}

display_state_t display_get_state(void)
{
    display_status_t status = {
        .state = DISPLAY_STATE_IDLE,
    };
    display_copy_status(&status);
    return status.state;
}

void display_set_low_power_overlay(bool enable)
{
    if (!s_initialized) {
        return;
    }

    if (enable) {
        if (display_get_state() != DISPLAY_STATE_IDLE) {
            return;
        }
        display_set_panel_power(true);
    }
    s_low_power_overlay = enable;
}

void display_set_panel_power(bool enable)
{
    if (!s_initialized || s_panel == NULL) {
        return;
    }
    if (s_panel_powered == enable) {
        return;
    }

    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, enable);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display power %s failed: %s",
                 enable ? "on" : "off", esp_err_to_name(err));
        return;
    }
    s_panel_powered = enable;
}

void display_set_error(void)
{
    display_update_state(DISPLAY_STATE_ERROR, 0, 0);
}

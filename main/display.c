#include "display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
#define DISPLAY_ROTATE_180       1

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
} display_status_t;

static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_task_handle;
static bool s_initialized;
static bool s_task_started;

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lvgl_disp;
static lv_obj_t *s_title_label;
static lv_obj_t *s_detail_label;
static uint8_t s_i2c_hw_addr = DISPLAY_I2C_HW_ADDR;

static const audio_buffer_t *s_audio_buf;
static uint32_t s_audio_sample_rate_hz;
static display_status_t s_status = {
    .state = DISPLAY_STATE_IDLE,
};

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
        s_status.state = state;
        s_status.elapsed_sec = elapsed_sec;
        s_status.total_sec = total_sec;
        s_status.state_start_tick = xTaskGetTickCount();
        xSemaphoreGive(s_state_mutex);
    }
}

static uint32_t display_audio_total_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    uint32_t total_samples = (uint32_t)(s_audio_buf->recorded_size / sizeof(int16_t));
    if (total_samples == 0) {
        total_samples = (uint32_t)s_audio_buf->current_write_pos;
    }

    return total_samples / s_audio_sample_rate_hz;
}

static uint32_t display_audio_read_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    return (uint32_t)(s_audio_buf->current_read_pos / s_audio_sample_rate_hz);
}

static uint32_t display_audio_write_sec(void)
{
    if (s_audio_buf == NULL || s_audio_sample_rate_hz == 0) {
        return 0;
    }

    return (uint32_t)(s_audio_buf->current_write_pos / s_audio_sample_rate_hz);
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
        if (s_audio_buf != NULL && s_audio_buf->recording) {
            elapsed = display_audio_write_sec();
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
        snprintf(title, title_size, "WiFi");
        snprintf(detail, detail_size, "%s",
                 status->wifi_status[0] != '\0' ? status->wifi_status : "init");
        break;

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

static void display_render_text(const char *title, const char *detail)
{
    if (s_title_label == NULL || s_detail_label == NULL) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_title_label, title);
        lv_label_set_text(s_detail_label, detail);
        lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 8);
        lvgl_port_unlock();
    }
}

static void display_task(void *arg)
{
    (void)arg;

    char title[24];
    char detail[32];
    display_status_t status;
    display_state_t last_state = DISPLAY_STATE_ERROR;
    uint32_t last_elapsed = UINT32_MAX;
    uint32_t last_total = UINT32_MAX;

    while (1) {
        display_copy_status(&status);

        if (s_audio_buf != NULL) {
            if (status.state == DISPLAY_STATE_LISTENING && !s_audio_buf->recording) {
                display_set_thinking(0);
                display_copy_status(&status);
            } else if (status.state == DISPLAY_STATE_ANSWERING && !s_audio_buf->playing) {
                display_set_idle();
                display_copy_status(&status);
            }
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

        if (status.state != last_state || elapsed != last_elapsed || total != last_total) {
            display_render_text(title, detail);
            last_state = status.state;
            last_elapsed = elapsed;
            last_total = total;
        }

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
    if (s_title_label == NULL || s_detail_label == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_width(s_title_label, DISPLAY_H_RES);
    lv_obj_set_width(s_detail_label, DISPLAY_H_RES);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_detail_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_label_set_text(s_title_label, "Ready");
    lv_label_set_text(s_detail_label, "Ask me anything");
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 8);

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

void display_set_error(void)
{
    display_update_state(DISPLAY_STATE_ERROR, 0, 0);
}

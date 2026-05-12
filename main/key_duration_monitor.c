#include "key_duration_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_button.h>

#define KEY_DURATION_MONITOR_MAX_KEYS 8

typedef struct {
    uint8_t key_id;
    TickType_t press_down_tick;
    uint32_t press_seq;
    bool pressed;
} key_duration_monitor_ctx_t;

static const char *TAG = "KEY_DURATION";

static key_duration_monitor_ctx_t s_key_contexts[KEY_DURATION_MONITOR_MAX_KEYS];
static size_t s_key_context_count;

static uint32_t key_duration_monitor_ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

static void key_duration_monitor_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;

    key_duration_monitor_ctx_t *ctx = (key_duration_monitor_ctx_t *)usr_data;
    if (ctx == NULL) {
        return;
    }

    ctx->press_down_tick = xTaskGetTickCount();
    ctx->press_seq++;
    ctx->pressed = true;

    ESP_LOGI(TAG, "K%u down seq=%lu tick=%lu",
             ctx->key_id,
             (unsigned long)ctx->press_seq,
             (unsigned long)ctx->press_down_tick);
}

static void key_duration_monitor_up_cb(void *button_handle, void *usr_data)
{
    key_duration_monitor_ctx_t *ctx = (key_duration_monitor_ctx_t *)usr_data;
    if (ctx == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t held_ms = ctx->pressed
                           ? key_duration_monitor_ticks_to_ms(now - ctx->press_down_tick)
                           : 0;
    uint32_t library_pressed_ms = iot_button_get_pressed_time((button_handle_t)button_handle);

    ESP_LOGI(TAG, "K%u up seq=%lu held=%lu ms library_pressed=%lu ms tick=%lu",
             ctx->key_id,
             (unsigned long)ctx->press_seq,
             (unsigned long)held_ms,
             (unsigned long)library_pressed_ms,
             (unsigned long)now);

    ctx->pressed = false;
}

static void key_duration_monitor_long_press_cb(void *button_handle, void *usr_data)
{
    key_duration_monitor_ctx_t *ctx = (key_duration_monitor_ctx_t *)usr_data;
    if (ctx == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t held_ms = ctx->pressed
                           ? key_duration_monitor_ticks_to_ms(now - ctx->press_down_tick)
                           : 0;
    uint32_t library_pressed_ms = iot_button_get_pressed_time((button_handle_t)button_handle);

    ESP_LOGI(TAG, "K%u long_press_start seq=%lu held=%lu ms library_pressed=%lu ms tick=%lu",
             ctx->key_id,
             (unsigned long)ctx->press_seq,
             (unsigned long)held_ms,
             (unsigned long)library_pressed_ms,
             (unsigned long)now);
}

esp_err_t key_duration_monitor_register(button_handle_t button_handle, uint8_t key_id)
{
    if (button_handle == NULL || key_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_key_context_count >= KEY_DURATION_MONITOR_MAX_KEYS) {
        ESP_LOGE(TAG, "No free monitor context for K%u", key_id);
        return ESP_ERR_NO_MEM;
    }

    key_duration_monitor_ctx_t *ctx = &s_key_contexts[s_key_context_count++];
    *ctx = (key_duration_monitor_ctx_t) {
        .key_id = key_id,
    };

    esp_err_t err = iot_button_register_cb(button_handle, BUTTON_PRESS_DOWN, NULL,
                                           key_duration_monitor_down_cb, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register down monitor for K%u: %s",
                 key_id, esp_err_to_name(err));
        return err;
    }

    err = iot_button_register_cb(button_handle, BUTTON_PRESS_UP, NULL,
                                 key_duration_monitor_up_cb, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register up monitor for K%u: %s",
                 key_id, esp_err_to_name(err));
        return err;
    }

    err = iot_button_register_cb(button_handle, BUTTON_LONG_PRESS_START, NULL,
                                 key_duration_monitor_long_press_cb, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register long-press monitor for K%u: %s",
                 key_id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Duration monitor registered for K%u", key_id);
    return ESP_OK;
}

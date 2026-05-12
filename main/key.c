#include "key.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <iot_button.h>
#include <button_gpio.h>

#include "led.h"

#define KEY_COUNT 8

#define KEY_1_GPIO 1
#define KEY_2_GPIO 2
#define KEY_3_GPIO 42
#define KEY_4_GPIO 41
#define KEY_5_GPIO 40
#define KEY_6_GPIO 39
#define KEY_7_GPIO 21
#define KEY_8_GPIO 45

#define KEY_LOW_POWER_STAGE2_ID 7
#define KEY_WIFI_CONFIG_ID      8
#define KEY_LONG_PRESS_MS       3000

typedef struct {
    uint8_t key_id;
    QueueHandle_t event_queue;
    TimerHandle_t long_press_timer;
    bool is_pressed;
    bool long_press_sent;
} key_ctx_t;

static const char *TAG = "KEY";

static const int s_key_gpios[KEY_COUNT] = {
    KEY_1_GPIO,
    KEY_2_GPIO,
    KEY_3_GPIO,
    KEY_4_GPIO,
    KEY_5_GPIO,
    KEY_6_GPIO,
    KEY_7_GPIO,
    KEY_8_GPIO,
};

static key_ctx_t s_key_ctx[KEY_COUNT];
static button_handle_t s_button_handles[KEY_COUNT];
static bool s_is_initialized;

static void key_send_event(void *button_handle, void *usr_data, led_key_event_t event)
{
    (void)button_handle;

    key_ctx_t *ctx = (key_ctx_t *)usr_data;
    if (ctx == NULL || ctx->event_queue == NULL) {
        return;
    }

    led_key_event_message_t msg = {
        .key_id = ctx->key_id,
        .event = event,
    };

    if (xQueueSend(ctx->event_queue, &msg, 0) != pdPASS) {
        ESP_LOGW(TAG, "Event queue full, dropped key %u", ctx->key_id);
    }
}

static void key_long_press_timer_cb(TimerHandle_t timer)
{
    key_ctx_t *ctx = (key_ctx_t *)pvTimerGetTimerID(timer);
    if (ctx == NULL || !ctx->is_pressed || ctx->long_press_sent) {
        return;
    }

    ctx->long_press_sent = true;
    key_send_event(NULL, ctx, LED_KEY_EVENT_LONG_PRESS_START);
}

static void key_press_down_cb(void *button_handle, void *usr_data)
{
    key_ctx_t *ctx = (key_ctx_t *)usr_data;
    if (ctx == NULL) {
        return;
    }

    if (ctx->long_press_timer == NULL) {
        key_send_event(button_handle, usr_data, LED_KEY_EVENT_PRESS_DOWN);
        return;
    }

    ctx->is_pressed = true;
    ctx->long_press_sent = false;
    xTimerStop(ctx->long_press_timer, 0);
    xTimerStart(ctx->long_press_timer, 0);
}

static void key_press_up_cb(void *button_handle, void *usr_data)
{
    key_ctx_t *ctx = (key_ctx_t *)usr_data;
    if (ctx == NULL || ctx->long_press_timer == NULL) {
        return;
    }

    ctx->is_pressed = false;
    xTimerStop(ctx->long_press_timer, 0);
    if (!ctx->long_press_sent) {
        key_send_event(button_handle, usr_data, LED_KEY_EVENT_SINGLE_CLICK);
    }
}

static void key_cleanup_created_buttons(size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (s_button_handles[i] != NULL) {
            iot_button_delete(s_button_handles[i]);
            s_button_handles[i] = NULL;
        }
    }
}

esp_err_t key_init(QueueHandle_t led_event_queue)
{
    if (s_is_initialized) {
        ESP_LOGW(TAG, "Key module already initialized");
        return ESP_OK;
    }

    if (led_event_queue == NULL) {
        ESP_LOGE(TAG, "Invalid LED event queue");
        return ESP_ERR_INVALID_ARG;
    }

    button_config_t button_cfg = {
        .long_press_time = KEY_LONG_PRESS_MS,
        .short_press_time = 0,
    };

    for (size_t i = 0; i < KEY_COUNT; ++i) {
        button_gpio_config_t gpio_cfg = {
            .gpio_num = s_key_gpios[i],
            .active_level = 0,     // Pressed = low level
            .enable_power_save = false,
            .disable_pull = false, // Enable internal pull-up
        };

        s_key_ctx[i].key_id = (uint8_t)(i + 1);
        s_key_ctx[i].event_queue = led_event_queue;

        esp_err_t err = iot_button_new_gpio_device(&button_cfg, &gpio_cfg, &s_button_handles[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create key %u on GPIO%d: %s",
                     (unsigned)(i + 1), s_key_gpios[i], esp_err_to_name(err));
            key_cleanup_created_buttons(i + 1);
            return err;
        }

        if (s_key_ctx[i].key_id == KEY_LOW_POWER_STAGE2_ID ||
            s_key_ctx[i].key_id == KEY_WIFI_CONFIG_ID) {
            s_key_ctx[i].long_press_timer = xTimerCreate("key_long",
                                                         pdMS_TO_TICKS(KEY_LONG_PRESS_MS),
                                                         pdFALSE,
                                                         &s_key_ctx[i],
                                                         key_long_press_timer_cb);
            if (s_key_ctx[i].long_press_timer == NULL) {
                ESP_LOGE(TAG, "Failed to create long-press timer for key %u",
                         (unsigned)(i + 1));
                key_cleanup_created_buttons(i + 1);
                return ESP_ERR_NO_MEM;
            }

            err = iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_DOWN, NULL,
                                         key_press_down_cb, &s_key_ctx[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register press-down callback for key %u: %s",
                         (unsigned)(i + 1), esp_err_to_name(err));
                key_cleanup_created_buttons(i + 1);
                return err;
            }

            err = iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_UP, NULL,
                                         key_press_up_cb, &s_key_ctx[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register press-up callback for key %u: %s",
                         (unsigned)(i + 1), esp_err_to_name(err));
                key_cleanup_created_buttons(i + 1);
                return err;
            }
        } else {
            err = iot_button_register_cb(s_button_handles[i], BUTTON_PRESS_DOWN, NULL,
                                         key_press_down_cb, &s_key_ctx[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register callback for key %u: %s",
                         (unsigned)(i + 1), esp_err_to_name(err));
                key_cleanup_created_buttons(i + 1);
                return err;
            }
        }
    }

    s_is_initialized = true;
    ESP_LOGI(TAG, "Initialized %d keys", KEY_COUNT);
    return ESP_OK;
}

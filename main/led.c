#include "led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <led_strip.h>

// LED Strip configuration
#define LED_STRIP_GPIO 48
#define LED_STRIP_NUM 1
#define LED_STRIP_RMT_RES_HZ (40 * 1000 * 1000) // 40MHz resolution

// Breathing effect configuration
#define BREATHING_PERIOD_MS 20
#define BREATHING_STEPS 50
#define COLOR_SWITCH_PERIOD_MS 2000
#define LED_BREATH_BRIGHTNESS 10 // 0-255, controls breath effect max brightness (~31%)

// Warning display configuration
#define WARNING_SHOW_MS 1000
#define LED_WARNING_BRIGHTNESS 10 // 0-255, controls warning color max brightness (~31%)
#define LED_LOW_POWER_WAIT_MS 1000

#define LED_TASK_STACK_SIZE 3072
#define LED_TASK_PRIORITY 5
#define LED_KEY_EVENT_QUEUE_LEN 16

static const char *TAG = "LED";

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_rgb_t;

static const led_rgb_t s_breath_colors[] = {
    {255, 0, 0}, // Red
    {0, 255, 0}, // Green
    {0, 0, 255}, // Blue
};

static const led_rgb_t s_warning_orange = {128, 0, 255};
static const led_rgb_t s_warning_yellow = {255, 255, 0};

static led_strip_handle_t s_led_strip;
static QueueHandle_t s_key_event_queue;
static SemaphoreHandle_t s_render_mutex;
static TaskHandle_t s_led_worker_handle;
static bool s_effect_started;
static volatile bool s_low_power;
static bool s_low_power_rendered;

static size_t s_current_color_idx;
static int s_breathing_step;
static uint32_t s_color_elapsed_ms;
static bool s_next_warning_is_orange = true;

static esp_err_t led_render_rgb(led_rgb_t color)
{
    if (s_render_mutex != NULL) {
        if (xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t err = led_strip_set_pixel(s_led_strip, 0, color.red, color.green, color.blue);
    if (err == ESP_OK) {
        err = led_strip_refresh(s_led_strip);
    }

    if (s_render_mutex != NULL) {
        xSemaphoreGive(s_render_mutex);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to refresh strip");
    return ESP_OK;
}

static void led_update_breathing_frame(void)
{
    int half_steps = BREATHING_STEPS / 2;
    int brightness;

    if (s_breathing_step < half_steps) {
        brightness = (s_breathing_step * 255) / half_steps;
    } else {
        brightness = ((BREATHING_STEPS - s_breathing_step - 1) * 255) / half_steps;
    }
    int scaled_brightness = (brightness * LED_BREATH_BRIGHTNESS) / 255;

    led_rgb_t base = s_breath_colors[s_current_color_idx];
    led_rgb_t mixed = {
        .red = (uint8_t)((base.red * scaled_brightness) / 255),
        .green = (uint8_t)((base.green * scaled_brightness) / 255),
        .blue = (uint8_t)((base.blue * scaled_brightness) / 255),
    };

    if (led_render_rgb(mixed) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to render breathing frame");
    }

    s_breathing_step++;
    if (s_breathing_step >= BREATHING_STEPS) {
        s_breathing_step = 0;
    }

    s_color_elapsed_ms += BREATHING_PERIOD_MS;
    if (s_color_elapsed_ms >= COLOR_SWITCH_PERIOD_MS) {
        s_color_elapsed_ms = 0;
        s_current_color_idx = (s_current_color_idx + 1) % (sizeof(s_breath_colors) / sizeof(s_breath_colors[0]));
        ESP_LOGI(TAG, "Switched to breath color index %u", (unsigned)s_current_color_idx);
    }
}

static void led_show_warning_and_wait(void)
{
    led_rgb_t raw = s_next_warning_is_orange ? s_warning_orange : s_warning_yellow;
    s_next_warning_is_orange = !s_next_warning_is_orange;

    led_rgb_t warning = {
        .red   = (uint8_t)((raw.red   * LED_WARNING_BRIGHTNESS) / 255),
        .green = (uint8_t)((raw.green * LED_WARNING_BRIGHTNESS) / 255),
        .blue  = (uint8_t)((raw.blue  * LED_WARNING_BRIGHTNESS) / 255),
    };

    if (led_render_rgb(warning) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to render warning color");
    }

    vTaskDelay(pdMS_TO_TICKS(WARNING_SHOW_MS));
}

static void led_worker_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_low_power) {
            if (!s_low_power_rendered) {
                (void)led_render_rgb((led_rgb_t){0, 0, 0});
                s_low_power_rendered = true;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LED_LOW_POWER_WAIT_MS));
            continue;
        }

        led_key_event_message_t msg;
        BaseType_t received = xQueueReceive(s_key_event_queue, &msg, pdMS_TO_TICKS(BREATHING_PERIOD_MS));
        if (received == pdPASS) {
            // Reset to orange so every new warning sequence always starts with orange
            s_next_warning_is_orange = true;

            do {
                ESP_LOGI(TAG, "Handle key%u warning", msg.key_id);
                led_show_warning_and_wait();
            } while (xQueueReceive(s_key_event_queue, &msg, 0) == pdPASS);

            continue;
        }

        led_update_breathing_frame();
    }
}

QueueHandle_t led_get_key_event_queue_handle(void)
{
    return s_key_event_queue;
}

void led_set_low_power(bool enable)
{
    if (!s_effect_started) {
        return;
    }

    s_low_power = enable;
    if (!enable) {
        s_low_power_rendered = false;
        if (s_led_worker_handle != NULL) {
            xTaskNotifyGive(s_led_worker_handle);
        }
    }
    if (enable) {
        (void)led_render_rgb((led_rgb_t){0, 0, 0});
        s_low_power_rendered = true;
    }
}

esp_err_t led_start_breath_effect(void)
{
    if (s_effect_started) {
        ESP_LOGW(TAG, "Breathing effect already started");
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_NUM,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip), TAG,
                        "Failed to initialize LED strip");

    s_key_event_queue = xQueueCreate(LED_KEY_EVENT_QUEUE_LEN, sizeof(led_key_event_message_t));
    if (s_key_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create key event queue");
        return ESP_ERR_NO_MEM;
    }

    s_render_mutex = xSemaphoreCreateMutex();
    if (s_render_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LED render mutex");
        vQueueDelete(s_key_event_queue);
        s_key_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(led_worker_task, "led_worker_task", LED_TASK_STACK_SIZE, NULL, LED_TASK_PRIORITY,
                    &s_led_worker_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create led_worker_task");
        vQueueDelete(s_key_event_queue);
        s_key_event_queue = NULL;
        vSemaphoreDelete(s_render_mutex);
        s_render_mutex = NULL;
        return ESP_FAIL;
    }

    s_effect_started = true;
    ESP_LOGI(TAG, "Breathing effect started");
    return ESP_OK;
}

#ifndef LED_H
#define LED_H

#include <stdint.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

typedef enum {
    LED_KEY_EVENT_PRESS_DOWN = 0,
    LED_KEY_EVENT_SINGLE_CLICK,
    LED_KEY_EVENT_LONG_PRESS_START,
} led_key_event_t;

typedef struct {
    uint8_t key_id;
    led_key_event_t event;
} led_key_event_message_t;

esp_err_t led_start_breath_effect(void);
QueueHandle_t led_get_key_event_queue_handle(void);

#endif // LED_H

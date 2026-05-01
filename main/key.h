#ifndef KEY_H
#define KEY_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

esp_err_t key_init(QueueHandle_t led_event_queue);

#endif // KEY_H

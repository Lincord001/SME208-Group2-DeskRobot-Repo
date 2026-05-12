#ifndef KEY_DURATION_MONITOR_H
#define KEY_DURATION_MONITOR_H

#include <stdint.h>

#include <esp_err.h>
#include <iot_button.h>

esp_err_t key_duration_monitor_register(button_handle_t button_handle, uint8_t key_id);

#endif // KEY_DURATION_MONITOR_H

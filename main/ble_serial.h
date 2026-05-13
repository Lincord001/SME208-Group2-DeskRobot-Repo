#ifndef BLE_SERIAL_H
#define BLE_SERIAL_H

#include <stdbool.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_serial_init(audio_buffer_t *audio_buffer, uint32_t sample_rate_hz);
bool ble_serial_is_connected(void);
esp_err_t ble_serial_send_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif // BLE_SERIAL_H

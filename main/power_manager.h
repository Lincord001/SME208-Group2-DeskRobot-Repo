#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t power_manager_init(audio_buffer_t *audio_buffer);
void power_manager_notify_activity(void);
bool power_manager_is_low_power(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_MANAGER_H

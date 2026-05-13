#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t power_manager_init(audio_buffer_t *audio_buffer);
void power_manager_notify_activity(void);
esp_err_t power_manager_enter_stage1(void);
esp_err_t power_manager_enter_stage2(void);
esp_err_t power_manager_set_idle_timeouts_ms(uint32_t stage1_ms,
                                             uint32_t stage2_ms);
void power_manager_get_idle_timeouts_ms(uint32_t *out_stage1_ms,
                                        uint32_t *out_stage2_ms);
bool power_manager_is_low_power(void);
const char *power_manager_get_stage_string(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_MANAGER_H

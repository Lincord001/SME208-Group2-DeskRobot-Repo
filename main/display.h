#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_STATE_IDLE = 0,
    DISPLAY_STATE_LISTENING,
    DISPLAY_STATE_THINKING,
    DISPLAY_STATE_ANSWERING,
    DISPLAY_STATE_WIFI,
    DISPLAY_STATE_STATUS,
    DISPLAY_STATE_ERROR,
} display_state_t;

esp_err_t display_init(void);
esp_err_t display_start_task(void);

void display_attach_audio_buffer(const audio_buffer_t *audio_buffer,
                                 uint32_t sample_rate_hz);

void display_update_state(display_state_t state,
                          uint32_t elapsed_sec,
                          uint32_t total_sec);
void display_set_idle(void);
void display_set_listening(uint32_t sec);
void display_set_thinking(uint32_t sec);
void display_set_answering(uint32_t current_sec, uint32_t total_sec);
void display_set_status(const char *title, const char *detail);
void display_set_wifi_status(const char *status);
display_state_t display_get_state(void);
void display_set_low_power_overlay(bool enable);
void display_set_panel_power(bool enable);
void display_set_error(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_H

#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYSTEM_STATUS_VOICE_ASR = 0,
    SYSTEM_STATUS_VOICE_LLM,
    SYSTEM_STATUS_VOICE_TTS,
    SYSTEM_STATUS_VOICE_STAGE_COUNT,
} system_status_voice_stage_t;

esp_err_t system_status_init(void);
void system_status_voice_begin(system_status_voice_stage_t stage);
void system_status_voice_end(system_status_voice_stage_t stage,
                             esp_err_t err,
                             const char *reason);
void system_status_show_next_page(void);
void system_status_log_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_STATUS_H

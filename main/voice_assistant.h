#ifndef VOICE_ASSISTANT_H
#define VOICE_ASSISTANT_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_assistant_init(void);
esp_err_t voice_assistant_start_llm_test(void);
esp_err_t voice_assistant_start_asr_test(const audio_buffer_t *audio_buffer,
                                         uint32_t sample_rate_hz);
esp_err_t voice_assistant_start_tts_test(audio_buffer_t *audio_buffer);
esp_err_t voice_assistant_start_full_test(audio_buffer_t *audio_buffer,
                                          uint32_t sample_rate_hz);
bool voice_assistant_is_busy(void);
esp_err_t voice_assistant_cancel_current(void);

#ifdef __cplusplus
}
#endif

#endif // VOICE_ASSISTANT_H

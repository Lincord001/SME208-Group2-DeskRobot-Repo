#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#include <stddef.h>
#include <stdbool.h>

#include <esp_err.h>

#include "audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tts_client_init(void);
esp_err_t tts_client_synthesize_count_bytes(const char *text, size_t *out_audio_bytes);
esp_err_t tts_client_synthesize_to_audio_buffer(const char *text,
                                                audio_buffer_t *audio_buffer,
                                                size_t *out_audio_bytes);
esp_err_t tts_client_synthesize_to_audio_buffer_with_cancel(const char *text,
                                                            audio_buffer_t *audio_buffer,
                                                            size_t *out_audio_bytes,
                                                            bool (*cancel_cb)(void *ctx),
                                                            void *cancel_ctx);

#ifdef __cplusplus
}
#endif

#endif // TTS_CLIENT_H

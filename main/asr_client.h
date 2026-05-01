#ifndef ASR_CLIENT_H
#define ASR_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t asr_client_init(void);
esp_err_t asr_client_recognize_pcm16(const int16_t *pcm,
                                     size_t sample_count,
                                     uint32_t sample_rate_hz,
                                     char *out_text,
                                     size_t out_text_len);

#ifdef __cplusplus
}
#endif

#endif // ASR_CLIENT_H

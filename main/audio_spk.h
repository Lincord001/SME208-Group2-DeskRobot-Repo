#ifndef AUDIO_SPK_H
#define AUDIO_SPK_H

#include <stdbool.h>
#include <esp_err.h>

#include "audio_mic.h" // audio_buffer_t

esp_err_t audio_spk_init(audio_buffer_t *audio_buffer);
esp_err_t audio_spk_set_playing(bool enable);
void audio_spk_cycle_volume_level(void);

#endif // AUDIO_SPK_H

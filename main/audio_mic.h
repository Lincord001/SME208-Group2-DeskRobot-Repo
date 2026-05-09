#ifndef AUDIO_MIC_H
#define AUDIO_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

typedef struct {
    int16_t *buffer;            // PSRAM中的音频数据缓冲区
    size_t total_size;          // 总缓冲区大小(字节)
    size_t current_write_pos;   // 当前写入位置(字节)
    size_t current_read_pos;    // 当前读取位置(字节)
    size_t recorded_size;       // 已记录的音频数据大小(字节)
    bool recording;             // 是否正在录音
    bool playing;               // 是否正在播放
    bool recording_complete;    // 录音是否完成
    portMUX_TYPE lock;          // 保护状态字段，避免多任务并发读写撕裂
} audio_buffer_t;

typedef struct {
    size_t total_size;
    size_t current_write_pos;
    size_t current_read_pos;
    size_t recorded_size;
    bool recording;
    bool playing;
    bool recording_complete;
} audio_buffer_state_t;

static inline void audio_buffer_init(audio_buffer_t *audio_buffer,
                                     int16_t *buffer,
                                     size_t total_size)
{
    audio_buffer->buffer = buffer;
    audio_buffer->total_size = total_size;
    audio_buffer->current_write_pos = 0;
    audio_buffer->current_read_pos = 0;
    audio_buffer->recorded_size = 0;
    audio_buffer->recording = false;
    audio_buffer->playing = false;
    audio_buffer->recording_complete = false;
    audio_buffer->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

static inline void audio_buffer_lock(audio_buffer_t *audio_buffer)
{
    portENTER_CRITICAL(&audio_buffer->lock);
}

static inline void audio_buffer_unlock(audio_buffer_t *audio_buffer)
{
    portEXIT_CRITICAL(&audio_buffer->lock);
}

static inline void audio_buffer_get_state(const audio_buffer_t *audio_buffer,
                                          audio_buffer_state_t *out_state)
{
    audio_buffer_t *mutable_audio_buffer = (audio_buffer_t *)audio_buffer;
    portENTER_CRITICAL(&mutable_audio_buffer->lock);
    out_state->total_size = audio_buffer->total_size;
    out_state->current_write_pos = audio_buffer->current_write_pos;
    out_state->current_read_pos = audio_buffer->current_read_pos;
    out_state->recorded_size = audio_buffer->recorded_size;
    out_state->recording = audio_buffer->recording;
    out_state->playing = audio_buffer->playing;
    out_state->recording_complete = audio_buffer->recording_complete;
    portEXIT_CRITICAL(&mutable_audio_buffer->lock);
}

esp_err_t audio_mic_init(audio_buffer_t *audio_buffer);
void audio_mic_set_recording(bool enable);

#endif // AUDIO_MIC_H

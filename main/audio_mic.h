#ifndef AUDIO_MIC_H
#define AUDIO_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

typedef struct {
    int16_t *buffer;            // PSRAM中的音频数据缓冲区
    size_t total_size;          // 总缓冲区大小(字节)
    size_t current_write_pos;   // 当前写入位置(字节)
    size_t current_read_pos;    // 当前读取位置(字节)
    size_t recorded_size;       // 已记录的音频数据大小(字节)
    bool recording;             // 是否正在录音
    bool playing;               // 是否正在播放
    bool recording_complete;    // 录音是否完成
} audio_buffer_t;

esp_err_t audio_mic_init(audio_buffer_t *audio_buffer);
void audio_mic_set_recording(bool enable);

#endif // AUDIO_MIC_H

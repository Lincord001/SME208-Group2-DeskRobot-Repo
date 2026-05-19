/*
 * audio_mic.c
 * INMP441 I2S RX 音频采集模块
 *
 * 硬件：INMP441, L/R 接 GND → 左声道单声道输出
 * I2S 配置：Philips 标准, 32-bit slot 接收 24-bit 有效载荷
 *            → 软件 >>8 下移得到 16-bit PCM 写入 PSRAM
 * 任务：持续采集，仅在 audio_buffer.recording==true 时写入缓冲区
 */

#include "audio_mic.h"
#include "audio_spk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ── 引脚 ─────────────────────────────────────────────── */
#define I2S_MIC_SCK_PIN GPIO_NUM_5   // INMP441 SCK (BCLK)
#define I2S_MIC_WS_PIN  GPIO_NUM_6   // INMP441 WS
#define I2S_MIC_DIN_PIN GPIO_NUM_7   // INMP441 SD  (DIN)

/* ── 音频参数 ──────────────────────────────────────────── */
#define MIC_SAMPLE_RATE_HZ   24000
#define MIC_DMA_FRAMES       512     // 每次 DMA 读取的帧数（32-bit × 512 = 2 KB）
#define MIC_LEVEL_LOG_MS     500

/* ── 任务参数 ──────────────────────────────────────────── */
#define MIC_TASK_STACK  4096
#define MIC_TASK_PRIO   6

static const char *TAG = "AUDIO_MIC";

static i2s_chan_handle_t  s_rx_chan;
static audio_buffer_t    *s_audio_buf;
static TaskHandle_t       s_task_handle;
static bool               s_initialized;

static inline int32_t mic_abs_i16(int16_t sample)
{
    return (sample == INT16_MIN) ? 32768 : (sample < 0 ? -(int32_t)sample : sample);
}

/* ── 采集任务 ─────────────────────────────────────────── */
static void mic_task(void *arg)
{
    (void)arg;

    /* DMA 缓冲区：32-bit slot，每帧 4 字节 */
    static int32_t rx_buf[MIC_DMA_FRAMES];
    int32_t level_peak = 0;
    uint64_t level_sum = 0;
    uint32_t level_samples = 0;
    TickType_t last_level_log_tick = xTaskGetTickCount();
    bool was_recording = false;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, rx_buf, sizeof(rx_buf),
                                          &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
            continue;
        }

        /* 录音停止后，先封存当前已采样的数据，再进入空闲循环。 */
        bool finalized_recording = false;
        size_t finalized_size = 0;
        audio_buffer_lock(s_audio_buf);
        if (was_recording &&
            !s_audio_buf->recording &&
            s_audio_buf->current_write_pos > 0 &&
            !s_audio_buf->recording_complete) {
            s_audio_buf->recorded_size =
                s_audio_buf->current_write_pos * sizeof(int16_t);
            s_audio_buf->recording_complete = true;
            finalized_recording = true;
            finalized_size = s_audio_buf->recorded_size;
        }
        bool recording = s_audio_buf->recording;
        size_t write_pos = s_audio_buf->current_write_pos;
        size_t total_size = s_audio_buf->total_size;
        audio_buffer_unlock(s_audio_buf);
        was_recording = recording;
        if (finalized_recording) {
            ESP_LOGI(TAG, "Recording stopped, %u bytes saved",
                     (unsigned)finalized_size);
        }

        /* 仅在录音状态下写入 PSRAM */
        if (!recording) {
            continue;
        }

        size_t frames = bytes_read / sizeof(int32_t);
        bool buffer_full = false;
        for (size_t i = 0; i < frames; i++) {
            /* INMP441 Philips 模式：24-bit 数据占 32-bit slot 的高 24 位
               右移 8 位取 24-bit 有符号值，再截断为 16-bit PCM              */
            int32_t s24 = rx_buf[i] >> 8;
            int16_t s16 = (int16_t)(s24 >> 8); // 再右移 8 得到 16-bit

            int32_t abs_sample = mic_abs_i16(s16);
            if (abs_sample > level_peak) {
                level_peak = abs_sample;
            }
            level_sum += (uint32_t)abs_sample;
            level_samples++;

            size_t byte_pos  = write_pos * sizeof(int16_t);

            if (byte_pos + sizeof(int16_t) > total_size) {
                buffer_full = true;
                break;
            }

            s_audio_buf->buffer[write_pos] = s16;
            write_pos++;
        }

        size_t full_size = 0;
        audio_buffer_lock(s_audio_buf);
        s_audio_buf->current_write_pos = write_pos;
        if (buffer_full) {
            size_t byte_pos = write_pos * sizeof(int16_t);
            /* 缓冲区已满，自动停止录音 */
            s_audio_buf->recording          = false;
            s_audio_buf->recording_complete = true;
            s_audio_buf->recorded_size      = byte_pos;
            full_size = byte_pos;
        }
        audio_buffer_unlock(s_audio_buf);
        if (buffer_full) {
            ESP_LOGI(TAG, "Buffer full, recording stopped (%u bytes)",
                     (unsigned)full_size);
        }

        TickType_t now_tick = xTaskGetTickCount();
        if (level_samples > 0 &&
            (now_tick - last_level_log_tick) >= pdMS_TO_TICKS(MIC_LEVEL_LOG_MS)) {
            uint32_t avg = (uint32_t)(level_sum / level_samples);
            ESP_LOGI(TAG, "Mic level: peak=%ld avg=%lu samples=%lu",
                     (long)level_peak, (unsigned long)avg, (unsigned long)level_samples);
            level_peak = 0;
            level_sum = 0;
            level_samples = 0;
            last_level_log_tick = now_tick;
        }
    }
}

/* ── 公开 API ─────────────────────────────────────────── */

esp_err_t audio_mic_init(audio_buffer_t *audio_buffer)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    if (audio_buffer == NULL || audio_buffer->buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_audio_buf = audio_buffer;

    /* ── I2S RX 通道 ─────────────────────────── */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
                        TAG, "create RX channel failed");

    /* Philips + 32-bit slot + 左声道单声道 */
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                             I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    /* 24-bit 模式要求 mclk_multiple 是 3 的倍数 */
    i2s_std_clk_config_t clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk  = I2S_GPIO_UNUSED,
            .bclk  = I2S_MIC_SCK_PIN,
            .ws    = I2S_MIC_WS_PIN,
            .dout  = I2S_GPIO_UNUSED,
            .din   = I2S_MIC_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
        TAG, "init std mode failed");
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_rx_chan),
        TAG, "enable RX channel failed");

    /* ── 创建采集任务 ─────────────────────────── */
    if (xTaskCreate(mic_task, "mic_task", MIC_TASK_STACK, NULL,
                    MIC_TASK_PRIO, &s_task_handle) != pdPASS) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized, sample_rate=%d Hz", MIC_SAMPLE_RATE_HZ);
    return ESP_OK;
}

void audio_mic_set_recording(bool enable)
{
    if (!s_initialized || s_audio_buf == NULL) return;

    if (enable) {
        (void)audio_spk_set_sample_rate(MIC_SAMPLE_RATE_HZ);

        audio_buffer_lock(s_audio_buf);
        /* 重置缓冲区写指针，开始新录音 */
        s_audio_buf->current_write_pos  = 0;
        s_audio_buf->current_read_pos   = 0;
        s_audio_buf->recorded_size      = 0;
        s_audio_buf->recording_complete = false;
        s_audio_buf->recording          = true;
        audio_buffer_unlock(s_audio_buf);
        ESP_LOGI(TAG, "Recording started");
    } else {
        size_t recorded_size = 0;
        bool finalized_recording = false;

        audio_buffer_lock(s_audio_buf);
        s_audio_buf->recording = false;
        if (s_audio_buf->current_write_pos > 0 &&
            !s_audio_buf->recording_complete) {
            s_audio_buf->recorded_size =
                s_audio_buf->current_write_pos * sizeof(int16_t);
            s_audio_buf->recording_complete = true;
            recorded_size = s_audio_buf->recorded_size;
            finalized_recording = true;
        }
        audio_buffer_unlock(s_audio_buf);
        /* Stop must publish a stable completed buffer before ASR can start. */
        if (finalized_recording) {
            ESP_LOGI(TAG, "Recording stopped, %u bytes saved",
                     (unsigned)recorded_size);
        } else {
            ESP_LOGI(TAG, "Recording stop requested");
        }
    }
}

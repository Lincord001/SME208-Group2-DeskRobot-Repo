/*
 * audio_spk.c
 * MAX98357A I2S TX 音频播放模块
 *
 * 硬件：MAX98357A，BCLK/WS/DIN 接 ESP32-S3
 * I2S 配置：Philips 标准, 16-bit slot, 单声道, 24 kHz
 * 任务：持续运行，仅在 audio_buffer.playing==true 时从 PSRAM 读数据写出
 */

#include "audio_spk.h"
#include "audio_mic.h"
#include "servo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ── 引脚 ─────────────────────────────────────────────── */
#define I2S_SPK_BCLK_PIN GPIO_NUM_12  // MAX98357A BCLK
#define I2S_SPK_WS_PIN   GPIO_NUM_11  // MAX98357A LRC (WS)
#define I2S_SPK_DOUT_PIN GPIO_NUM_13  // MAX98357A DIN (DOUT from ESP)

/* ── 音频参数 ──────────────────────────────────────────── */
#define SPK_SAMPLE_RATE_HZ  24000
#define SPK_TTS_PLAYBACK_RATE_HZ 12800
#define SPK_STREAM_WAIT_MS  10
#define SPK_STREAM_LOW_WATER_MS 300
#define SPK_STREAM_HIGH_WATER_MS 1250
#define SPK_MONITOR_INTERVAL_US 1000000
#define SPK_DMA_FRAMES      512       // 每次 DMA 写出的帧数（16-bit × 512 = 1 KB）
#define SPK_SILENCE_FLUSH_FRAMES 512  // 播放结束后额外发送的静音帧数
#define SPK_STARTUP_SILENCE_FRAMES 128 // 播放开始前用于清空残留样本的静音帧数
/* ── 任务参数 ──────────────────────────────────────────── */
#define SPK_TASK_STACK  4096
#define SPK_TASK_PRIO   6

static const char *TAG = "AUDIO_SPK";

static const uint8_t s_volume_levels_percent[] = {50, 100, 150, 200};

static i2s_chan_handle_t  s_tx_chan;
static audio_buffer_t    *s_audio_buf;
static TaskHandle_t       s_task_handle;
static bool               s_initialized;
static bool               s_tx_enabled;
static uint32_t           s_sample_rate_hz = SPK_SAMPLE_RATE_HZ;
static size_t             s_volume_level_idx = 2; // default 100%

static inline int16_t audio_spk_apply_volume(int16_t sample)
{
    int32_t scaled = ((int32_t)sample * s_volume_levels_percent[s_volume_level_idx]) / 100;
    if (scaled > INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

static esp_err_t audio_spk_enable_tx_if_needed(void)
{
    if (s_tx_enabled) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK) {
        s_tx_enabled = true;
    }
    return err;
}

static esp_err_t audio_spk_reconfig_clock(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sample_rate_hz == sample_rate_hz) {
        return ESP_OK;
    }
    if (s_tx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconfig TX sample rate %lu Hz failed: %s",
                 (unsigned long)sample_rate_hz,
                 esp_err_to_name(err));
        return err;
    }

    s_sample_rate_hz = sample_rate_hz;
    ESP_LOGI(TAG, "TX sample rate set to %lu Hz", (unsigned long)s_sample_rate_hz);
    return ESP_OK;
}

static void audio_spk_prime_tx_with_silence(void)
{
    if (!s_tx_enabled) {
        return;
    }

    static int16_t silence_buf[SPK_STARTUP_SILENCE_FRAMES];
    size_t bytes_written = 0;

    memset(silence_buf, 0, sizeof(silence_buf));
    esp_err_t err = i2s_channel_write(s_tx_chan, silence_buf, sizeof(silence_buf),
                                      &bytes_written, pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "prime silence failed: %s", esp_err_to_name(err));
    }
}

static void audio_spk_flush_and_disable_tx(void)
{
    if (!s_tx_enabled) {
        return;
    }

    static int16_t silence_buf[SPK_SILENCE_FLUSH_FRAMES];
    size_t bytes_written = 0;

    memset(silence_buf, 0, sizeof(silence_buf));
    esp_err_t err = i2s_channel_write(s_tx_chan, silence_buf, sizeof(silence_buf),
                                      &bytes_written, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush silence failed: %s", esp_err_to_name(err));
    }

    err = i2s_channel_disable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable TX channel failed: %s", esp_err_to_name(err));
        return;
    }

    s_tx_enabled = false;
}

static esp_err_t audio_spk_write_stream_gap(void)
{
    static int16_t silence_buf[SPK_DMA_FRAMES];
    size_t bytes_written = 0;

    memset(silence_buf, 0, sizeof(silence_buf));
    return i2s_channel_write(s_tx_chan, silence_buf, sizeof(silence_buf),
                             &bytes_written, portMAX_DELAY);
}

static uint32_t audio_spk_samples_to_ms(size_t samples)
{
    uint32_t sample_rate_hz = s_sample_rate_hz > 0 ? s_sample_rate_hz : SPK_SAMPLE_RATE_HZ;
    return (uint32_t)((samples * 1000U) / sample_rate_hz);
}

/* ── 播放任务 ─────────────────────────────────────────── */
static void spk_task(void *arg)
{
    (void)arg;

    static int16_t tx_buf[SPK_DMA_FRAMES];

    while (1) {
        audio_buffer_state_t state;
        audio_buffer_get_state(s_audio_buf, &state);
        if (!state.playing) {
            /* 未在播放状态，让出 CPU 等待 */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 检查是否有可播放的录音数据 */
        if (state.recorded_size == 0) {
            ESP_LOGW(TAG, "No recorded data to play");
            audio_buffer_lock(s_audio_buf);
            s_audio_buf->playing = false;
            audio_buffer_unlock(s_audio_buf);
            continue;
        }

        /* 重置读指针，开始播放 */
        audio_buffer_lock(s_audio_buf);
        s_audio_buf->current_read_pos = 0;
        size_t available_samples = s_audio_buf->recorded_size / sizeof(int16_t);
        audio_buffer_unlock(s_audio_buf);

        if (audio_spk_enable_tx_if_needed() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to enable TX channel");
            audio_buffer_lock(s_audio_buf);
            s_audio_buf->playing = false;
            audio_buffer_unlock(s_audio_buf);
            continue;
        }

        /* 清掉上一次播放停止后残留在 FIFO/总线上的旧样本，避免一按 K2
           先冒出一小段末尾音频。 */
        audio_spk_prime_tx_with_silence();

        bool playback_motion_started = false;
        esp_err_t motion_err = servo_start_playback_motion();
        if (motion_err == ESP_OK) {
            playback_motion_started = true;
        } else {
            ESP_LOGW(TAG, "Failed to start playback servo motion: %s",
                     esp_err_to_name(motion_err));
        }

        bool stream_buffering = false;
        int64_t playback_start_us = esp_timer_get_time();
        int64_t last_monitor_us = playback_start_us;

        ESP_LOGI(TAG, "Playback started, initial=%u samples, streaming=%u",
                 (unsigned)available_samples,
                 state.recording_complete ? 0U : 1U);

        while (1) {
            audio_buffer_get_state(s_audio_buf, &state);
            if (!state.playing) {
                break;
            }

            size_t read_pos = state.current_read_pos;
            available_samples = state.recorded_size / sizeof(int16_t);
            size_t buffered_samples =
                available_samples > read_pos ? available_samples - read_pos : 0;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_monitor_us >= SPK_MONITOR_INTERVAL_US) {
                ESP_LOGI(TAG,
                         "VOICE_MON SPK elapsed=%ums played=%ums buffered=%ums read=%u available=%u streaming=%u buffering=%u complete=%u",
                         (unsigned)((now_us - playback_start_us) / 1000),
                         (unsigned)audio_spk_samples_to_ms(read_pos),
                         (unsigned)audio_spk_samples_to_ms(buffered_samples),
                         (unsigned)read_pos,
                         (unsigned)available_samples,
                         state.recording_complete ? 0U : 1U,
                         stream_buffering ? 1U : 0U,
                         state.recording_complete ? 1U : 0U);
                last_monitor_us = now_us;
            }

            if (!state.recording_complete) {
                if (stream_buffering) {
                    if (audio_spk_samples_to_ms(buffered_samples) < SPK_STREAM_HIGH_WATER_MS) {
                        esp_err_t gap_err = audio_spk_write_stream_gap();
                        if (gap_err != ESP_OK) {
                            ESP_LOGW(TAG, "stream refill silence failed: %s",
                                     esp_err_to_name(gap_err));
                            vTaskDelay(pdMS_TO_TICKS(SPK_STREAM_WAIT_MS));
                        }
                        continue;
                    }

                    stream_buffering = false;
                    ESP_LOGI(TAG, "VOICE_MON SPK buffer_resume buffered=%ums samples=%u",
                             (unsigned)audio_spk_samples_to_ms(buffered_samples),
                             (unsigned)buffered_samples);
                } else if (audio_spk_samples_to_ms(buffered_samples) < SPK_STREAM_LOW_WATER_MS) {
                    stream_buffering = true;
                    ESP_LOGI(TAG, "VOICE_MON SPK buffer_low buffered=%ums samples=%u",
                             (unsigned)audio_spk_samples_to_ms(buffered_samples),
                             (unsigned)buffered_samples);
                    esp_err_t gap_err = audio_spk_write_stream_gap();
                    if (gap_err != ESP_OK) {
                        ESP_LOGW(TAG, "stream low-water silence failed: %s",
                                 esp_err_to_name(gap_err));
                        vTaskDelay(pdMS_TO_TICKS(SPK_STREAM_WAIT_MS));
                    }
                    continue;
                }
            }

            if (read_pos >= available_samples) {
                if (!state.recording_complete) {
                    esp_err_t gap_err = audio_spk_write_stream_gap();
                    if (gap_err != ESP_OK) {
                        ESP_LOGW(TAG, "stream gap silence failed: %s",
                                 esp_err_to_name(gap_err));
                        vTaskDelay(pdMS_TO_TICKS(SPK_STREAM_WAIT_MS));
                    }
                    continue;
                }

                /* 播完一遍，停止 */
                audio_buffer_lock(s_audio_buf);
                s_audio_buf->playing = false;
                audio_buffer_unlock(s_audio_buf);
                ESP_LOGI(TAG, "Playback finished");
                break;
            }

            /* 填充 DMA 发送缓冲区 */
            size_t chunk = buffered_samples;
            if (chunk > SPK_DMA_FRAMES) chunk = SPK_DMA_FRAMES;

            for (size_t i = 0; i < chunk; ++i) {
                tx_buf[i] = audio_spk_apply_volume(s_audio_buf->buffer[read_pos + i]);
            }

            /* 不足一帧时补零 */
            if (chunk < SPK_DMA_FRAMES) {
                memset(&tx_buf[chunk], 0,
                       (SPK_DMA_FRAMES - chunk) * sizeof(int16_t));
            }

            size_t bytes_written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan, tx_buf,
                                               SPK_DMA_FRAMES * sizeof(int16_t),
                                               &bytes_written, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s_channel_write failed: %s",
                         esp_err_to_name(err));
                audio_buffer_lock(s_audio_buf);
                s_audio_buf->playing = false;
                audio_buffer_unlock(s_audio_buf);
                break;
            }

            audio_buffer_lock(s_audio_buf);
            s_audio_buf->current_read_pos += chunk;
            audio_buffer_unlock(s_audio_buf);
        }

        if (playback_motion_started) {
            servo_stop_playback_motion();
        }
        audio_spk_flush_and_disable_tx();
    }
}

/* ── 公开 API ─────────────────────────────────────────── */

esp_err_t audio_spk_init(audio_buffer_t *audio_buffer)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    if (audio_buffer == NULL || audio_buffer->buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_audio_buf = audio_buffer;

    /* ── I2S TX 通道 ─────────────────────────── */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "create TX channel failed");

    /* Philips + 16-bit slot + 单声道 */
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                             I2S_SLOT_MODE_MONO);

    i2s_std_clk_config_t clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE_HZ);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk  = I2S_GPIO_UNUSED,
            .bclk  = I2S_SPK_BCLK_PIN,
            .ws    = I2S_SPK_WS_PIN,
            .dout  = I2S_SPK_DOUT_PIN,
            .din   = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
        TAG, "init std mode failed");
    /* ── 创建播放任务 ─────────────────────────── */
    if (xTaskCreate(spk_task, "spk_task", SPK_TASK_STACK, NULL,
                    SPK_TASK_PRIO, &s_task_handle) != pdPASS) {
        i2s_del_channel(s_tx_chan);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized, sample_rate=%d Hz", SPK_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_spk_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_initialized || s_audio_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_buffer_state_t state = {0};
    audio_buffer_get_state(s_audio_buf, &state);
    if (state.playing) {
        return ESP_ERR_INVALID_STATE;
    }

    return audio_spk_reconfig_clock(sample_rate_hz);
}

esp_err_t audio_spk_set_playing(bool enable)
{
    if (!s_initialized || s_audio_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable) {
        bool no_recording_available = false;
        bool still_recording = false;

        audio_buffer_lock(s_audio_buf);
        if (s_audio_buf->recorded_size == 0) {
            no_recording_available = true;
        } else if (s_audio_buf->recording) {
            still_recording = true;
        } else {
            s_audio_buf->current_read_pos = 0;
            s_audio_buf->playing          = true;
        }
        audio_buffer_unlock(s_audio_buf);

        if (no_recording_available) {
            ESP_LOGW(TAG, "No recording available to play");
            return ESP_ERR_INVALID_STATE;
        }
        if (still_recording) {
            ESP_LOGW(TAG, "Still recording, cannot play");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Playback start requested");
    } else {
        audio_buffer_lock(s_audio_buf);
        s_audio_buf->playing = false;
        audio_buffer_unlock(s_audio_buf);
        ESP_LOGI(TAG, "Playback stop requested");
    }

    return ESP_OK;
}

void audio_spk_cycle_volume_level(void)
{
    s_volume_level_idx = (s_volume_level_idx + 1) %
                         (sizeof(s_volume_levels_percent) / sizeof(s_volume_levels_percent[0]));
    ESP_LOGI(TAG, "Volume level: %u%%",
             (unsigned)s_volume_levels_percent[s_volume_level_idx]);
}

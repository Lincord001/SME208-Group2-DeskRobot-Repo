/*
 * main.c
 *
 * 功能：
 *   - 分配 PSRAM 音频缓冲区（20 秒 × 24kHz × 16-bit mono = 960 KB）
 *   - 初始化 LED 呼吸灯、按键、麦克风（I2S RX）、扬声器（I2S TX）
 *   - 按键事件通过中转队列分发：
 *       K1 → 切换录音 开/停
 *       K2 → 切换播放 开/停
 *       K8 → 切换播放音量挡位
 *       所有事件同步转发给 LED 队列（保持按键闪灯效果）
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "audio_mic.h"
#include "audio_spk.h"
#include "display.h"
#include "key.h"
#include "led.h"
#include "servo.h"
#include "voice_assistant.h"
#include "wifi_network.h"

static const char *TAG = "MAIN";

/* ── 音频缓冲区参数 ──────────────────────────────────────
 * 24 kHz × 16-bit × 1 ch × 20 s = 960 000 字节
 * 向上对齐到 4 字节边界                                    */
#define AUDIO_SAMPLE_RATE  24000
#define AUDIO_MAX_SECS     20
#define AUDIO_BUF_BYTES    (AUDIO_SAMPLE_RATE * sizeof(int16_t) * AUDIO_MAX_SECS)

/* ── 中转队列 ────────────────────────────────────────────
 * main_key_task 从此队列读取按键事件，按 key_id 分发后
 * 再将事件转发给 LED 队列                                  */
#define MAIN_KEY_QUEUE_LEN 16

static audio_buffer_t s_audio_buf;
static QueueHandle_t  s_main_key_queue;

/* ── 按键分发任务 ─────────────────────────────────────── */
static void main_key_task(void *arg)
{
    QueueHandle_t led_queue = (QueueHandle_t)arg;
    led_key_event_message_t msg;
    esp_err_t err;

    while (1) {
        if (xQueueReceive(s_main_key_queue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }

        /* ── 音频控制逻辑（不阻塞，仅设置标志位）─────── */
        switch (msg.key_id) {
        case 1: /* K1：录音 开/停 */
            if (!s_audio_buf.recording) {
                if (s_audio_buf.playing) {
                    ESP_LOGW(TAG, "K1: stop playback first");
                } else {
                    audio_mic_set_recording(true);
                    display_set_listening(0);
                    ESP_LOGI(TAG, "K1: recording started");
                }
            } else {
                audio_mic_set_recording(false);
                display_set_status("Recorded", "Press K7");
                ESP_LOGI(TAG, "K1: recording stopped");
            }
            break;

        case 2: /* K2：播放 开/停 */
            if (!s_audio_buf.playing) {
                if (s_audio_buf.recording) {
                    ESP_LOGW(TAG, "K2: stop recording first");
                } else {
                    esp_err_t play_err = audio_spk_set_playing(true);
                    if (play_err == ESP_OK) {
                        uint32_t total_sec = (uint32_t)((s_audio_buf.recorded_size / sizeof(int16_t)) /
                                                        AUDIO_SAMPLE_RATE);
                        display_set_answering(0, total_sec);
                        ESP_LOGI(TAG, "K2: playback started");
                    } else {
                        ESP_LOGW(TAG, "K2: playback start rejected");
                    }
                }
            } else {
                esp_err_t play_err = audio_spk_set_playing(false);
                if (play_err == ESP_OK) {
                    display_set_idle();
                    ESP_LOGI(TAG, "K2: playback stopped");
                }
            }
            break;

        case 8: /* K8：切换播放音量挡位 */
            audio_spk_cycle_volume_level();
            ESP_LOGI(TAG, "K8: volume level switched");
            break;

        case 7: /* K7: full ASR->LLM->TTS test, or TTS smoke test if no audio */
            if (s_audio_buf.recording_complete && s_audio_buf.recorded_size > 0 &&
                !s_audio_buf.recording && !s_audio_buf.playing) {
                err = voice_assistant_start_full_test(&s_audio_buf, AUDIO_SAMPLE_RATE);
            } else {
                err = voice_assistant_start_tts_test(&s_audio_buf);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K7: voice test rejected (%s)", esp_err_to_name(err));
            }
            break;

        case 3: /* K3：垂直舵机 -10° */
            err = servo_move_relative(SERVO_AXIS_VERTICAL, -10);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: servo command rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            }
            break;

        case 4: /* K4：垂直舵机 +10° */
            err = servo_move_relative(SERVO_AXIS_VERTICAL, +10);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: servo command rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            }
            break;

        case 5: /* K5：水平舵机 -10° */
            err = servo_move_relative(SERVO_AXIS_HORIZONTAL, -10);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: servo command rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            }
            break;

        case 6: /* K6：水平舵机 +10° */
            err = servo_move_relative(SERVO_AXIS_HORIZONTAL, +10);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: servo command rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            }
            break;

        default:
            break;
        }

        /* ── 所有事件转发给 LED 队列（保留按键闪灯）──── */
        if (led_queue != NULL) {
            xQueueSend(led_queue, &msg, 0);
        }
    }
}

/* ── app_main ─────────────────────────────────────────── */
void app_main(void)
{
    esp_err_t err;

    /* 1. 分配 PSRAM 音频缓冲区 */
    s_audio_buf.buffer = (int16_t *)heap_caps_malloc(
        AUDIO_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_audio_buf.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffer (%u bytes)",
                 (unsigned)AUDIO_BUF_BYTES);
        return;
    }
    s_audio_buf.total_size          = AUDIO_BUF_BYTES;
    s_audio_buf.current_write_pos   = 0;
    s_audio_buf.current_read_pos    = 0;
    s_audio_buf.recorded_size       = 0;
    s_audio_buf.recording           = false;
    s_audio_buf.playing             = false;
    s_audio_buf.recording_complete  = false;
    ESP_LOGI(TAG, "PSRAM buffer allocated: %u bytes", (unsigned)AUDIO_BUF_BYTES);

    /* OLED display is non-critical: log failures and keep audio/key features running. */
    err = display_init();
    if (err == ESP_OK) {
        display_attach_audio_buffer(&s_audio_buf, AUDIO_SAMPLE_RATE);
        err = display_start_task();
        if (err == ESP_OK) {
            display_set_idle();
        } else {
            ESP_LOGW(TAG, "display_start_task: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "display_init: %s", esp_err_to_name(err));
    }

    err = wifi_network_init();
    if (err == ESP_OK) {
        err = wifi_network_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_network_start: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "wifi_network_init: %s", esp_err_to_name(err));
    }

    err = voice_assistant_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice_assistant_init: %s", esp_err_to_name(err));
    }

    /* 2. LED 呼吸灯 */
    err = led_start_breath_effect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_start_breath_effect: %s", esp_err_to_name(err));
        return;
    }

    /* 3. 中转队列 + 按键分发任务 */
    s_main_key_queue = xQueueCreate(MAIN_KEY_QUEUE_LEN,
                                    sizeof(led_key_event_message_t));
    if (s_main_key_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create main key queue");
        return;
    }

    /* 把 LED 队列句柄传给分发任务，用于事件转发 */
    if (xTaskCreate(main_key_task, "main_key_task", 3072,
                    (void *)led_get_key_event_queue_handle(),
                    5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create main_key_task");
        return;
    }

    /* 4. 按键模块：事件投递到中转队列 */
    err = key_init(s_main_key_queue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key_init: %s", esp_err_to_name(err));
    }

    /* 5. 麦克风（I2S RX） */
    err = audio_mic_init(&s_audio_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_mic_init: %s", esp_err_to_name(err));
    }

    /* 6. 扬声器（I2S TX） */
    err = audio_spk_init(&s_audio_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_spk_init: %s", esp_err_to_name(err));
    }

    /* 7. 双舵机（LEDC PWM） */
    err = servo_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo_init: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG,
             "System ready  |  K1=录音开/停  K2=播放开/停  K3/K4=垂直-/+  K5/K6=水平-/+  K8=音量切换");
}

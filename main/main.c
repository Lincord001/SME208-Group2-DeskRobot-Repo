/*
 * main.c
 *
 * 功能：
 *   - 分配 PSRAM 音频缓冲区（20 秒 × 24kHz × 16-bit mono = 960 KB）
 *   - 初始化 LED 呼吸灯、按键、麦克风（I2S RX）、扬声器（I2S TX）
 *   - 按键事件通过中转队列分发：
 *       K1 → 切换录音 开/停
 *       K2 → 切换播放 开/停
 *       K4 → 进入时钟模式
 *       K5 → 进入第二级低功耗
 *       K6 → 进入 Wi-Fi 配置
 *       K7 → 语音测试；长按进入第二级低功耗
 *       K8 → 显示运行状态页；长按进入 Wi-Fi 配置
 *       所有事件同步转发给 LED 队列（保持按键闪灯效果）
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#include "audio_mic.h"
#include "audio_spk.h"
#include "display.h"
#include "key.h"
#include "led.h"
#include "power_manager.h"
#include "servo.h"
#include "system_status.h"
#include "voice_assistant.h"
#include "wifi_network.h"

static const char *TAG = "MAIN";

/* ── 音频缓冲区参数 ──────────────────────────────────────
 * 24 kHz × 16-bit × 1 ch × 20 s = 960 000 字节
 * 向上对齐到 4 字节边界                                    */
#define AUDIO_SAMPLE_RATE  24000
#define AUDIO_MAX_SECS     20
#define AUDIO_BUF_BYTES    (AUDIO_SAMPLE_RATE * sizeof(int16_t) * AUDIO_MAX_SECS)
#define POWER_MIN_CPU_FREQ_MHZ 80
#define CLOCK_MODE_KEY_ID 4
#define CLOCK_EXIT_GRACE_MS 700

/* ── 中转队列 ────────────────────────────────────────────
 * main_key_task 从此队列读取按键事件，按 key_id 分发后
 * 再将事件转发给 LED 队列                                  */
#define MAIN_KEY_QUEUE_LEN 16

static audio_buffer_t s_audio_buf;
static QueueHandle_t  s_main_key_queue;
static TickType_t     s_clock_mode_enter_tick;

static void main_configure_power_management(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = POWER_MIN_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Dynamic frequency scaling enabled: %u-%u MHz, light sleep off",
             (unsigned)pm_config.min_freq_mhz,
             (unsigned)pm_config.max_freq_mhz);
#else
    ESP_LOGW(TAG, "Dynamic frequency scaling disabled: CONFIG_PM_ENABLE is not set");
#endif
}

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

        if (power_manager_is_low_power()) {
            power_manager_notify_activity();
            ESP_LOGI(TAG, "K%u: wake from low power", msg.key_id);
            continue;
        }

        power_manager_notify_activity();

        if (display_get_state() == DISPLAY_STATE_CLOCK) {
            TickType_t held_ticks = xTaskGetTickCount() - s_clock_mode_enter_tick;
            if (!(msg.key_id == CLOCK_MODE_KEY_ID &&
                  held_ticks < pdMS_TO_TICKS(CLOCK_EXIT_GRACE_MS))) {
                display_set_clock_mode(false);
                ESP_LOGI(TAG, "Clock mode exited by K%u", msg.key_id);
            }
            if (led_queue != NULL) {
                xQueueSend(led_queue, &msg, 0);
            }
            continue;
        }

        /* ── 音频控制逻辑（不阻塞，仅设置标志位）─────── */
        audio_buffer_state_t audio_state;
        audio_buffer_get_state(&s_audio_buf, &audio_state);

        switch (msg.key_id) {
        case 1: /* K1：录音 开/停 */
            if (!audio_state.recording) {
                if (audio_state.playing) {
                    ESP_LOGW(TAG, "K1: stop playback first");
                } else {
                    audio_mic_set_recording(true);
                    display_set_listening(0);
                    err = servo_start_listening_motion();
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "K1: listening servo motion rejected (%s)",
                                 esp_err_to_name(err));
                    }
                    ESP_LOGI(TAG, "K1: recording started");
                }
            } else {
                audio_mic_set_recording(false);
                servo_stop_listening_motion();
                display_set_status("Recorded", "Press K7");
                ESP_LOGI(TAG, "K1: recording stopped");
            }
            break;

        case 2: /* K2：播放 开/停 */
            if (!audio_state.playing) {
                if (audio_state.recording) {
                    ESP_LOGW(TAG, "K2: stop recording first");
                } else {
                    esp_err_t play_err = audio_spk_set_playing(true);
                    if (play_err == ESP_OK) {
                        audio_buffer_get_state(&s_audio_buf, &audio_state);
                        uint32_t total_sec = (uint32_t)((audio_state.recorded_size / sizeof(int16_t)) /
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

        case 8: /* K8: status page; long press starts WiFi config mode */
            if (msg.event == LED_KEY_EVENT_LONG_PRESS_START) {
                err = wifi_network_start_config_mode(false);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "K8: WiFi config mode rejected (%s)", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "K8: WiFi config mode started");
                }
            } else {
                system_status_show_next_page();
                system_status_log_snapshot();
                ESP_LOGI(TAG, "K8: status page switched");
            }
            break;

        case 7: /* K7: voice test; long press enters stage-2 low power */
            if (msg.event == LED_KEY_EVENT_LONG_PRESS_START) {
                err = power_manager_enter_stage2();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "K7: stage-2 low power rejected (%s)", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "K7: stage-2 low power entered");
                }
            } else {
                if (audio_state.recording_complete && audio_state.recorded_size > 0 &&
                    !audio_state.recording && !audio_state.playing) {
                    err = voice_assistant_start_full_test(&s_audio_buf, AUDIO_SAMPLE_RATE);
                } else {
                    err = voice_assistant_start_tts_test(&s_audio_buf);
                }
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "K7: voice test rejected (%s)", esp_err_to_name(err));
                }
            }
            break;

        case 3: /* K3：垂直舵机 -10° */
            err = servo_move_relative(SERVO_AXIS_VERTICAL, -10);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: servo command rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            }
            break;

        case 4: /* K4：进入时钟模式 */
            s_clock_mode_enter_tick = xTaskGetTickCount();
            display_set_clock_mode(true);
            ESP_LOGI(TAG, "K%u: clock mode entered", msg.key_id);
            break;

        case 5: /* K5：进入第二级低功耗 */
            err = power_manager_enter_stage2();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: stage-2 low power rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "K%u: stage-2 low power entered", msg.key_id);
            }
            break;

        case 6: /* K6：进入 Wi-Fi 配置 */
            err = wifi_network_start_config_mode(false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "K%u: WiFi config mode rejected (%s)",
                         msg.key_id, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "K%u: WiFi config mode started", msg.key_id);
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

    main_configure_power_management();

    err = system_status_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "system_status_init: %s", esp_err_to_name(err));
    }

    /* 1. 分配 PSRAM 音频缓冲区 */
    s_audio_buf.buffer = (int16_t *)heap_caps_malloc(
        AUDIO_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_audio_buf.buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffer (%u bytes)",
                 (unsigned)AUDIO_BUF_BYTES);
        return;
    }
    audio_buffer_init(&s_audio_buf, s_audio_buf.buffer, AUDIO_BUF_BYTES);
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

    err = power_manager_init(&s_audio_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power_manager_init: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG,
             "System ready  |  K1=录音开/停  K2=播放开/停  K3=垂直-  K4=时钟模式  K5=二级低功耗  K6=WiFi配置  K7=语音测试  K7长按=二级低功耗  K8=状态页  K8长按=WiFi配置");
}

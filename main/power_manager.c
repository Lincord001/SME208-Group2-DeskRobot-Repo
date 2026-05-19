#include "power_manager.h"

#include <stdbool.h>

#include <esp_clk_tree.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#include "display.h"
#include "led.h"
#include "servo.h"
#include "wifi_network.h"

#define POWER_STAGE1_IDLE_MS 30000
#define POWER_STAGE2_IDLE_MS 60000
#define POWER_IDLE_TIMEOUT_MIN_MS 1000
#define POWER_TASK_STACK     3072
#define POWER_TASK_PRIO      3
#define POWER_POLL_MS        250
#define POWER_OBSERVE_LOG_MS 30000
#define POWER_MIN_CPU_FREQ_MHZ 80

typedef enum {
    POWER_STAGE_AWAKE = 0,
    POWER_STAGE_SLEEPY,
    POWER_STAGE_DISPLAY_OFF,
} power_stage_t;

static const char *TAG = "power_manager";

static audio_buffer_t *s_audio_buf;
static TaskHandle_t s_task_handle;
static volatile bool s_initialized;
static volatile power_stage_t s_stage = POWER_STAGE_AWAKE;
static TickType_t s_last_activity_tick;
static TickType_t s_last_observe_log_tick;
static uint32_t s_stage1_idle_ms = POWER_STAGE1_IDLE_MS;
static uint32_t s_stage2_idle_ms = POWER_STAGE2_IDLE_MS;
static bool s_cpu_low_freq_limited;

static const char *power_manager_stage_to_string(power_stage_t stage)
{
    switch (stage) {
    case POWER_STAGE_AWAKE:
        return "awake";
    case POWER_STAGE_SLEEPY:
        return "sleepy";
    case POWER_STAGE_DISPLAY_OFF:
        return "display off";
    default:
        return "unknown";
    }
}

static bool power_manager_can_sleep(void)
{
    if (s_audio_buf == NULL || display_get_state() != DISPLAY_STATE_IDLE) {
        return false;
    }

    audio_buffer_state_t audio_state;
    audio_buffer_get_state(s_audio_buf, &audio_state);
    return !audio_state.recording && !audio_state.playing;
}

static uint32_t power_manager_get_cpu_freq_mhz(void)
{
    uint32_t cpu_freq_hz = 0;
    esp_err_t err = esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                                 &cpu_freq_hz);
    if (err != ESP_OK) {
        return 0;
    }

    return cpu_freq_hz / 1000000U;
}

static const char *power_manager_display_state_to_string(display_state_t state)
{
    switch (state) {
    case DISPLAY_STATE_IDLE:
        return "idle";
    case DISPLAY_STATE_LISTENING:
        return "listening";
    case DISPLAY_STATE_THINKING:
        return "thinking";
    case DISPLAY_STATE_ANSWERING:
        return "answering";
    case DISPLAY_STATE_WIFI:
        return "wifi";
    case DISPLAY_STATE_STATUS:
        return "status";
    case DISPLAY_STATE_CLOCK:
        return "clock";
    case DISPLAY_STATE_CINNAMOROLL:
        return "cinnamoroll";
    case DISPLAY_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static esp_err_t power_manager_apply_cpu_limit(bool enable)
{
#if CONFIG_PM_ENABLE
    if (s_cpu_low_freq_limited == enable) {
        return ESP_OK;
    }

    esp_pm_config_t pm_config = {
        .max_freq_mhz = enable ? POWER_MIN_CPU_FREQ_MHZ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = POWER_MIN_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };

    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU low frequency limit %s failed: %s",
                 enable ? "enable" : "disable",
                 esp_err_to_name(err));
        return err;
    }

    s_cpu_low_freq_limited = enable;
    ESP_LOGI(TAG, "CPU frequency range set to %u-%u MHz",
             (unsigned)pm_config.min_freq_mhz,
             (unsigned)pm_config.max_freq_mhz);
    return ESP_OK;
#else
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void power_manager_log_observability(const char *reason)
{
    audio_buffer_state_t audio_state = {0};
    if (s_audio_buf != NULL) {
        audio_buffer_get_state(s_audio_buf, &audio_state);
    }

    ESP_LOGI(TAG,
             "Power observe (%s): stage=%s display=%s wifi=%s wifi_ps=%s cpu=%luMHz cpu_limit=%s pm=%s(%u-%uMHz) audio=rec:%u play:%u complete:%u bytes:%u/%u heap=%u psram=%u",
             reason != NULL ? reason : "snapshot",
             power_manager_stage_to_string(s_stage),
             power_manager_display_state_to_string(display_get_state()),
             wifi_network_get_status_string(),
             wifi_network_is_power_save_enabled() ? "on" : "off",
             (unsigned long)power_manager_get_cpu_freq_mhz(),
             s_cpu_low_freq_limited ? "80MHz" : "auto",
#if CONFIG_PM_ENABLE
             "on",
#else
             "off",
#endif
             (unsigned)POWER_MIN_CPU_FREQ_MHZ,
             (unsigned)(s_cpu_low_freq_limited ? POWER_MIN_CPU_FREQ_MHZ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
             audio_state.recording ? 1U : 0U,
             audio_state.playing ? 1U : 0U,
             audio_state.recording_complete ? 1U : 0U,
             (unsigned)audio_state.recorded_size,
             (unsigned)audio_state.total_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void power_manager_set_stage(power_stage_t stage)
{
    if (s_stage == stage) {
        return;
    }

    switch (stage) {
    case POWER_STAGE_AWAKE:
        (void)power_manager_apply_cpu_limit(false);
        servo_center();
        (void)wifi_network_set_power_save(false);
        display_set_panel_power(true);
        display_set_low_power_overlay(false);
        led_set_low_power(false);
        ESP_LOGI(TAG, "Power stage: awake");
        break;

    case POWER_STAGE_SLEEPY:
        servo_center();
        led_set_low_power(true);
        display_set_panel_power(true);
        display_set_low_power_overlay(true);
        ESP_LOGI(TAG, "Power stage: sleepy overlay");
        break;

    case POWER_STAGE_DISPLAY_OFF:
        servo_look_down();
        display_set_low_power_overlay(false);
        display_set_panel_power(false);
        led_set_low_power(true);
        if (wifi_network_is_connected()) {
            (void)wifi_network_set_power_save(true);
        }
        (void)power_manager_apply_cpu_limit(true);
        ESP_LOGI(TAG, "Power stage: display off");
        break;
    }

    s_stage = stage;
    power_manager_log_observability("stage-change");
    s_last_observe_log_tick = xTaskGetTickCount();
}

static void power_manager_task(void *arg)
{
    (void)arg;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        TickType_t idle_ticks = now - s_last_activity_tick;

        if (!power_manager_can_sleep()) {
            s_last_activity_tick = now;
            if (s_stage != POWER_STAGE_AWAKE) {
                power_manager_set_stage(POWER_STAGE_AWAKE);
            }
        } else if (idle_ticks >= pdMS_TO_TICKS(s_stage2_idle_ms)) {
            power_manager_set_stage(POWER_STAGE_DISPLAY_OFF);
        } else if (idle_ticks >= pdMS_TO_TICKS(s_stage1_idle_ms) &&
                   s_stage != POWER_STAGE_DISPLAY_OFF) {
            power_manager_set_stage(POWER_STAGE_SLEEPY);
        }

        if (s_stage != POWER_STAGE_AWAKE &&
            now - s_last_observe_log_tick >= pdMS_TO_TICKS(POWER_OBSERVE_LOG_MS)) {
            power_manager_log_observability("periodic");
            s_last_observe_log_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    }
}

esp_err_t power_manager_init(audio_buffer_t *audio_buffer)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (audio_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_audio_buf = audio_buffer;
    s_last_activity_tick = xTaskGetTickCount();
    s_last_observe_log_tick = s_last_activity_tick;

    if (xTaskCreate(power_manager_task,
                    "power_manager",
                    POWER_TASK_STACK,
                    NULL,
                    POWER_TASK_PRIO,
                    &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized: stage1=%u ms stage2=%u ms",
             (unsigned)s_stage1_idle_ms, (unsigned)s_stage2_idle_ms);
    power_manager_log_observability("init");
    return ESP_OK;
}

void power_manager_notify_activity(void)
{
    if (!s_initialized) {
        return;
    }

    s_last_activity_tick = xTaskGetTickCount();
    if (s_stage != POWER_STAGE_AWAKE) {
        power_manager_set_stage(POWER_STAGE_AWAKE);
    }
}

esp_err_t power_manager_enter_stage1(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!power_manager_can_sleep()) {
        return ESP_ERR_INVALID_STATE;
    }

    power_manager_set_stage(POWER_STAGE_SLEEPY);
    return ESP_OK;
}

esp_err_t power_manager_enter_stage2(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!power_manager_can_sleep()) {
        return ESP_ERR_INVALID_STATE;
    }

    power_manager_set_stage(POWER_STAGE_DISPLAY_OFF);
    return ESP_OK;
}

esp_err_t power_manager_set_idle_timeouts_ms(uint32_t stage1_ms,
                                             uint32_t stage2_ms)
{
    if (stage1_ms < POWER_IDLE_TIMEOUT_MIN_MS ||
        stage2_ms < POWER_IDLE_TIMEOUT_MIN_MS ||
        stage1_ms >= stage2_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    s_stage1_idle_ms = stage1_ms;
    s_stage2_idle_ms = stage2_ms;
    power_manager_notify_activity();
    ESP_LOGI(TAG, "Idle timeouts updated: stage1=%u ms stage2=%u ms",
             (unsigned)s_stage1_idle_ms, (unsigned)s_stage2_idle_ms);
    return ESP_OK;
}

void power_manager_get_idle_timeouts_ms(uint32_t *out_stage1_ms,
                                        uint32_t *out_stage2_ms)
{
    if (out_stage1_ms != NULL) {
        *out_stage1_ms = s_stage1_idle_ms;
    }
    if (out_stage2_ms != NULL) {
        *out_stage2_ms = s_stage2_idle_ms;
    }
}

bool power_manager_is_low_power(void)
{
    return s_stage != POWER_STAGE_AWAKE;
}

const char *power_manager_get_stage_string(void)
{
    return power_manager_stage_to_string(s_stage);
}

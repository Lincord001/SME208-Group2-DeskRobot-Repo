#include "system_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "display.h"
#include "power_manager.h"
#include "wifi_network.h"

#define STATUS_REASON_LEN 24
#define STATUS_PAGE_COUNT 5

typedef struct {
    int64_t start_us;
    uint32_t last_ms;
    esp_err_t last_err;
    char reason[STATUS_REASON_LEN];
    bool running;
    bool seen;
} system_status_voice_t;

typedef struct {
    system_status_voice_t voice[SYSTEM_STATUS_VOICE_STAGE_COUNT];
    uint8_t page;
} system_status_t;

static const char *TAG = "system_status";
static SemaphoreHandle_t s_mutex;
static system_status_t s_status;

static const char *const s_stage_names[SYSTEM_STATUS_VOICE_STAGE_COUNT] = {
    "ASR",
    "LLM",
    "TTS",
};

static const char *system_status_err_text(const system_status_voice_t *voice)
{
    if (voice->running) {
        return "running";
    }
    if (!voice->seen) {
        return "n/a";
    }
    if (voice->last_err == ESP_OK) {
        return "ok";
    }
    if (voice->reason[0] != '\0') {
        return voice->reason;
    }
    return esp_err_to_name(voice->last_err);
}

static void system_status_format_voice(system_status_voice_stage_t stage,
                                       const system_status_voice_t *voice,
                                       char *title,
                                       size_t title_len,
                                       char *detail,
                                       size_t detail_len)
{
    uint32_t elapsed_ms = voice->last_ms;
    if (voice->running && voice->start_us > 0) {
        elapsed_ms = (uint32_t)((esp_timer_get_time() - voice->start_us) / 1000);
    }

    snprintf(title, title_len, "%s %lums",
             s_stage_names[stage],
             (unsigned long)elapsed_ms);
    snprintf(detail, detail_len, "%s", system_status_err_text(voice));
}

esp_err_t system_status_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < SYSTEM_STATUS_VOICE_STAGE_COUNT; ++i) {
        s_status.voice[i].last_err = ESP_ERR_INVALID_STATE;
        snprintf(s_status.voice[i].reason, sizeof(s_status.voice[i].reason), "n/a");
    }

    ESP_LOGI(TAG, "Initialized runtime status");
    return ESP_OK;
}

void system_status_voice_begin(system_status_voice_stage_t stage)
{
    if (stage >= SYSTEM_STATUS_VOICE_STAGE_COUNT || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        system_status_voice_t *voice = &s_status.voice[stage];
        voice->start_us = esp_timer_get_time();
        voice->last_ms = 0;
        voice->last_err = ESP_OK;
        voice->reason[0] = '\0';
        voice->running = true;
        voice->seen = true;
        xSemaphoreGive(s_mutex);
    }
}

void system_status_voice_end(system_status_voice_stage_t stage,
                             esp_err_t err,
                             const char *reason)
{
    if (stage >= SYSTEM_STATUS_VOICE_STAGE_COUNT || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        system_status_voice_t *voice = &s_status.voice[stage];
        int64_t now_us = esp_timer_get_time();
        if (voice->running && voice->start_us > 0) {
            voice->last_ms = (uint32_t)((now_us - voice->start_us) / 1000);
        } else {
            voice->last_ms = 0;
        }
        voice->last_err = err;
        snprintf(voice->reason, sizeof(voice->reason), "%s",
                 reason != NULL ? reason : (err == ESP_OK ? "ok" : esp_err_to_name(err)));
        voice->start_us = 0;
        voice->running = false;
        voice->seen = true;
        xSemaphoreGive(s_mutex);
    }
}

void system_status_show_next_page(void)
{
    if (s_mutex == NULL) {
        return;
    }

    char title[24];
    char detail[32];
    system_status_voice_t voice[SYSTEM_STATUS_VOICE_STAGE_COUNT];
    uint8_t page;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    page = s_status.page;
    memcpy(voice, s_status.voice, sizeof(voice));
    s_status.page = (uint8_t)((s_status.page + 1U) % STATUS_PAGE_COUNT);
    xSemaphoreGive(s_mutex);

    switch (page) {
    case 0:
        snprintf(title, sizeof(title), "WiFi %s",
                 wifi_network_is_connected() ? "connected" : "offline");
        snprintf(detail, sizeof(detail), "Pwr %s", power_manager_get_stage_string());
        break;

    case 1:
        snprintf(title, sizeof(title), "Heap %luK",
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));
        snprintf(detail, sizeof(detail), "PSRAM %luK",
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
        break;

    case 2:
        system_status_format_voice(SYSTEM_STATUS_VOICE_ASR, &voice[SYSTEM_STATUS_VOICE_ASR],
                                   title, sizeof(title), detail, sizeof(detail));
        break;

    case 3:
        system_status_format_voice(SYSTEM_STATUS_VOICE_LLM, &voice[SYSTEM_STATUS_VOICE_LLM],
                                   title, sizeof(title), detail, sizeof(detail));
        break;

    case 4:
    default:
        system_status_format_voice(SYSTEM_STATUS_VOICE_TTS, &voice[SYSTEM_STATUS_VOICE_TTS],
                                   title, sizeof(title), detail, sizeof(detail));
        break;
    }

    display_set_status(title, detail);
    ESP_LOGI(TAG, "Status page: %s | %s", title, detail);
}

void system_status_log_snapshot(void)
{
    if (s_mutex == NULL) {
        return;
    }

    system_status_voice_t voice[SYSTEM_STATUS_VOICE_STAGE_COUNT];
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    memcpy(voice, s_status.voice, sizeof(voice));
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Runtime: wifi=%s status=%s power=%s heap=%u psram=%u",
             wifi_network_is_connected() ? "connected" : "offline",
             wifi_network_get_status_string(),
             power_manager_get_stage_string(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (size_t i = 0; i < SYSTEM_STATUS_VOICE_STAGE_COUNT; ++i) {
        ESP_LOGI(TAG, "Runtime: %s ms=%u result=%s",
                 s_stage_names[i],
                 (unsigned)voice[i].last_ms,
                 system_status_err_text(&voice[i]));
    }
}

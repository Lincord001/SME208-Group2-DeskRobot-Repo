#include "voice_assistant.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "asr_client.h"
#include "display.h"
#include "llm_client.h"
#include "servo.h"
#include "system_status.h"
#include "tts_client.h"
#include "wifi_network.h"

#define VOICE_LLM_TASK_STACK_SIZE 8192
#define VOICE_ASR_TASK_STACK_SIZE 8192
#define VOICE_TTS_TASK_STACK_SIZE 8192
#define VOICE_FULL_TASK_STACK_SIZE 10240

static const char *TAG = "voice_assistant";
static TaskHandle_t s_llm_task;
static TaskHandle_t s_asr_task;
static TaskHandle_t s_tts_task;
static TaskHandle_t s_full_task;

typedef struct {
    const audio_buffer_t *audio_buffer;
    uint32_t sample_rate_hz;
} voice_asr_task_arg_t;

typedef struct {
    audio_buffer_t *audio_buffer;
} voice_tts_task_arg_t;

typedef struct {
    audio_buffer_t *audio_buffer;
    uint32_t sample_rate_hz;
} voice_full_task_arg_t;

static void voice_assistant_llm_test_task(void *arg)
{
    (void)arg;

    char reply[1024];
    const char *prompt = "你好，请用一句话介绍你自己。";

    if (!wifi_network_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected; skip LLM test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_LLM, ESP_ERR_INVALID_STATE, "wifi offline");
        display_set_status("Network", "Not connected");
        s_llm_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    display_set_thinking(0);
    ESP_LOGI(TAG, "LLM test prompt: %s", prompt);

    system_status_voice_begin(SYSTEM_STATUS_VOICE_LLM);
    esp_err_t err = llm_client_chat(prompt, reply, sizeof(reply));
    system_status_voice_end(SYSTEM_STATUS_VOICE_LLM, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LLM reply: %s", reply);
        display_set_idle();
        display_set_wifi_status("LLM done");
    } else {
        ESP_LOGE(TAG, "LLM test failed: %s", esp_err_to_name(err));
        display_set_error();
    }

    s_llm_task = NULL;
    vTaskDelete(NULL);
}

static void voice_assistant_asr_test_task(void *arg)
{
    voice_asr_task_arg_t *task_arg = (voice_asr_task_arg_t *)arg;
    const audio_buffer_t *audio_buffer = task_arg->audio_buffer;
    uint32_t sample_rate_hz = task_arg->sample_rate_hz;
    free(task_arg);

    char transcript[512];

    if (!wifi_network_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected; skip ASR test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_INVALID_STATE, "wifi offline");
        display_set_status("Network", "Not connected");
        s_asr_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    audio_buffer_state_t audio_state = {0};
    if (audio_buffer != NULL) {
        audio_buffer_get_state(audio_buffer, &audio_state);
    }

    if (audio_buffer == NULL ||
        audio_buffer->buffer == NULL ||
        !audio_state.recording_complete ||
        audio_state.recorded_size == 0 ||
        audio_state.recording ||
        audio_state.playing) {
        ESP_LOGW(TAG, "No stable recorded audio for ASR test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_INVALID_ARG, "no audio");
        display_set_error();
        s_asr_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t sample_count = audio_state.recorded_size / sizeof(int16_t);
    ESP_LOGI(TAG, "ASR test: samples=%u sample_rate=%lu",
             (unsigned)sample_count,
             (unsigned long)sample_rate_hz);
    display_set_idle();
    display_set_status("Recognizing", "ASR");

    system_status_voice_begin(SYSTEM_STATUS_VOICE_ASR);
    esp_err_t err = asr_client_recognize_pcm16(audio_buffer->buffer,
                                               sample_count,
                                               sample_rate_hz,
                                               transcript,
                                               sizeof(transcript));
    system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ASR transcript: %s", transcript);
        display_set_idle();
        display_set_status("Recognized", "ASR done");
    } else {
        ESP_LOGE(TAG, "ASR test failed: %s", esp_err_to_name(err));
        display_set_error();
    }

    s_asr_task = NULL;
    vTaskDelete(NULL);
}

static void voice_assistant_tts_test_task(void *arg)
{
    voice_tts_task_arg_t *task_arg = (voice_tts_task_arg_t *)arg;
    audio_buffer_t *audio_buffer = task_arg->audio_buffer;
    free(task_arg);

    size_t audio_bytes = 0;
    const char *text = "你好，我是桌面机器人。";

    if (!wifi_network_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected; skip TTS test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_TTS, ESP_ERR_INVALID_STATE, "wifi offline");
        display_set_status("Network", "Not connected");
        s_tts_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TTS test text: %s", text);
    display_set_idle();
    display_set_status("Generating", "TTS");

    system_status_voice_begin(SYSTEM_STATUS_VOICE_TTS);
    esp_err_t err = tts_client_synthesize_to_audio_buffer(text, audio_buffer, &audio_bytes);
    system_status_voice_end(SYSTEM_STATUS_VOICE_TTS, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS test done, received %u audio bytes; press K2 to play when speaker is available",
                 (unsigned)audio_bytes);
        display_set_idle();
        display_set_status("Voice ready", "Press K2");
    } else {
        ESP_LOGE(TAG, "TTS test failed: %s", esp_err_to_name(err));
        display_set_error();
    }

    s_tts_task = NULL;
    vTaskDelete(NULL);
}

static void voice_assistant_full_test_task(void *arg)
{
    voice_full_task_arg_t *task_arg = (voice_full_task_arg_t *)arg;
    audio_buffer_t *audio_buffer = task_arg->audio_buffer;
    uint32_t sample_rate_hz = task_arg->sample_rate_hz;
    free(task_arg);

    int16_t *pcm_copy = NULL;
    char transcript[512];
    char reply[1024];
    bool servo_orbit_started = false;
    esp_err_t err = ESP_OK;

    if (!wifi_network_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected; skip full voice test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_INVALID_STATE, "wifi offline");
        display_set_status("Network", "Not connected");
        goto done;
    }

    audio_buffer_state_t audio_state = {0};
    if (audio_buffer != NULL) {
        audio_buffer_get_state(audio_buffer, &audio_state);
    }

    if (audio_buffer == NULL ||
        audio_buffer->buffer == NULL ||
        !audio_state.recording_complete ||
        audio_state.recorded_size == 0 ||
        audio_state.recording ||
        audio_state.playing) {
        ESP_LOGW(TAG, "No stable recorded audio for full voice test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_INVALID_ARG, "no audio");
        display_set_error();
        goto done;
    }

    size_t recorded_size = audio_state.recorded_size;
    size_t sample_count = recorded_size / sizeof(int16_t);
    pcm_copy = (int16_t *)heap_caps_malloc(recorded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm_copy == NULL) {
        pcm_copy = (int16_t *)malloc(recorded_size);
    }
    if (pcm_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate ASR PCM copy (%u bytes)", (unsigned)recorded_size);
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_NO_MEM, "no mem");
        display_set_error();
        goto done;
    }

    memcpy(pcm_copy, audio_buffer->buffer, recorded_size);

    err = servo_start_orbit_motion();
    if (err == ESP_OK) {
        servo_orbit_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start servo orbit motion: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Full voice test: ASR samples=%u sample_rate=%lu",
             (unsigned)sample_count,
             (unsigned long)sample_rate_hz);
    display_set_idle();
    display_set_status("Recognizing", "ASR");

    system_status_voice_begin(SYSTEM_STATUS_VOICE_ASR);
    err = asr_client_recognize_pcm16(pcm_copy,
                                     sample_count,
                                     sample_rate_hz,
                                     transcript,
                                     sizeof(transcript));
    system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice ASR failed: %s", esp_err_to_name(err));
        display_set_error();
        goto done;
    }

    ESP_LOGI(TAG, "Full voice ASR transcript: %s", transcript);
    display_set_thinking(0);

    system_status_voice_begin(SYSTEM_STATUS_VOICE_LLM);
    err = llm_client_chat(transcript, reply, sizeof(reply));
    system_status_voice_end(SYSTEM_STATUS_VOICE_LLM, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice LLM failed: %s", esp_err_to_name(err));
        display_set_error();
        goto done;
    }

    ESP_LOGI(TAG, "Full voice LLM reply: %s", reply);
    display_set_idle();
    display_set_status("Generating", "TTS");

    size_t tts_audio_bytes = 0;
    system_status_voice_begin(SYSTEM_STATUS_VOICE_TTS);
    err = tts_client_synthesize_to_audio_buffer(reply, audio_buffer, &tts_audio_bytes);
    system_status_voice_end(SYSTEM_STATUS_VOICE_TTS, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice TTS failed: %s", esp_err_to_name(err));
        display_set_error();
        goto done;
    }

    ESP_LOGI(TAG, "Full voice test done, TTS audio bytes=%u; press K2 to play when speaker is available",
             (unsigned)tts_audio_bytes);
    display_set_idle();
    display_set_status("Voice ready", "Press K2");

done:
    if (servo_orbit_started) {
        servo_stop_orbit_motion();
    }
    free(pcm_copy);
    s_full_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_assistant_init(void)
{
    esp_err_t err = llm_client_init();
    if (err != ESP_OK) {
        return err;
    }

    err = asr_client_init();
    if (err != ESP_OK) {
        return err;
    }

    return tts_client_init();
}

esp_err_t voice_assistant_start_llm_test(void)
{
    if (s_llm_task != NULL) {
        ESP_LOGW(TAG, "LLM test is already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(voice_assistant_llm_test_task,
                    "llm_test",
                    VOICE_LLM_TASK_STACK_SIZE,
                    NULL,
                    4,
                    &s_llm_task) != pdPASS) {
        s_llm_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t voice_assistant_start_tts_test(audio_buffer_t *audio_buffer)
{
    if (s_tts_task != NULL) {
        ESP_LOGW(TAG, "TTS test is already running");
        return ESP_ERR_INVALID_STATE;
    }

    voice_tts_task_arg_t *task_arg = (voice_tts_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task_arg->audio_buffer = audio_buffer;

    if (xTaskCreate(voice_assistant_tts_test_task,
                    "tts_test",
                    VOICE_TTS_TASK_STACK_SIZE,
                    task_arg,
                    4,
                    &s_tts_task) != pdPASS) {
        free(task_arg);
        s_tts_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t voice_assistant_start_full_test(audio_buffer_t *audio_buffer,
                                          uint32_t sample_rate_hz)
{
    if (s_full_task != NULL) {
        ESP_LOGW(TAG, "Full voice test is already running");
        return ESP_ERR_INVALID_STATE;
    }

    voice_full_task_arg_t *task_arg = (voice_full_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task_arg->audio_buffer = audio_buffer;
    task_arg->sample_rate_hz = sample_rate_hz;

    if (xTaskCreate(voice_assistant_full_test_task,
                    "voice_full",
                    VOICE_FULL_TASK_STACK_SIZE,
                    task_arg,
                    4,
                    &s_full_task) != pdPASS) {
        free(task_arg);
        s_full_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


esp_err_t voice_assistant_start_asr_test(const audio_buffer_t *audio_buffer,
                                         uint32_t sample_rate_hz)
{
    if (s_asr_task != NULL) {
        ESP_LOGW(TAG, "ASR test is already running");
        return ESP_ERR_INVALID_STATE;
    }

    voice_asr_task_arg_t *task_arg = (voice_asr_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    task_arg->audio_buffer = audio_buffer;
    task_arg->sample_rate_hz = sample_rate_hz;

    if (xTaskCreate(voice_assistant_asr_test_task,
                    "asr_test",
                    VOICE_ASR_TASK_STACK_SIZE,
                    task_arg,
                    4,
                    &s_asr_task) != pdPASS) {
        free(task_arg);
        s_asr_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

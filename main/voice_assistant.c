#include "voice_assistant.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#include "api_config.h"
#include "asr_client.h"
#include "audio_spk.h"
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
#define VOICE_TTS_SAMPLE_RATE_HZ 16000

static const char *TAG = "voice_assistant";
static TaskHandle_t s_llm_task;
static TaskHandle_t s_asr_task;
static TaskHandle_t s_tts_task;
static TaskHandle_t s_full_task;
static SemaphoreHandle_t s_voice_busy_sem;
static volatile bool s_voice_cancel_requested;

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

static BaseType_t voice_assistant_create_task(TaskFunction_t task_func,
                                              const char *name,
                                              uint32_t stack_size,
                                              void *arg,
                                              TaskHandle_t *task_handle)
{
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    BaseType_t created = xTaskCreateWithCaps(task_func,
                                             name,
                                             stack_size,
                                             arg,
                                             4,
                                             task_handle,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created == pdPASS) {
        return pdPASS;
    }

    ESP_LOGW(TAG,
             "%s task PSRAM stack allocation failed; retrying with internal stack",
             name);
#else
    BaseType_t created;
#endif

    created = xTaskCreate(task_func,
                          name,
                          stack_size,
                          arg,
                          4,
                          task_handle);
    if (created == pdPASS) {
        return pdPASS;
    }

    ESP_LOGE(TAG,
             "%s task create failed, free internal=%u, free psram=%u",
             name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return created;
}

static esp_err_t voice_assistant_ensure_busy_sem(void)
{
    if (s_voice_busy_sem != NULL) {
        return ESP_OK;
    }

    s_voice_busy_sem = xSemaphoreCreateBinary();
    if (s_voice_busy_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create voice busy semaphore");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_voice_busy_sem);
    return ESP_OK;
}

static esp_err_t voice_assistant_try_begin_operation(const char *name)
{
    esp_err_t err = voice_assistant_ensure_busy_sem();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_voice_busy_sem, 0) != pdTRUE) {
        ESP_LOGW(TAG, "%s skipped: another voice operation is running", name);
        return ESP_ERR_INVALID_STATE;
    }

    s_voice_cancel_requested = false;
    return ESP_OK;
}

static void voice_assistant_end_operation(void)
{
    if (s_voice_busy_sem != NULL) {
        xSemaphoreGive(s_voice_busy_sem);
    }
}

static bool voice_assistant_cancel_cb(void *ctx)
{
    (void)ctx;
    return s_voice_cancel_requested;
}

static const char *voice_assistant_error_detail(esp_err_t err)
{
    if (s_voice_cancel_requested) {
        return "Canceled";
    }
    if (!api_config_has_dashscope_api_key()) {
        return "No API key";
    }

    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "Request timeout";
    case ESP_ERR_NO_MEM:
        return "Out of memory";
    case ESP_ERR_INVALID_RESPONSE:
        return "Bad response";
    case ESP_ERR_INVALID_STATE:
        return "Busy or invalid";
    case ESP_ERR_INVALID_ARG:
        return "No audio";
    case ESP_ERR_NOT_SUPPORTED:
        return "Bad sample rate";
    default:
        return esp_err_to_name(err);
    }
}

static void voice_assistant_show_error(const char *title, esp_err_t err)
{
    if (s_voice_cancel_requested) {
        display_set_status("Canceled", "Voice stopped");
    } else {
        display_set_error_detail(title, voice_assistant_error_detail(err));
    }
}

static void voice_assistant_show_tts_ready(audio_buffer_t *audio_buffer)
{
    audio_buffer_state_t audio_state = {0};
    if (audio_buffer != NULL) {
        audio_buffer_get_state(audio_buffer, &audio_state);
    }

    if (audio_state.playing) {
        uint32_t current_sec = (uint32_t)(audio_state.current_read_pos /
                                          VOICE_TTS_SAMPLE_RATE_HZ);
        uint32_t total_sec = (uint32_t)((audio_state.recorded_size / sizeof(int16_t)) /
                                        VOICE_TTS_SAMPLE_RATE_HZ);
        display_set_answering(current_sec, total_sec);
    } else {
        display_set_idle();
        display_set_status("Voice ready", "Press K2");
    }
}

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
        voice_assistant_end_operation();
        vTaskDelete(NULL);
        return;
    }

    display_set_thinking(0);
    ESP_LOGI(TAG, "LLM test prompt: %s", prompt);

    system_status_voice_begin(SYSTEM_STATUS_VOICE_LLM);
    esp_err_t err = llm_client_chat_with_cancel(prompt,
                                                reply,
                                                sizeof(reply),
                                                voice_assistant_cancel_cb,
                                                NULL);
    system_status_voice_end(SYSTEM_STATUS_VOICE_LLM, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LLM reply: %s", reply);
        display_set_idle();
        display_set_wifi_status("LLM done");
    } else {
        ESP_LOGE(TAG, "LLM test failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("LLM failed", err);
    }

    s_llm_task = NULL;
    voice_assistant_end_operation();
    vTaskDelete(NULL);
}

static void voice_assistant_asr_test_task(void *arg)
{
    voice_asr_task_arg_t *task_arg = (voice_asr_task_arg_t *)arg;
    const audio_buffer_t *audio_buffer = task_arg->audio_buffer;
    uint32_t sample_rate_hz = task_arg->sample_rate_hz;
    free(task_arg);

    char transcript[512];
    bool servo_asr_started = false;

    if (!wifi_network_is_connected()) {
        ESP_LOGW(TAG, "WiFi is not connected; skip ASR test");
        system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, ESP_ERR_INVALID_STATE, "wifi offline");
        display_set_status("Network", "Not connected");
        s_asr_task = NULL;
        voice_assistant_end_operation();
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
        display_set_error_detail("ASR failed", "No audio");
        s_asr_task = NULL;
        voice_assistant_end_operation();
        vTaskDelete(NULL);
        return;
    }

    size_t sample_count = audio_state.recorded_size / sizeof(int16_t);
    ESP_LOGI(TAG, "ASR test: samples=%u sample_rate=%lu",
             (unsigned)sample_count,
             (unsigned long)sample_rate_hz);
    display_set_idle();
    display_set_status("Recognizing", "ASR");
    esp_err_t motion_err = servo_start_asr_motion();
    if (motion_err == ESP_OK) {
        servo_asr_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start servo ASR motion: %s",
                 esp_err_to_name(motion_err));
    }

    system_status_voice_begin(SYSTEM_STATUS_VOICE_ASR);
    esp_err_t err = asr_client_recognize_pcm16_with_cancel(audio_buffer->buffer,
                                                           sample_count,
                                                           sample_rate_hz,
                                                           transcript,
                                                           sizeof(transcript),
                                                           voice_assistant_cancel_cb,
                                                           NULL);
    if (servo_asr_started) {
        servo_stop_asr_motion();
    }
    system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ASR transcript: %s", transcript);
        display_set_idle();
        display_set_status("Recognized", "ASR done");
    } else {
        ESP_LOGE(TAG, "ASR test failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("ASR failed", err);
    }

    s_asr_task = NULL;
    voice_assistant_end_operation();
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
        voice_assistant_end_operation();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TTS test text: %s", text);
    display_set_idle();
    display_set_status("Generating", "TTS");

    system_status_voice_begin(SYSTEM_STATUS_VOICE_TTS);
    esp_err_t err = tts_client_synthesize_to_audio_buffer_with_cancel(text,
                                                                      audio_buffer,
                                                                      &audio_bytes,
                                                                      voice_assistant_cancel_cb,
                                                                      NULL);
    system_status_voice_end(SYSTEM_STATUS_VOICE_TTS, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS test done, received %u audio bytes; press K2 to play when speaker is available",
                 (unsigned)audio_bytes);
        voice_assistant_show_tts_ready(audio_buffer);
    } else {
        ESP_LOGE(TAG, "TTS test failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("TTS failed", err);
    }

    s_tts_task = NULL;
    voice_assistant_end_operation();
    vTaskDelete(NULL);
}

static void voice_assistant_full_test_task(void *arg)
{
    voice_full_task_arg_t *task_arg = (voice_full_task_arg_t *)arg;
    audio_buffer_t *audio_buffer = task_arg->audio_buffer;
    uint32_t sample_rate_hz = task_arg->sample_rate_hz;
    free(task_arg);

    char transcript[512];
    char reply[1024];
    bool servo_asr_started = false;
    bool servo_thinking_started = false;
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
        display_set_error_detail("ASR failed", "No audio");
        goto done;
    }

    size_t recorded_size = audio_state.recorded_size;
    size_t sample_count = recorded_size / sizeof(int16_t);

    ESP_LOGI(TAG, "Full voice test: ASR samples=%u sample_rate=%lu",
             (unsigned)sample_count,
             (unsigned long)sample_rate_hz);
    display_set_idle();
    display_set_status("Recognizing", "ASR");
    esp_err_t motion_err = servo_start_asr_motion();
    if (motion_err == ESP_OK) {
        servo_asr_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start servo ASR motion: %s",
                 esp_err_to_name(motion_err));
    }

    system_status_voice_begin(SYSTEM_STATUS_VOICE_ASR);
    err = asr_client_recognize_pcm16_with_cancel(audio_buffer->buffer,
                                                 sample_count,
                                                 sample_rate_hz,
                                                 transcript,
                                                 sizeof(transcript),
                                                 voice_assistant_cancel_cb,
                                                 NULL);
    if (servo_asr_started) {
        servo_stop_asr_motion();
        servo_asr_started = false;
    }
    system_status_voice_end(SYSTEM_STATUS_VOICE_ASR, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice ASR failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("ASR failed", err);
        goto done;
    }

    ESP_LOGI(TAG, "Full voice ASR transcript: %s", transcript);
    display_set_thinking(0);
    err = servo_start_thinking_motion();
    if (err == ESP_OK) {
        servo_thinking_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start servo thinking motion: %s", esp_err_to_name(err));
    }

    system_status_voice_begin(SYSTEM_STATUS_VOICE_LLM);
    err = llm_client_chat_with_cancel(transcript,
                                      reply,
                                      sizeof(reply),
                                      voice_assistant_cancel_cb,
                                      NULL);
    system_status_voice_end(SYSTEM_STATUS_VOICE_LLM, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice LLM failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("LLM failed", err);
        goto done;
    }

    ESP_LOGI(TAG, "Full voice LLM reply: %s", reply);
    display_set_idle();
    display_set_status("Generating", "TTS");

    size_t tts_audio_bytes = 0;
    system_status_voice_begin(SYSTEM_STATUS_VOICE_TTS);
    err = tts_client_synthesize_to_audio_buffer_with_cancel(reply,
                                                            audio_buffer,
                                                            &tts_audio_bytes,
                                                            voice_assistant_cancel_cb,
                                                            NULL);
    system_status_voice_end(SYSTEM_STATUS_VOICE_TTS, err, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Full voice TTS failed: %s", esp_err_to_name(err));
        voice_assistant_show_error("TTS failed", err);
        goto done;
    }

    ESP_LOGI(TAG, "Full voice test done, TTS audio bytes=%u; press K2 to play when speaker is available",
             (unsigned)tts_audio_bytes);
    voice_assistant_show_tts_ready(audio_buffer);

done:
    if (servo_asr_started) {
        servo_stop_asr_motion();
    }
    if (servo_thinking_started) {
        servo_stop_thinking_motion();
    }
    s_full_task = NULL;
    voice_assistant_end_operation();
    vTaskDelete(NULL);
}

esp_err_t voice_assistant_init(void)
{
    esp_err_t err = voice_assistant_ensure_busy_sem();
    if (err != ESP_OK) {
        return err;
    }

    err = llm_client_init();
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

    esp_err_t err = voice_assistant_try_begin_operation("LLM test");
    if (err != ESP_OK) {
        return err;
    }

    if (voice_assistant_create_task(voice_assistant_llm_test_task,
                                    "llm_test",
                                    VOICE_LLM_TASK_STACK_SIZE,
                                    NULL,
                                    &s_llm_task) != pdPASS) {
        s_llm_task = NULL;
        voice_assistant_end_operation();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool voice_assistant_is_busy(void)
{
    if (s_voice_busy_sem == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_voice_busy_sem, 0) == pdTRUE) {
        xSemaphoreGive(s_voice_busy_sem);
        return false;
    }

    return true;
}

esp_err_t voice_assistant_cancel_current(void)
{
    if (!voice_assistant_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_voice_cancel_requested = true;
    (void)audio_spk_set_playing(false);
    display_set_status("Canceling", "Voice task");
    ESP_LOGW(TAG, "Voice operation cancel requested");
    return ESP_OK;
}

esp_err_t voice_assistant_start_tts_test(audio_buffer_t *audio_buffer)
{
    if (s_tts_task != NULL) {
        ESP_LOGW(TAG, "TTS test is already running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = voice_assistant_try_begin_operation("TTS test");
    if (err != ESP_OK) {
        return err;
    }

    voice_tts_task_arg_t *task_arg = (voice_tts_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        voice_assistant_end_operation();
        return ESP_ERR_NO_MEM;
    }
    task_arg->audio_buffer = audio_buffer;

    if (voice_assistant_create_task(voice_assistant_tts_test_task,
                                    "tts_test",
                                    VOICE_TTS_TASK_STACK_SIZE,
                                    task_arg,
                                    &s_tts_task) != pdPASS) {
        free(task_arg);
        s_tts_task = NULL;
        voice_assistant_end_operation();
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

    esp_err_t err = voice_assistant_try_begin_operation("Full voice test");
    if (err != ESP_OK) {
        return err;
    }

    voice_full_task_arg_t *task_arg = (voice_full_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        voice_assistant_end_operation();
        return ESP_ERR_NO_MEM;
    }
    task_arg->audio_buffer = audio_buffer;
    task_arg->sample_rate_hz = sample_rate_hz;

    if (voice_assistant_create_task(voice_assistant_full_test_task,
                                    "voice_full",
                                    VOICE_FULL_TASK_STACK_SIZE,
                                    task_arg,
                                    &s_full_task) != pdPASS) {
        free(task_arg);
        s_full_task = NULL;
        voice_assistant_end_operation();
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

    esp_err_t err = voice_assistant_try_begin_operation("ASR test");
    if (err != ESP_OK) {
        return err;
    }

    voice_asr_task_arg_t *task_arg = (voice_asr_task_arg_t *)calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        voice_assistant_end_operation();
        return ESP_ERR_NO_MEM;
    }

    task_arg->audio_buffer = audio_buffer;
    task_arg->sample_rate_hz = sample_rate_hz;

    if (voice_assistant_create_task(voice_assistant_asr_test_task,
                                    "asr_test",
                                    VOICE_ASR_TASK_STACK_SIZE,
                                    task_arg,
                                    &s_asr_task) != pdPASS) {
        free(task_arg);
        s_asr_task = NULL;
        voice_assistant_end_operation();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

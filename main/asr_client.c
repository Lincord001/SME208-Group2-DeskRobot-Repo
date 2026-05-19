#include "asr_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mbedtls/base64.h>

#include "api_config.h"

#define ASR_CONNECTED_BIT BIT0
#define ASR_FINISHED_BIT  BIT1
#define ASR_ERROR_BIT     BIT2

#define ASR_TARGET_SAMPLE_RATE_HZ 16000
#define ASR_CHUNK_BYTES           3200
#define ASR_EVENT_TIMEOUT_MS      25000
#define ASR_CANCEL_POLL_MS        100
#define ASR_SEND_TIMEOUT_TICKS    pdMS_TO_TICKS(5000)
#define ASR_WS_BUFFER_SIZE        4096
#define ASR_WS_TASK_STACK         4096

static const char *TAG = "asr_client";

typedef struct {
    EventGroupHandle_t events;
    char *out_text;
    size_t out_text_len;
    char *message_buf;
    size_t message_len;
    size_t message_cap;
    bool (*cancel_cb)(void *ctx);
    void *cancel_ctx;
} asr_session_t;

static bool asr_is_canceled(const asr_session_t *session)
{
    return session != NULL &&
           session->cancel_cb != NULL &&
           session->cancel_cb(session->cancel_ctx);
}

static EventBits_t asr_wait_bits(asr_session_t *session,
                                 EventBits_t bits_to_wait_for,
                                 uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = 0;

    do {
        if (asr_is_canceled(session)) {
            xEventGroupSetBits(session->events, ASR_ERROR_BIT);
            return ASR_ERROR_BIT;
        }

        bits = xEventGroupWaitBits(session->events,
                                   bits_to_wait_for,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(ASR_CANCEL_POLL_MS));
        if ((bits & bits_to_wait_for) != 0) {
            return bits;
        }
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    return bits;
}

static esp_err_t asr_accumulate_message(asr_session_t *session,
                                        const esp_websocket_event_data_t *data,
                                        char **out_message)
{
    *out_message = NULL;

    if (data->payload_len <= 0 || data->data_len <= 0 || data->data_ptr == NULL) {
        return ESP_OK;
    }

    if (data->payload_offset == 0) {
        free(session->message_buf);
        session->message_buf = NULL;
        session->message_len = 0;
        session->message_cap = (size_t)data->payload_len + 1;
        session->message_buf = (char *)heap_caps_calloc(1,
                                                        session->message_cap,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (session->message_buf == NULL) {
            session->message_buf = (char *)calloc(1, session->message_cap);
        }
        if (session->message_buf == NULL) {
            session->message_cap = 0;
            return ESP_ERR_NO_MEM;
        }
    }

    if (session->message_buf == NULL ||
        (size_t)data->payload_offset + (size_t)data->data_len >= session->message_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(session->message_buf + data->payload_offset,
           data->data_ptr,
           (size_t)data->data_len);
    session->message_len += (size_t)data->data_len;

    if (data->fin && session->message_len >= (size_t)data->payload_len) {
        session->message_buf[data->payload_len] = '\0';
        *out_message = session->message_buf;
        session->message_buf = NULL;
        session->message_len = 0;
        session->message_cap = 0;
    }

    return ESP_OK;
}

static const cJSON *asr_find_string_item(const cJSON *root, const char *key)
{
    if (root == NULL || key == NULL) {
        return NULL;
    }

    if (cJSON_IsObject(root)) {
        const cJSON *item = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
            return item;
        }

        const cJSON *child = root->child;
        while (child != NULL) {
            if (strcmp(child->string != NULL ? child->string : "", "type") != 0 &&
                strcmp(child->string != NULL ? child->string : "", "event_id") != 0) {
                item = asr_find_string_item(child, key);
                if (item != NULL) {
                    return item;
                }
            }
            child = child->next;
        }
    } else if (cJSON_IsArray(root)) {
        const cJSON *child = root->child;
        while (child != NULL) {
            const cJSON *item = asr_find_string_item(child, key);
            if (item != NULL) {
                return item;
            }
            child = child->next;
        }
    }

    return NULL;
}

static void asr_append_text(asr_session_t *session, const char *text)
{
    if (session == NULL || session->out_text == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    size_t used = strlen(session->out_text);
    if (used + 1 >= session->out_text_len) {
        return;
    }

    snprintf(session->out_text + used, session->out_text_len - used, "%s", text);
}

static bool asr_store_text_from_event(asr_session_t *session,
                                      const cJSON *root,
                                      bool append)
{
    const cJSON *text = asr_find_string_item(root, "transcript");
    if (text == NULL) {
        text = asr_find_string_item(root, "text");
    }
    if (text == NULL) {
        text = asr_find_string_item(root, "delta");
    }

    if (!cJSON_IsString(text) || text->valuestring == NULL || text->valuestring[0] == '\0') {
        return false;
    }

    if (append) {
        asr_append_text(session, text->valuestring);
    } else {
        snprintf(session->out_text, session->out_text_len, "%s", text->valuestring);
    }

    ESP_LOGI(TAG, "ASR text: %s", text->valuestring);
    return true;
}

static esp_err_t asr_ws_send_json(esp_websocket_client_handle_t client, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(client, json, (int)strlen(json), ASR_SEND_TIMEOUT_TICKS);
    free(json);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

static esp_err_t asr_send_session_update(esp_websocket_client_handle_t client)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    cJSON *transcription = cJSON_CreateObject();
    cJSON *turn_detection = cJSON_CreateObject();
    if (root == NULL || session == NULL || modalities == NULL ||
        transcription == NULL || turn_detection == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(modalities);
        cJSON_Delete(transcription);
        cJSON_Delete(turn_detection);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "event_id", "esp32_session_update");
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(session, "input_audio_format", "pcm");
    cJSON_AddNumberToObject(session, "sample_rate", ASR_TARGET_SAMPLE_RATE_HZ);
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToObject(session, "modalities", modalities);
    cJSON_AddStringToObject(transcription, "language", "zh");
    cJSON_AddItemToObject(session, "input_audio_transcription", transcription);
    cJSON_AddStringToObject(turn_detection, "type", "server_vad");
    cJSON_AddNumberToObject(turn_detection, "threshold", 0.0);
    cJSON_AddNumberToObject(turn_detection, "silence_duration_ms", 400);
    cJSON_AddItemToObject(session, "turn_detection", turn_detection);
    cJSON_AddItemToObject(root, "session", session);

    esp_err_t err = asr_ws_send_json(client, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t asr_send_finish(esp_websocket_client_handle_t client)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "event_id", "esp32_session_finish");
    cJSON_AddStringToObject(root, "type", "session.finish");
    esp_err_t err = asr_ws_send_json(client, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t asr_send_audio_chunk(esp_websocket_client_handle_t client,
                                      const uint8_t *pcm,
                                      size_t pcm_len)
{
    size_t b64_len = 0;
    if (mbedtls_base64_encode(NULL, 0, &b64_len, pcm, pcm_len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    unsigned char *b64 = (unsigned char *)heap_caps_malloc(b64_len + 1,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 == NULL) {
        b64 = (unsigned char *)malloc(b64_len + 1);
    }
    if (b64 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int ret = mbedtls_base64_encode(b64, b64_len, &b64_len, pcm, pcm_len);
    if (ret != 0) {
        free(b64);
        return ESP_FAIL;
    }
    b64[b64_len] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(b64);
        return ESP_ERR_NO_MEM;
    }

    cJSON *audio = cJSON_CreateStringReference((const char *)b64);
    if (audio == NULL) {
        cJSON_Delete(root);
        free(b64);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "event_id", "esp32_audio_append");
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    cJSON_AddItemToObject(root, "audio", audio);
    esp_err_t err = asr_ws_send_json(client, root);
    cJSON_Delete(root);
    free(b64);
    return err;
}

static void asr_ws_event_handler(void *handler_args,
                                 esp_event_base_t base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)base;

    asr_session_t *session = (asr_session_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ASR WebSocket connected");
        xEventGroupSetBits(session->events, ASR_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (data->data_ptr == NULL || data->data_len <= 0 || data->op_code != 0x1) {
            break;
        }
        if (asr_is_canceled(session)) {
            xEventGroupSetBits(session->events, ASR_ERROR_BIT);
            break;
        }

        char *message = NULL;
        esp_err_t err = asr_accumulate_message(session, data, &message);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to accumulate ASR message: %s", esp_err_to_name(err));
            xEventGroupSetBits(session->events, ASR_ERROR_BIT);
            break;
        }

        if (message == NULL) {
            break;
        }

        cJSON *root = cJSON_Parse(message);
        if (root != NULL) {
            const cJSON *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) && type->valuestring != NULL) {
                ESP_LOGI(TAG, "ASR event: %s", type->valuestring);
                if (strcmp(type->valuestring, "conversation.item.input_audio_transcription.text") == 0) {
                    asr_store_text_from_event(session, root, true);
                } else if (strcmp(type->valuestring, "conversation.item.input_audio_transcription.completed") == 0) {
                    asr_store_text_from_event(session, root, false);
                }

                if (strcmp(type->valuestring, "session.finished") == 0) {
                    if (session->out_text[0] == '\0') {
                        asr_store_text_from_event(session, root, false);
                    }
                    xEventGroupSetBits(session->events, ASR_FINISHED_BIT);
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Failed to parse ASR message: %s", message);
        }

        free(message);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ASR WebSocket error");
        xEventGroupSetBits(session->events, ASR_ERROR_BIT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "ASR WebSocket closed");
        break;

    default:
        break;
    }
}

static size_t asr_resample_linear_to_16k(const int16_t *src,
                                         size_t src_samples,
                                         uint32_t src_rate_hz,
                                         int16_t *dst,
                                         size_t dst_capacity)
{
    if (src == NULL || dst == NULL || src_rate_hz == 0 || src_samples == 0) {
        return 0;
    }

    size_t out_samples = ((uint64_t)src_samples * ASR_TARGET_SAMPLE_RATE_HZ) /
                         src_rate_hz;
    if (out_samples > dst_capacity) {
        out_samples = dst_capacity;
    }

    for (size_t out = 0; out < out_samples; ++out) {
        uint64_t src_pos_num = (uint64_t)out * src_rate_hz;
        size_t src_idx = (size_t)(src_pos_num / ASR_TARGET_SAMPLE_RATE_HZ);
        uint32_t frac = (uint32_t)(src_pos_num % ASR_TARGET_SAMPLE_RATE_HZ);

        if (src_idx + 1 >= src_samples) {
            dst[out] = src[src_samples - 1];
            continue;
        }

        int32_t a = src[src_idx];
        int32_t b = src[src_idx + 1];
        dst[out] = (int16_t)(a + (((b - a) * (int32_t)frac) /
                                  ASR_TARGET_SAMPLE_RATE_HZ));
    }

    return out_samples;
}

static esp_err_t asr_send_audio_from_pcm(esp_websocket_client_handle_t client,
                                         const int16_t *pcm,
                                         size_t sample_count,
                                         uint32_t sample_rate_hz,
                                         asr_session_t *session)
{
    const size_t input_chunk_samples = 2400; // 100 ms at 24 kHz
    int16_t *chunk16 = (int16_t *)heap_caps_malloc(ASR_CHUNK_BYTES,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk16 == NULL) {
        chunk16 = (int16_t *)malloc(ASR_CHUNK_BYTES);
    }
    if (chunk16 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (size_t pos = 0; pos < sample_count; pos += input_chunk_samples) {
        if (asr_is_canceled(session)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        size_t remaining = sample_count - pos;
        size_t take = remaining > input_chunk_samples ? input_chunk_samples : remaining;
        size_t out_samples = 0;

        if (sample_rate_hz == ASR_TARGET_SAMPLE_RATE_HZ) {
            out_samples = take;
            if (out_samples * sizeof(int16_t) > ASR_CHUNK_BYTES) {
                out_samples = ASR_CHUNK_BYTES / sizeof(int16_t);
            }
            memcpy(chunk16, &pcm[pos], out_samples * sizeof(int16_t));
        } else if (sample_rate_hz == 24000) {
            out_samples = asr_resample_linear_to_16k(&pcm[pos], take, sample_rate_hz,
                                                     chunk16,
                                                     ASR_CHUNK_BYTES / sizeof(int16_t));
        } else {
            ESP_LOGE(TAG, "Unsupported ASR input sample rate: %lu", (unsigned long)sample_rate_hz);
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (out_samples == 0) {
            continue;
        }

        err = asr_send_audio_chunk(client,
                                   (const uint8_t *)chunk16,
                                   out_samples * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send ASR audio chunk: %s", esp_err_to_name(err));
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    free(chunk16);
    return err;
}

esp_err_t asr_client_init(void)
{
    if (!api_config_has_dashscope_api_key()) {
        ESP_LOGW(TAG, "DashScope API key is not configured");
    }
    return ESP_OK;
}

esp_err_t asr_client_recognize_pcm16(const int16_t *pcm,
                                     size_t sample_count,
                                     uint32_t sample_rate_hz,
                                     char *out_text,
                                     size_t out_text_len)
{
    return asr_client_recognize_pcm16_with_cancel(pcm,
                                                  sample_count,
                                                  sample_rate_hz,
                                                  out_text,
                                                  out_text_len,
                                                  NULL,
                                                  NULL);
}

esp_err_t asr_client_recognize_pcm16_with_cancel(const int16_t *pcm,
                                                 size_t sample_count,
                                                 uint32_t sample_rate_hz,
                                                 char *out_text,
                                                 size_t out_text_len,
                                                 bool (*cancel_cb)(void *ctx),
                                                 void *cancel_ctx)
{
    if (pcm == NULL || sample_count == 0 || out_text == NULL || out_text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_text[0] = '\0';

    if (!api_config_has_dashscope_api_key()) {
        ESP_LOGE(TAG, "Missing API key. Create main/api_config_private.h from the example file.");
        return ESP_ERR_INVALID_STATE;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    asr_session_t session = {
        .events = events,
        .out_text = out_text,
        .out_text_len = out_text_len,
        .cancel_cb = cancel_cb,
        .cancel_ctx = cancel_ctx,
    };

    char headers[192];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "OpenAI-Beta: realtime=v1\r\n",
             api_config_get_dashscope_api_key());

    esp_websocket_client_config_t config = {
        .uri = api_config_get_asr_url(),
        .headers = headers,
        .disable_auto_reconnect = true,
        .task_stack = ASR_WS_TASK_STACK,
        .buffer_size = ASR_WS_BUFFER_SIZE,
        .network_timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ESP_LOGI(TAG,
             "ASR heap before websocket init: internal=%u psram=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG,
                 "ASR websocket init failed: internal=%u psram=%u largest_internal=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vEventGroupDelete(events);
        return ESP_FAIL;
    }

    esp_err_t err = esp_websocket_register_events(client,
                                                  WEBSOCKET_EVENT_ANY,
                                                  asr_ws_event_handler,
                                                  &session);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        vEventGroupDelete(events);
        return err;
    }

    ESP_LOGI(TAG, "Connecting ASR WebSocket: %s", api_config_get_asr_url());
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to start ASR WebSocket: %s internal=%u psram=%u largest_internal=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_destroy(client);
        vEventGroupDelete(events);
        return err;
    }

    EventBits_t bits = asr_wait_bits(&session,
                                     ASR_CONNECTED_BIT | ASR_ERROR_BIT,
                                     ASR_EVENT_TIMEOUT_MS);
    if ((bits & ASR_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "ASR WebSocket connect timeout/error");
        err = asr_is_canceled(&session) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    err = asr_send_session_update(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    err = asr_send_audio_from_pcm(client, pcm, sample_count, sample_rate_hz, &session);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = asr_send_finish(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = asr_wait_bits(&session,
                         ASR_FINISHED_BIT | ASR_ERROR_BIT,
                         ASR_EVENT_TIMEOUT_MS);
    if ((bits & ASR_FINISHED_BIT) == 0) {
        ESP_LOGE(TAG, "ASR session finish timeout/error");
        err = asr_is_canceled(&session) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    if (out_text[0] == '\0') {
        ESP_LOGW(TAG, "ASR finished without transcript");
        err = ESP_ERR_INVALID_RESPONSE;
    }

cleanup:
    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    free(session.message_buf);
    vEventGroupDelete(events);
    return err;
}

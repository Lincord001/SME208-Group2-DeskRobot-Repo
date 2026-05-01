#include "asr_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
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
#define ASR_EVENT_TIMEOUT_MS      30000
#define ASR_SEND_TIMEOUT_TICKS    pdMS_TO_TICKS(5000)
#define ASR_WS_BUFFER_SIZE        4096

static const char *TAG = "asr_client";

typedef struct {
    EventGroupHandle_t events;
    char *out_text;
    size_t out_text_len;
} asr_session_t;

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

    unsigned char *b64 = (unsigned char *)malloc(b64_len + 1);
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

    cJSON_AddStringToObject(root, "event_id", "esp32_audio_append");
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", (const char *)b64);
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

        char *message = (char *)calloc(1, (size_t)data->data_len + 1);
        if (message == NULL) {
            xEventGroupSetBits(session->events, ASR_ERROR_BIT);
            break;
        }
        memcpy(message, data->data_ptr, (size_t)data->data_len);

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

static size_t asr_downsample_24k_to_16k(const int16_t *src,
                                        size_t src_samples,
                                        int16_t *dst,
                                        size_t dst_capacity)
{
    size_t out = 0;
    for (size_t i = 0; i + 2 < src_samples && out + 1 < dst_capacity; i += 3) {
        dst[out++] = src[i];
        dst[out++] = src[i + 2];
    }
    return out;
}

static esp_err_t asr_send_audio_from_pcm(esp_websocket_client_handle_t client,
                                         const int16_t *pcm,
                                         size_t sample_count,
                                         uint32_t sample_rate_hz)
{
    const size_t input_chunk_samples = 2400; // 100 ms at 24 kHz
    int16_t *chunk16 = (int16_t *)malloc(ASR_CHUNK_BYTES);
    if (chunk16 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (size_t pos = 0; pos < sample_count; pos += input_chunk_samples) {
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
            out_samples = asr_downsample_24k_to_16k(&pcm[pos], take,
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
        .task_stack = 6144,
        .buffer_size = ASR_WS_BUFFER_SIZE,
        .network_timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (client == NULL) {
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
        ESP_LOGE(TAG, "Failed to start ASR WebSocket: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        vEventGroupDelete(events);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(events,
                                           ASR_CONNECTED_BIT | ASR_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(ASR_EVENT_TIMEOUT_MS));
    if ((bits & ASR_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "ASR WebSocket connect timeout/error");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    err = asr_send_session_update(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    err = asr_send_audio_from_pcm(client, pcm, sample_count, sample_rate_hz);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = asr_send_finish(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(events,
                               ASR_FINISHED_BIT | ASR_ERROR_BIT,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(ASR_EVENT_TIMEOUT_MS));
    if ((bits & ASR_FINISHED_BIT) == 0) {
        ESP_LOGE(TAG, "ASR session finish timeout/error");
        err = ESP_ERR_TIMEOUT;
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
    vEventGroupDelete(events);
    return err;
}

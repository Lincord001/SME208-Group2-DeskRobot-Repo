#include "tts_client.h"

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

#define TTS_CONNECTED_BIT BIT0
#define TTS_FINISHED_BIT  BIT1
#define TTS_ERROR_BIT     BIT2

#define TTS_EVENT_TIMEOUT_MS   45000
#define TTS_SEND_TIMEOUT_TICKS pdMS_TO_TICKS(5000)
#define TTS_WS_BUFFER_SIZE     4096
#define TTS_WS_TASK_STACK      4096

static const char *TAG = "tts_client";

typedef struct {
    EventGroupHandle_t events;
    size_t audio_bytes;
    size_t audio_chunks;
    char *message_buf;
    size_t message_len;
    size_t message_cap;
    audio_buffer_t *audio_buffer;
    bool buffer_overflow;
} tts_session_t;

static esp_err_t tts_accumulate_message(tts_session_t *session,
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
        session->message_buf = (char *)heap_caps_calloc(1, session->message_cap,
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

static esp_err_t tts_ws_send_json(esp_websocket_client_handle_t client, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(client, json, (int)strlen(json), TTS_SEND_TIMEOUT_TICKS);
    free(json);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

static esp_err_t tts_send_session_update(esp_websocket_client_handle_t client)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    if (root == NULL || session == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(root, "event_id", "esp32_tts_session_update");
    cJSON_AddStringToObject(session, "mode", "server_commit");
    cJSON_AddStringToObject(session, "voice", api_config_get_tts_voice());
    cJSON_AddStringToObject(session, "language_type", "Auto");
    cJSON_AddStringToObject(session, "response_format", "pcm");
    cJSON_AddNumberToObject(session, "sample_rate", 24000);
    cJSON_AddItemToObject(root, "session", session);

    esp_err_t err = tts_ws_send_json(client, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t tts_send_text_append(esp_websocket_client_handle_t client, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_text_buffer.append");
    cJSON_AddStringToObject(root, "event_id", "esp32_tts_text_append");
    cJSON_AddStringToObject(root, "text", text);
    esp_err_t err = tts_ws_send_json(client, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t tts_send_finish(esp_websocket_client_handle_t client)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "session.finish");
    cJSON_AddStringToObject(root, "event_id", "esp32_tts_session_finish");
    esp_err_t err = tts_ws_send_json(client, root);
    cJSON_Delete(root);
    return err;
}

static void tts_write_audio_to_buffer(tts_session_t *session,
                                      const unsigned char *data,
                                      size_t len)
{
    if (session->audio_buffer == NULL || data == NULL || len == 0) {
        return;
    }

    audio_buffer_t *audio_buffer = session->audio_buffer;
    audio_buffer_state_t audio_state;
    audio_buffer_get_state(audio_buffer, &audio_state);
    size_t write_byte_pos = audio_state.current_write_pos * sizeof(int16_t);
    if (write_byte_pos >= audio_state.total_size) {
        len = 0;
        session->buffer_overflow = true;
    } else if (write_byte_pos + len > audio_state.total_size) {
        len = audio_state.total_size - write_byte_pos;
        session->buffer_overflow = true;
    }

    if (len > 0) {
        memcpy((uint8_t *)audio_buffer->buffer + write_byte_pos, data, len);
        audio_buffer_lock(audio_buffer);
        audio_buffer->current_write_pos += len / sizeof(int16_t);
        audio_buffer->recorded_size = audio_buffer->current_write_pos * sizeof(int16_t);
        audio_buffer_unlock(audio_buffer);
    }
}

static esp_err_t tts_count_audio_delta_b64(tts_session_t *session,
                                           const char *b64_text,
                                           size_t b64_len)
{
    if (b64_text == NULL || b64_len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const unsigned char *b64 = (const unsigned char *)b64_text;
    size_t raw_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &raw_len, b64, b64_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    unsigned char *raw = (unsigned char *)heap_caps_malloc(raw_len,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        raw = (unsigned char *)malloc(raw_len);
    }
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(raw, raw_len, &raw_len, b64, b64_len);
    if (ret != 0) {
        free(raw);
        return ESP_FAIL;
    }

    tts_write_audio_to_buffer(session, raw, raw_len);
    session->audio_bytes += raw_len;
    session->audio_chunks++;
    free(raw);

    if ((session->audio_chunks % 10) == 0) {
        ESP_LOGI(TAG, "TTS audio received: chunks=%u bytes=%u",
                 (unsigned)session->audio_chunks,
                 (unsigned)session->audio_bytes);
    }

    return ESP_OK;
}

static esp_err_t tts_count_audio_delta_json(tts_session_t *session, const cJSON *root)
{
    const cJSON *delta = cJSON_GetObjectItem(root, "delta");
    if (!cJSON_IsString(delta) || delta->valuestring == NULL || delta->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return tts_count_audio_delta_b64(session,
                                     delta->valuestring,
                                     strlen(delta->valuestring));
}

static bool tts_extract_audio_delta(const char *message,
                                    const char **out_b64,
                                    size_t *out_b64_len)
{
    const char *type = strstr(message, "\"type\":\"response.audio.delta\"");
    if (type == NULL) {
        return false;
    }

    const char *delta = strstr(message, "\"delta\":\"");
    if (delta == NULL) {
        return false;
    }

    delta += strlen("\"delta\":\"");
    const char *end = strchr(delta, '"');
    if (end == NULL || end <= delta) {
        return false;
    }

    *out_b64 = delta;
    *out_b64_len = (size_t)(end - delta);
    return true;
}

static void tts_ws_event_handler(void *handler_args,
                                 esp_event_base_t base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)base;

    tts_session_t *session = (tts_session_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "TTS WebSocket connected");
        xEventGroupSetBits(session->events, TTS_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (data->data_ptr == NULL || data->data_len <= 0 || data->op_code != 0x1) {
            break;
        }

        char *message = NULL;
        esp_err_t err = tts_accumulate_message(session, data, &message);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to accumulate TTS message: %s", esp_err_to_name(err));
            xEventGroupSetBits(session->events, TTS_ERROR_BIT);
            break;
        }

        if (message == NULL) {
            break;
        }

        const char *audio_b64 = NULL;
        size_t audio_b64_len = 0;
        if (tts_extract_audio_delta(message, &audio_b64, &audio_b64_len)) {
            esp_err_t err = tts_count_audio_delta_b64(session, audio_b64, audio_b64_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to decode TTS audio delta: %s", esp_err_to_name(err));
            }
            free(message);
            break;
        }

        cJSON *root = cJSON_Parse(message);
        if (root != NULL) {
            const cJSON *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) && type->valuestring != NULL) {
                if (strcmp(type->valuestring, "response.audio.delta") == 0) {
                    esp_err_t err = tts_count_audio_delta_json(session, root);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to decode TTS audio delta: %s", esp_err_to_name(err));
                    }
                } else {
                    ESP_LOGI(TAG, "TTS event: %s", type->valuestring);
                    if (strcmp(type->valuestring, "error") == 0) {
                        ESP_LOGE(TAG, "TTS error event: %s", message);
                        xEventGroupSetBits(session->events, TTS_ERROR_BIT);
                    } else if (strcmp(type->valuestring, "session.finished") == 0) {
                        xEventGroupSetBits(session->events, TTS_FINISHED_BIT);
                    }
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Failed to parse TTS message header, len=%u", (unsigned)strlen(message));
        }

        free(message);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "TTS WebSocket error");
        xEventGroupSetBits(session->events, TTS_ERROR_BIT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "TTS WebSocket closed");
        break;

    default:
        break;
    }
}

esp_err_t tts_client_init(void)
{
    if (!api_config_has_dashscope_api_key()) {
        ESP_LOGW(TAG, "DashScope API key is not configured");
    }
    return ESP_OK;
}

esp_err_t tts_client_synthesize_count_bytes(const char *text, size_t *out_audio_bytes)
{
    return tts_client_synthesize_to_audio_buffer(text, NULL, out_audio_bytes);
}

esp_err_t tts_client_synthesize_to_audio_buffer(const char *text,
                                                audio_buffer_t *audio_buffer,
                                                size_t *out_audio_bytes)
{
    if (text == NULL || text[0] == '\0' || out_audio_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_audio_bytes = 0;

    if (audio_buffer != NULL) {
        audio_buffer_state_t audio_state;
        audio_buffer_get_state(audio_buffer, &audio_state);
        if (audio_buffer->buffer == NULL || audio_state.total_size == 0 ||
            audio_state.recording || audio_state.playing) {
            return ESP_ERR_INVALID_STATE;
        }

        audio_buffer_lock(audio_buffer);
        audio_buffer->current_write_pos = 0;
        audio_buffer->current_read_pos = 0;
        audio_buffer->recorded_size = 0;
        audio_buffer->recording = false;
        audio_buffer->playing = false;
        audio_buffer->recording_complete = false;
        audio_buffer_unlock(audio_buffer);
    }

    if (!api_config_has_dashscope_api_key()) {
        ESP_LOGE(TAG, "Missing API key. Create main/api_config_private.h from the example file.");
        return ESP_ERR_INVALID_STATE;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tts_session_t session = {
        .events = events,
        .audio_buffer = audio_buffer,
    };

    char headers[160];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n",
             api_config_get_dashscope_api_key());

    esp_websocket_client_config_t config = {
        .uri = api_config_get_tts_url(),
        .headers = headers,
        .disable_auto_reconnect = true,
        .task_stack = TTS_WS_TASK_STACK,
        .buffer_size = TTS_WS_BUFFER_SIZE,
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
                                                  tts_ws_event_handler,
                                                  &session);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        vEventGroupDelete(events);
        return err;
    }

    ESP_LOGI(TAG, "Connecting TTS WebSocket: %s", api_config_get_tts_url());
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TTS WebSocket: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        vEventGroupDelete(events);
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(events,
                                           TTS_CONNECTED_BIT | TTS_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(TTS_EVENT_TIMEOUT_MS));
    if ((bits & TTS_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "TTS WebSocket connect timeout/error");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    err = tts_send_session_update(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    err = tts_send_text_append(client, text);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = tts_send_finish(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(events,
                               TTS_FINISHED_BIT | TTS_ERROR_BIT,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(TTS_EVENT_TIMEOUT_MS));
    if ((bits & TTS_FINISHED_BIT) == 0) {
        ESP_LOGE(TAG, "TTS session finish timeout/error");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    *out_audio_bytes = session.audio_bytes;
    if (session.audio_bytes == 0) {
        ESP_LOGW(TAG, "TTS finished without audio");
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (session.buffer_overflow) {
        audio_buffer_state_t audio_state = {0};
        if (audio_buffer != NULL) {
            audio_buffer_get_state(audio_buffer, &audio_state);
        }
        ESP_LOGW(TAG, "TTS audio buffer overflow, stored %u of received %u bytes",
                 (unsigned)(audio_buffer != NULL ? audio_state.recorded_size : 0),
                 (unsigned)session.audio_bytes);
        err = ESP_ERR_NO_MEM;
    } else {
        ESP_LOGI(TAG, "TTS audio total: chunks=%u bytes=%u",
                 (unsigned)session.audio_chunks,
                 (unsigned)session.audio_bytes);
    }

    if (audio_buffer != NULL && session.audio_bytes > 0) {
        audio_buffer_lock(audio_buffer);
        audio_buffer->recording_complete = true;
        audio_buffer->current_read_pos = 0;
        size_t recorded_size = audio_buffer->recorded_size;
        audio_buffer_unlock(audio_buffer);
        ESP_LOGI(TAG, "TTS PCM stored to audio buffer: %u bytes",
                 (unsigned)recorded_size);
    }

cleanup:
    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    free(session.message_buf);
    vEventGroupDelete(events);
    return err;
}

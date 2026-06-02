#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "api_config.h"

#define LLM_RESPONSE_BUFFER_BYTES (16 * 1024)
#define LLM_MAX_COMPLETION_TOKENS 100
#define LLM_REPLY_MAX_UTF8_CHARS  70

static const char *LLM_SYSTEM_PROMPT =
    "你是桌面机器人语音助手。请用中文直接回答，控制在70个汉字以内，"
    "不要使用Markdown、列表或长段解释。";

static const char *TAG = "llm_client";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool (*cancel_cb)(void *ctx);
    void *cancel_ctx;
} llm_http_response_t;

static esp_err_t llm_http_event_handler(esp_http_client_event_t *evt)
{
    llm_http_response_t *response = (llm_http_response_t *)evt->user_data;
    if (response != NULL &&
        response->cancel_cb != NULL &&
        response->cancel_cb(response->cancel_ctx)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    if (response == NULL || response->data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t copy_len = (size_t)evt->data_len;
    if (response->len + copy_len >= response->cap) {
        copy_len = response->cap - response->len - 1;
    }

    if (copy_len > 0) {
        memcpy(response->data + response->len, evt->data, copy_len);
        response->len += copy_len;
        response->data[response->len] = '\0';
    }

    return ESP_OK;
}

static char *llm_build_request_body(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system_message = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON *thinking = cJSON_CreateObject();
    if (root == NULL || messages == NULL || system_message == NULL ||
        message == NULL || thinking == NULL) {
        goto fail;
    }

    cJSON_AddStringToObject(root, "model", api_config_get_llm_model());
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", LLM_MAX_COMPLETION_TOKENS);
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON_AddItemToObject(root, "thinking", thinking);
    thinking = NULL;

    cJSON_AddStringToObject(system_message, "role", "system");
    cJSON_AddStringToObject(system_message, "content", LLM_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, system_message);
    system_message = NULL;

    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", user_text);
    cJSON_AddItemToArray(messages, message);
    message = NULL;
    cJSON_AddItemToObject(root, "messages", messages);
    messages = NULL;

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;

fail:
    cJSON_Delete(root);
    cJSON_Delete(messages);
    cJSON_Delete(system_message);
    cJSON_Delete(message);
    cJSON_Delete(thinking);
    return NULL;
}

static size_t llm_limited_utf8_bytes(const char *text, size_t max_chars, size_t max_bytes)
{
    size_t bytes = 0;
    size_t chars = 0;

    while (text[bytes] != '\0' && chars < max_chars && bytes < max_bytes) {
        unsigned char c = (unsigned char)text[bytes];
        size_t char_bytes = 1;

        if ((c & 0x80) == 0) {
            char_bytes = 1;
        } else if ((c & 0xE0) == 0xC0) {
            char_bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            char_bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            char_bytes = 4;
        }

        if (bytes + char_bytes > max_bytes) {
            break;
        }

        for (size_t i = 1; i < char_bytes; ++i) {
            if ((text[bytes + i] & 0xC0) != 0x80) {
                char_bytes = 1;
                break;
            }
        }

        bytes += char_bytes;
        chars++;
    }

    return bytes;
}

static void llm_copy_limited_reply(char *out_reply,
                                   size_t out_reply_len,
                                   const char *content)
{
    if (out_reply_len == 0) {
        return;
    }

    size_t max_bytes = out_reply_len - 1;
    size_t copy_len = llm_limited_utf8_bytes(content,
                                             LLM_REPLY_MAX_UTF8_CHARS,
                                             max_bytes);
    memcpy(out_reply, content, copy_len);
    out_reply[copy_len] = '\0';
}

static esp_err_t llm_parse_reply(const char *response_json, char *out_reply, size_t out_reply_len)
{
    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse LLM JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = choice != NULL ? cJSON_GetObjectItem(choice, "message") : NULL;
    const cJSON *content = message != NULL ? cJSON_GetObjectItem(message, "content") : NULL;

    if (!cJSON_IsString(content) || content->valuestring == NULL || content->valuestring[0] == '\0') {
        const cJSON *error = cJSON_GetObjectItem(root, "error");
        const cJSON *error_message = error != NULL ? cJSON_GetObjectItem(error, "message") : NULL;
        if (cJSON_IsString(error_message) && error_message->valuestring != NULL) {
            ESP_LOGE(TAG, "LLM API error: %s", error_message->valuestring);
        } else {
            ESP_LOGE(TAG, "LLM response has no choices[0].message.content");
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    llm_copy_limited_reply(out_reply, out_reply_len, content->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t llm_client_init(void)
{
    if (!api_config_has_llm_api_key()) {
        ESP_LOGW(TAG, "LLM API key is not configured");
    }
    return ESP_OK;
}

esp_err_t llm_client_chat(const char *user_text, char *out_reply, size_t out_reply_len)
{
    return llm_client_chat_with_cancel(user_text, out_reply, out_reply_len, NULL, NULL);
}

esp_err_t llm_client_chat_with_cancel(const char *user_text,
                                      char *out_reply,
                                      size_t out_reply_len,
                                      bool (*cancel_cb)(void *ctx),
                                      void *cancel_ctx)
{
    if (user_text == NULL || out_reply == NULL || out_reply_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_reply[0] = '\0';

    if (!api_config_has_llm_api_key()) {
        ESP_LOGE(TAG, "Missing API key. Create main/api_config_private.h from the example file.");
        return ESP_ERR_INVALID_STATE;
    }
    if (cancel_cb != NULL && cancel_cb(cancel_ctx)) {
        return ESP_ERR_INVALID_STATE;
    }

    char *request_body = llm_build_request_body(user_text);
    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to build LLM request body");
        return ESP_ERR_NO_MEM;
    }

    llm_http_response_t response = {
        .data = (char *)calloc(1, LLM_RESPONSE_BUFFER_BYTES),
        .len = 0,
        .cap = LLM_RESPONSE_BUFFER_BYTES,
        .cancel_cb = cancel_cb,
        .cancel_ctx = cancel_ctx,
    };
    if (response.data == NULL) {
        free(request_body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = api_config_get_llm_url(),
        .event_handler = llm_http_event_handler,
        .user_data = &response,
        .timeout_ms = api_config_get_http_timeout_ms(),
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request_body);
        free(response.data);
        return ESP_FAIL;
    }

    char auth_header[192];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_config_get_llm_api_key());

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    ESP_LOGI(TAG, "POST %s model=%s", api_config_get_llm_url(), api_config_get_llm_model());
    esp_err_t err = esp_http_client_perform(client);
    if (cancel_cb != NULL && cancel_cb(cancel_ctx)) {
        err = ESP_ERR_INVALID_STATE;
    }
    int status_code = esp_http_client_get_status_code(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP status=%d response_len=%u", status_code, (unsigned)response.len);
        if (status_code >= 200 && status_code < 300) {
            err = llm_parse_reply(response.data, out_reply, out_reply_len);
        } else {
            ESP_LOGE(TAG, "LLM HTTP request failed with status %d: %s", status_code, response.data);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "LLM HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(request_body);
    free(response.data);
    return err;
}

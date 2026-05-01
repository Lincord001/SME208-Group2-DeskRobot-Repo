#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t llm_client_init(void);
esp_err_t llm_client_chat(const char *user_text, char *out_reply, size_t out_reply_len);

#ifdef __cplusplus
}
#endif

#endif // LLM_CLIENT_H

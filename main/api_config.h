#ifndef API_CONFIG_H
#define API_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *api_config_get_dashscope_api_key(void);
const char *api_config_get_llm_api_key(void);
const char *api_config_get_llm_url(void);
const char *api_config_get_llm_model(void);
const char *api_config_get_asr_url(void);
const char *api_config_get_tts_url(void);
const char *api_config_get_tts_voice(void);
int api_config_get_http_timeout_ms(void);
bool api_config_has_dashscope_api_key(void);
bool api_config_has_llm_api_key(void);

#ifdef __cplusplus
}
#endif

#endif // API_CONFIG_H

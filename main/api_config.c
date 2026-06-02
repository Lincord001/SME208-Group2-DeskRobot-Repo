#include "api_config.h"

#include <stdbool.h>
#include <string.h>

#if __has_include("api_config_private.h")
#include "api_config_private.h"
#endif

#ifndef DASHSCOPE_API_KEY
#define DASHSCOPE_API_KEY ""
#endif

#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY ""
#endif

#ifndef DEEPSEEK_LLM_URL
#define DEEPSEEK_LLM_URL "https://api.deepseek.com/chat/completions"
#endif

#ifndef DEEPSEEK_LLM_MODEL
#define DEEPSEEK_LLM_MODEL "deepseek-v4-pro"
#endif

#ifndef DASHSCOPE_ASR_URL
#define DASHSCOPE_ASR_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"
#endif

#ifndef DASHSCOPE_TTS_URL
#define DASHSCOPE_TTS_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime"
#endif

#ifndef DASHSCOPE_TTS_VOICE
#define DASHSCOPE_TTS_VOICE "Cherry"
#endif

#ifndef DASHSCOPE_HTTP_TIMEOUT_MS
#define DASHSCOPE_HTTP_TIMEOUT_MS 45000
#endif

const char *api_config_get_dashscope_api_key(void)
{
    return DASHSCOPE_API_KEY;
}

const char *api_config_get_llm_api_key(void)
{
    return DEEPSEEK_API_KEY;
}

const char *api_config_get_llm_url(void)
{
    return DEEPSEEK_LLM_URL;
}

const char *api_config_get_llm_model(void)
{
    return DEEPSEEK_LLM_MODEL;
}

const char *api_config_get_asr_url(void)
{
    return DASHSCOPE_ASR_URL;
}

const char *api_config_get_tts_url(void)
{
    return DASHSCOPE_TTS_URL;
}

const char *api_config_get_tts_voice(void)
{
    return DASHSCOPE_TTS_VOICE;
}

int api_config_get_http_timeout_ms(void)
{
    return DASHSCOPE_HTTP_TIMEOUT_MS;
}

bool api_config_has_dashscope_api_key(void)
{
    const char *key = api_config_get_dashscope_api_key();
    return key != NULL && key[0] != '\0' && strcmp(key, "PLEASE_SET_YOUR_API_KEY") != 0;
}

bool api_config_has_llm_api_key(void)
{
    const char *key = api_config_get_llm_api_key();
    return key != NULL && key[0] != '\0' && strcmp(key, "PLEASE_SET_YOUR_API_KEY") != 0;
}

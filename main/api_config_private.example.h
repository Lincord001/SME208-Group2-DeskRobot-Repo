#ifndef API_CONFIG_PRIVATE_H
#define API_CONFIG_PRIVATE_H

// Copy this file to main/api_config_private.h for local testing.
// Do not commit the real private header or a real API key.
#define DASHSCOPE_API_KEY "PLEASE_SET_YOUR_API_KEY"
#define DEEPSEEK_API_KEY "PLEASE_SET_YOUR_API_KEY"
// To route LLM requests through the local PC search proxy, override the URL:
// #define DEEPSEEK_LLM_URL "http://YOUR_PC_LAN_IP:8080/chat/completions"

#endif // API_CONFIG_PRIVATE_H

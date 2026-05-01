# AI Service API Research

This note is for SME208 Lab6 project preparation. It does not contain real API
keys and does not call any paid cloud service.

## ASR / LLM / TTS

- ASR (Automatic Speech Recognition): converts recorded speech audio into text.
- LLM (Large Language Model): converts the user's text question into a text
  answer or command.
- TTS (Text To Speech): converts text back into playable speech audio.

## What ESP32 Needs To Call Web Services

- WiFi networking, including automatic reconnect after disconnection.
- HTTP or HTTPS client support.
- API key, token, or another authentication mechanism.
- JSON request generation and response parsing.
- Audio upload for ASR, and audio download or streaming for TTS.

## Likely ESP-IDF Components For Future Integration

- `esp_http_client` for HTTP/HTTPS requests.
- `cJSON` for JSON request and response data.
- `mbedTLS` and HTTPS certificate configuration for TLS verification.
- Audio format handling, such as PCM, WAV, OPUS, or MP3 depending on the API.
- Retry and timeout handling for network errors.

## Relationship To Lab6 WiFi

The WiFi networking module added in Lab6 is the foundation for future ASR, LLM,
and TTS cloud calls. Without stable WiFi connection, configuration persistence,
and reconnect behavior, the device cannot reliably upload microphone audio,
send text prompts, or download synthesized speech.

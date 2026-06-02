#include "ble_serial.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <store/config/ble_store_config.h>

#include "audio_spk.h"
#include "display.h"
#include "power_manager.h"
#include "servo.h"
#include "system_status.h"
#include "voice_assistant.h"
#include "wifi_network.h"

#define BLE_SERIAL_DEVICE_NAME "DeskRobot"
#define BLE_SERIAL_QUEUE_LEN 8
#define BLE_SERIAL_LINE_MAX 128
#define BLE_SERIAL_TASK_STACK 4096
#define BLE_SERIAL_TASK_PRIO 4
#define BLE_SERIAL_NOTIFY_CHUNK 20
#define BLE_SERIAL_SERVO_STEP_DEG 10
#define BLE_SERIAL_RESTART_DELAY_MS 200

typedef struct {
    char line[BLE_SERIAL_LINE_MAX];
} ble_serial_command_t;

static const char *TAG = "ble_serial";

void ble_store_config_init(void);

static audio_buffer_t *s_audio_buffer;
static uint32_t s_sample_rate_hz;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_command_task_handle;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool s_notify_enabled;
static bool s_initialized;

static char s_rx_line[BLE_SERIAL_LINE_MAX];
static size_t s_rx_line_len;

static const ble_uuid128_t s_nus_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static int ble_serial_gap_event(struct ble_gap_event *event, void *arg);
static int ble_serial_gatt_access(uint16_t conn_handle,
                                  uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg);

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_nus_tx_uuid.u,
                .access_cb = ble_serial_gatt_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_nus_rx_uuid.u,
                .access_cb = ble_serial_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {0},
};

static char *ble_serial_trim(char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static const char *ble_serial_display_state_name(display_state_t state)
{
    switch (state) {
    case DISPLAY_STATE_IDLE:
        return "idle";
    case DISPLAY_STATE_LISTENING:
        return "listening";
    case DISPLAY_STATE_THINKING:
        return "thinking";
    case DISPLAY_STATE_ANSWERING:
        return "answering";
    case DISPLAY_STATE_WIFI:
        return "wifi";
    case DISPLAY_STATE_STATUS:
        return "status";
    case DISPLAY_STATE_USAGE_GUIDE:
        return "usage_guide";
    case DISPLAY_STATE_CLOCK:
        return "clock";
    case DISPLAY_STATE_CINNAMOROLL:
        return "cinnamoroll";
    case DISPLAY_STATE_GAME:
        return "game";
    case DISPLAY_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static esp_err_t ble_serial_send_bytes(const char *data, size_t len)
{
    if (!s_initialized || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        !s_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > BLE_SERIAL_NOTIFY_CHUNK) {
            chunk = BLE_SERIAL_NOTIFY_CHUNK;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + offset, chunk);
        if (om == NULL) {
            return ESP_ERR_NO_MEM;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        if (rc != 0) {
            return ESP_FAIL;
        }

        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return ESP_OK;
}

esp_err_t ble_serial_send_line(const char *line)
{
    if (line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ble_serial_send_bytes(line, strlen(line));
    if (err != ESP_OK) {
        return err;
    }
    return ble_serial_send_bytes("\r\n", 2);
}

static void ble_serial_queue_line(const char *line)
{
    if (s_command_queue == NULL || line == NULL || line[0] == '\0') {
        return;
    }

    ble_serial_command_t cmd = {0};
    strlcpy(cmd.line, line, sizeof(cmd.line));
    if (xQueueSend(s_command_queue, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "Command queue full, dropped: %s", line);
        (void)ble_serial_send_line("ERR busy");
    }
}

static void ble_serial_handle_rx_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char ch = (char)data[i];
        if (ch == '\r' || ch == '\n') {
            if (s_rx_line_len > 0) {
                s_rx_line[s_rx_line_len] = '\0';
                char *line = ble_serial_trim(s_rx_line);
                ble_serial_queue_line(line);
                s_rx_line_len = 0;
            }
            continue;
        }

        if (s_rx_line_len + 1 >= sizeof(s_rx_line)) {
            s_rx_line_len = 0;
            (void)ble_serial_send_line("ERR line too long");
            continue;
        }

        s_rx_line[s_rx_line_len++] = ch;
    }
}

static int ble_serial_gatt_access(uint16_t conn_handle,
                                  uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return os_mbuf_append(ctxt->om, "DeskRobot BLE UART", 18) == 0
            ? 0
            : BLE_ATT_ERR_INSUFFICIENT_RES;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[BLE_SERIAL_LINE_MAX];
        if (len > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        int rc = os_mbuf_copydata(ctxt->om, 0, len, buf);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        ble_serial_handle_rx_bytes(buf, len);
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void ble_serial_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (const uint8_t *)BLE_SERIAL_DEVICE_NAME;
    fields.name_len = strlen(BLE_SERIAL_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_serial_gap_event,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
    }
}

static int ble_serial_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "BLE connected, handle=%u", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connect failed, status=%d", event->connect.status);
            ble_serial_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        s_rx_line_len = 0;
        ble_serial_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_serial_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "BLE notify %s", s_notify_enabled ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated: %u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void ble_serial_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void ble_serial_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
        return;
    }

    ble_serial_advertise();
}

static void ble_serial_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_serial_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_SERIAL_DEVICE_NAME);
    if (rc != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t ble_serial_handle_servo_command(char *args)
{
    if (args == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *saveptr = NULL;
    char *axis = strtok_r(args, " ", &saveptr);
    char *value = strtok_r(NULL, " ", &saveptr);

    if (axis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcasecmp(axis, "center") == 0) {
        servo_center();
        return ble_serial_send_line("servo center");
    }
    if (strcasecmp(axis, "down") == 0) {
        servo_look_down();
        return ble_serial_send_line("servo down");
    }

    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int delta = atoi(value);
    if (delta == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    servo_axis_t servo_axis;
    if (strcasecmp(axis, "h") == 0 || strcasecmp(axis, "horizontal") == 0) {
        servo_axis = SERVO_AXIS_HORIZONTAL;
    } else if (strcasecmp(axis, "v") == 0 || strcasecmp(axis, "vertical") == 0) {
        servo_axis = SERVO_AXIS_VERTICAL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (delta > BLE_SERIAL_SERVO_STEP_DEG) {
        delta = BLE_SERIAL_SERVO_STEP_DEG;
    } else if (delta < -BLE_SERIAL_SERVO_STEP_DEG) {
        delta = -BLE_SERIAL_SERVO_STEP_DEG;
    }

    esp_err_t err = servo_move_relative(servo_axis, delta);
    if (err != ESP_OK) {
        return err;
    }

    char reply[48];
    snprintf(reply, sizeof(reply), "servo %s %+d", axis, delta);
    return ble_serial_send_line(reply);
}

static void ble_serial_send_status(void)
{
    audio_buffer_state_t audio_state = {0};
    if (s_audio_buffer != NULL) {
        audio_buffer_get_state(s_audio_buffer, &audio_state);
    }
    uint32_t stage1_ms = 0;
    uint32_t stage2_ms = 0;
    power_manager_get_idle_timeouts_ms(&stage1_ms, &stage2_ms);

    char reply[224];
    snprintf(reply,
             sizeof(reply),
             "status wifi=%s power=%s display=%s rec=%d play=%d bytes=%u timeout=%us/%us",
             wifi_network_get_status_string(),
             power_manager_get_stage_string(),
             ble_serial_display_state_name(display_get_state()),
             audio_state.recording ? 1 : 0,
             audio_state.playing ? 1 : 0,
             (unsigned)audio_state.recorded_size,
             (unsigned)(stage1_ms / 1000U),
             (unsigned)(stage2_ms / 1000U));
    (void)ble_serial_send_line(reply);
}

static void ble_serial_send_result(const char *ok_reply,
                                   const char *err_prefix,
                                   esp_err_t err)
{
    if (err == ESP_OK) {
        (void)ble_serial_send_line(ok_reply);
        return;
    }

    char reply[80];
    snprintf(reply, sizeof(reply), "ERR %s: %s", err_prefix, esp_err_to_name(err));
    (void)ble_serial_send_line(reply);
}

static void ble_serial_handle_record_command(char *args)
{
    if (s_audio_buffer == NULL) {
        (void)ble_serial_send_line("ERR record unavailable");
        return;
    }

    audio_buffer_state_t audio_state = {0};
    audio_buffer_get_state(s_audio_buffer, &audio_state);

    if (args == NULL || args[0] == '\0' || strcasecmp(args, "toggle") == 0) {
        args = audio_state.recording ? "stop" : "start";
    }

    if (strcasecmp(args, "start") == 0) {
        if (audio_state.playing) {
            (void)ble_serial_send_line("ERR stop playback first");
            return;
        }
        if (audio_state.recording) {
            (void)ble_serial_send_line("recording already active");
            return;
        }

        audio_mic_set_recording(true);
        display_set_listening(0);
        esp_err_t err = servo_start_listening_motion();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "listening servo motion rejected: %s", esp_err_to_name(err));
        }
        (void)ble_serial_send_line("record start");
        return;
    }

    if (strcasecmp(args, "stop") == 0) {
        if (!audio_state.recording) {
            (void)ble_serial_send_line("recording already stopped");
            return;
        }

        audio_mic_set_recording(false);
        servo_stop_listening_motion();
        display_set_status("Recorded", "BLE upload/play");
        (void)ble_serial_send_line("record stop");
        return;
    }

    (void)ble_serial_send_line("ERR usage: record [start|stop|toggle]");
}

static void ble_serial_handle_play_command(char *args)
{
    if (s_audio_buffer == NULL || s_sample_rate_hz == 0) {
        (void)ble_serial_send_line("ERR play unavailable");
        return;
    }

    audio_buffer_state_t audio_state = {0};
    audio_buffer_get_state(s_audio_buffer, &audio_state);
    if (args == NULL || args[0] == '\0' || strcasecmp(args, "toggle") == 0) {
        args = audio_state.playing ? "stop" : "start";
    }
    if (strcasecmp(args, "stop") == 0) {
        esp_err_t err = audio_spk_set_playing(false);
        if (err == ESP_OK) {
            display_set_idle();
        }
        ble_serial_send_result("play stop", "play stop rejected", err);
        return;
    }
    if (strcasecmp(args, "start") != 0) {
        (void)ble_serial_send_line("ERR usage: play [start|stop|toggle]");
        return;
    }
    if (audio_state.recording) {
        (void)ble_serial_send_line("ERR stop recording first");
        return;
    }

    esp_err_t err = audio_spk_set_playing(true);
    if (err == ESP_OK) {
        audio_buffer_get_state(s_audio_buffer, &audio_state);
        uint32_t total_sec = (uint32_t)((audio_state.recorded_size / sizeof(int16_t)) /
                                        s_sample_rate_hz);
        display_set_answering(0, total_sec);
    }
    ble_serial_send_result("play start", "play rejected", err);
}

static void ble_serial_handle_upload_command(char *args)
{
    if (args != NULL && args[0] != '\0') {
        (void)ble_serial_send_line("ERR usage: upload");
        return;
    }

    if (s_audio_buffer == NULL || s_sample_rate_hz == 0) {
        (void)ble_serial_send_line("ERR upload unavailable");
        return;
    }

    audio_buffer_state_t audio_state = {0};
    audio_buffer_get_state(s_audio_buffer, &audio_state);
    if (audio_state.recording || audio_state.playing) {
        (void)ble_serial_send_line("ERR stop recording/playback first");
        return;
    }
    if (!audio_state.recording_complete || audio_state.recorded_size == 0) {
        (void)ble_serial_send_line("ERR upload needs a completed recording");
        return;
    }

    esp_err_t err = voice_assistant_start_full_test(s_audio_buffer, s_sample_rate_hz);
    ble_serial_send_result("upload start", "upload rejected", err);
}

static void ble_serial_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BLE_SERIAL_RESTART_DELAY_MS));
    esp_restart();
}

static void ble_serial_show_help(void)
{
    (void)ble_serial_send_line("DeskRobot BLE commands:");
    (void)ble_serial_send_line("  Commands are case-insensitive; end each command with newline.");
    (void)ble_serial_send_line("Workflow:");
    (void)ble_serial_send_line("  record - toggle recording, same as K1");
    (void)ble_serial_send_line("  play - toggle playback of the last recording, same as K2");
    (void)ble_serial_send_line("  upload - upload the completed recording, then run ASR/LLM/TTS, same as K7");
    (void)ble_serial_send_line("  Example: record -> record -> upload -> play");
    (void)ble_serial_send_line("Audio:");
    (void)ble_serial_send_line("  record [start|stop|toggle] - start/stop/toggle recording");
    (void)ble_serial_send_line("  play [start|stop|toggle] - start/stop/toggle playback");
    (void)ble_serial_send_line("  upload - requires a completed recording and idle audio state");
    (void)ble_serial_send_line("  volume next - cycle speaker volume: 50%, 100%, 150%, 200%");
    (void)ble_serial_send_line("Status and display:");
    (void)ble_serial_send_line("  status - show wifi, power, display, audio, and timeout status");
    (void)ble_serial_send_line("  display dog|clock|guide|status - switch display page");
    (void)ble_serial_send_line("  game - start jump obstacle game");
    (void)ble_serial_send_line("  jump - jump in game; restart after game over");
    (void)ble_serial_send_line("  exit - leave dog, clock, guide, status, or game page and return to idle");
    (void)ble_serial_send_line("Config and power:");
    (void)ble_serial_send_line("  config wifi - enter Wi-Fi configuration mode");
    (void)ble_serial_send_line("  config power-timeout <stage1_sec> <stage2_sec> - set idle timers");
    (void)ble_serial_send_line("  power stage1|stage2 - enter low-power stage manually");
    (void)ble_serial_send_line("Motion:");
    (void)ble_serial_send_line("  servo center - center both servos");
    (void)ble_serial_send_line("  servo down - look down");
    (void)ble_serial_send_line("  servo h|horizontal <deg> - move horizontal servo, clamped to +/-10");
    (void)ble_serial_send_line("  servo v|vertical <deg> - move vertical servo, clamped to +/-10");
    (void)ble_serial_send_line("System:");
    (void)ble_serial_send_line("  help - show this help");
    (void)ble_serial_send_line("  rst - restart the board");
}

static void ble_serial_handle_power_command(char *args)
{
    if (args == NULL) {
        (void)ble_serial_send_line("ERR usage: power stage1|stage2");
        return;
    }

    char *saveptr = NULL;
    char *sub = strtok_r(args, " ", &saveptr);
    if (sub == NULL) {
        (void)ble_serial_send_line("ERR usage: power stage1|stage2");
        return;
    }

    if (strcasecmp(sub, "stage1") == 0) {
        esp_err_t err = power_manager_enter_stage1();
        ble_serial_send_result("power stage1", "power stage1 rejected", err);
        return;
    }
    if (strcasecmp(sub, "stage2") == 0) {
        esp_err_t err = power_manager_enter_stage2();
        ble_serial_send_result("power stage2", "power stage2 rejected", err);
        return;
    }

    (void)ble_serial_send_line("ERR usage: power stage1|stage2");
}

static void ble_serial_handle_config_command(char *args)
{
    if (args == NULL) {
        (void)ble_serial_send_line("ERR usage: config wifi|power-timeout <stage1_sec> <stage2_sec>");
        return;
    }

    char *saveptr = NULL;
    char *sub = strtok_r(args, " ", &saveptr);
    if (sub == NULL) {
        (void)ble_serial_send_line("ERR usage: config wifi|power-timeout <stage1_sec> <stage2_sec>");
        return;
    }

    if (strcasecmp(sub, "wifi") == 0) {
        esp_err_t err = wifi_network_start_config_mode(false);
        ble_serial_send_result("config wifi", "config wifi rejected", err);
        return;
    }

    if (strcasecmp(sub, "power-timeout") == 0) {
        char *stage1_text = strtok_r(NULL, " ", &saveptr);
        char *stage2_text = strtok_r(NULL, " ", &saveptr);
        if (stage1_text == NULL || stage2_text == NULL) {
            (void)ble_serial_send_line("ERR usage: config power-timeout <stage1_sec> <stage2_sec>");
            return;
        }

        uint32_t stage1_sec = (uint32_t)strtoul(stage1_text, NULL, 10);
        uint32_t stage2_sec = (uint32_t)strtoul(stage2_text, NULL, 10);
        esp_err_t err = power_manager_set_idle_timeouts_ms(stage1_sec * 1000U,
                                                           stage2_sec * 1000U);
        if (err == ESP_OK) {
            char reply[64];
            snprintf(reply, sizeof(reply), "config power-timeout %us %us",
                     (unsigned)stage1_sec,
                     (unsigned)stage2_sec);
            (void)ble_serial_send_line(reply);
        } else {
            ble_serial_send_result("", "config power-timeout rejected", err);
        }
        return;
    }

    (void)ble_serial_send_line("ERR usage: config wifi|power-timeout <stage1_sec> <stage2_sec>");
}

static void ble_serial_handle_display_command(char *args)
{
    if (args == NULL) {
        (void)ble_serial_send_line("ERR usage: display dog|clock|guide|status");
        return;
    }

    if (strcasecmp(args, "dog") == 0) {
        display_set_cinnamoroll_mode(true);
        (void)ble_serial_send_line("display dog");
        return;
    }
    if (strcasecmp(args, "clock") == 0) {
        display_set_clock_mode(true);
        (void)ble_serial_send_line("display clock");
        return;
    }
    if (strcasecmp(args, "guide") == 0) {
        display_set_usage_guide_mode(true);
        (void)ble_serial_send_line("display guide");
        return;
    }
    if (strcasecmp(args, "status") == 0) {
        system_status_show_next_page();
        system_status_log_snapshot();
        (void)ble_serial_send_line("display status");
        return;
    }

    (void)ble_serial_send_line("ERR usage: display dog|clock|guide|status");
}

static void ble_serial_handle_exit_command(void)
{
    display_state_t state = display_get_state();

    switch (state) {
    case DISPLAY_STATE_CINNAMOROLL:
        display_set_cinnamoroll_mode(false);
        (void)ble_serial_send_line("exit idle");
        break;

    case DISPLAY_STATE_CLOCK:
        display_set_clock_mode(false);
        (void)ble_serial_send_line("exit idle");
        break;

    case DISPLAY_STATE_USAGE_GUIDE:
        display_set_usage_guide_mode(false);
        (void)ble_serial_send_line("exit idle");
        break;

    case DISPLAY_STATE_STATUS:
        display_set_idle();
        (void)ble_serial_send_line("exit idle");
        break;

    case DISPLAY_STATE_GAME:
        display_set_game_mode(false);
        (void)ble_serial_send_line("exit idle");
        break;

    default: {
        char reply[48];
        snprintf(reply, sizeof(reply), "exit ignored state=%s",
                 ble_serial_display_state_name(state));
        (void)ble_serial_send_line(reply);
        break;
    }
    }
}

static void ble_serial_handle_command(char *line)
{
    char *cmd = ble_serial_trim(line);
    if (cmd[0] == '\0') {
        return;
    }

    if (power_manager_is_low_power()) {
        power_manager_notify_activity();
        (void)ble_serial_send_line("wake");
        return;
    }

    char *saveptr = NULL;
    char *name = strtok_r(cmd, " ", &saveptr);
    char *args = strtok_r(NULL, "", &saveptr);
    if (args != NULL) {
        args = ble_serial_trim(args);
    }

    power_manager_notify_activity();

    if (strcasecmp(name, "help") == 0) {
        ble_serial_show_help();
    } else if (strcasecmp(name, "config") == 0) {
        ble_serial_handle_config_command(args);
    } else if (strcasecmp(name, "status") == 0) {
        ble_serial_send_status();
    } else if (strcasecmp(name, "power") == 0) {
        ble_serial_handle_power_command(args);
    } else if (strcasecmp(name, "play") == 0) {
        ble_serial_handle_play_command(args);
    } else if (strcasecmp(name, "record") == 0) {
        ble_serial_handle_record_command(args);
    } else if (strcasecmp(name, "upload") == 0) {
        ble_serial_handle_upload_command(args);
    } else if (strcasecmp(name, "volume") == 0) {
        if (args != NULL && strcasecmp(args, "next") == 0) {
            audio_spk_cycle_volume_level();
            (void)ble_serial_send_line("volume next");
        } else {
            (void)ble_serial_send_line("ERR usage: volume next");
        }
    } else if (strcasecmp(name, "servo") == 0) {
        esp_err_t err = ble_serial_handle_servo_command(args);
        if (err != ESP_OK) {
            (void)ble_serial_send_line("ERR usage: servo h|v +/-10|center|down");
        }
    } else if (strcasecmp(name, "display") == 0) {
        ble_serial_handle_display_command(args);
    } else if (strcasecmp(name, "game") == 0) {
        if (args != NULL && args[0] != '\0') {
            (void)ble_serial_send_line("ERR usage: game");
        } else {
            display_set_game_mode(true);
            (void)ble_serial_send_line("game start");
        }
    } else if (strcasecmp(name, "jump") == 0) {
        if (args != NULL && args[0] != '\0') {
            (void)ble_serial_send_line("ERR usage: jump");
        } else if (display_get_state() != DISPLAY_STATE_GAME) {
            (void)ble_serial_send_line("ERR game not running");
        } else {
            display_game_jump();
            (void)ble_serial_send_line("jump");
        }
    } else if (strcasecmp(name, "exit") == 0) {
        ble_serial_handle_exit_command();
    } else if (strcasecmp(name, "rst") == 0) {
        (void)ble_serial_send_line("rst");
        if (xTaskCreate(ble_serial_restart_task,
                        "ble_rst",
                        2048,
                        NULL,
                        BLE_SERIAL_TASK_PRIO,
                        NULL) != pdPASS) {
            esp_restart();
        }
    } else {
        (void)ble_serial_send_line("ERR unknown command");
    }
}

static void ble_serial_command_task(void *arg)
{
    (void)arg;

    ble_serial_command_t cmd;
    while (1) {
        if (xQueueReceive(s_command_queue, &cmd, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "Command: %s", cmd.line);
            ble_serial_handle_command(cmd.line);
        }
    }
}

bool ble_serial_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

esp_err_t ble_serial_init(audio_buffer_t *audio_buffer, uint32_t sample_rate_hz)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_audio_buffer = audio_buffer;
    s_sample_rate_hz = sample_rate_hz;
    s_command_queue = xQueueCreate(BLE_SERIAL_QUEUE_LEN, sizeof(ble_serial_command_t));
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ble_serial_command_task,
                    "ble_serial_cmd",
                    BLE_SERIAL_TASK_STACK,
                    NULL,
                    BLE_SERIAL_TASK_PRIO,
                    &s_command_task_handle) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = ble_serial_on_reset;
    ble_hs_cfg.sync_cb = ble_serial_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    err = ble_serial_gatt_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_serial_host_task);

    s_initialized = true;
    ESP_LOGI(TAG, "BLE UART initialized as %s", BLE_SERIAL_DEVICE_NAME);
    return ESP_OK;
}

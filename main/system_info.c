#include "system_info.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_clk_tree.h>
#include <esp_heap_caps.h>
#include <esp_random.h>


#define TAG "SystemInfo"

void SystemInfo_PrintClockFreq() {

    uint32_t cpu_freq = 0;
    
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                 &cpu_freq);
    ESP_LOGI(TAG, "CPU Frequency: %lu Hz", cpu_freq);

    int64_t start = esp_timer_get_time(); 
    vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t end = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "Expected: 1000 ms, Actual: %lld ms", end / 1000);
}

size_t SystemInfo_GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo_GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo_GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

esp_err_t SystemInfo_PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo_PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo_PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    // int cur_task_free_sram = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "free sram: %u, minimal sram: %u", free_sram, min_free_sram);
}

void SystemInfo_PrintPsramStats() {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t used = total_psram - free_psram;
    ESP_LOGI(TAG, "PSRAM total: %u, free: %u, used: %u", total_psram, free_psram, used);
}


#define MAC_ADDR_LEN  13  // 12 hex chars + null terminator
static uint8_t mac[6] = {0};
static char mac_str[MAC_ADDR_LEN] = {0};
char* SystemInfo_GetMACAddress() {
    if (mac_str[0] != '\0') {
        return mac_str;
    }
    // uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(mac_str, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mac_str;
}


static char s_uuid_str[37] = {0};
char* SystemInfo_GetUUID() {

    if (mac_str[0] == '\0') {
        SystemInfo_GetMACAddress();
    }

    // Get FreeRTOS tick count (since boot)
    TickType_t tick = xTaskGetTickCount();

    // Generate 6 random bytes
    uint8_t rand_bytes[6];
    for (int i = 0; i < 6; i++) {
        rand_bytes[i] = (uint8_t)(esp_random() & 0xFF);
    }

    // Use tick value in place of 'time'
    // Split 32-bit tick into 4 bytes
    uint8_t t0 = (uint8_t)((tick >> 24) & 0xFF);
    uint8_t t1 = (uint8_t)((tick >> 16) & 0xFF);
    uint8_t t2 = (uint8_t)((tick >> 8)  & 0xFF);
    uint8_t t3 = (uint8_t)((tick)       & 0xFF);

    snprintf(s_uuid_str, sizeof(s_uuid_str),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], t0,
        t1, t2,
        t3, mac[3],
        mac[4], mac[5],
        rand_bytes[0], rand_bytes[1], rand_bytes[2],
        rand_bytes[3], rand_bytes[4], rand_bytes[5]);

    return s_uuid_str;
}

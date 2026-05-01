#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

void SystemInfo_PrintClockFreq();

size_t SystemInfo_GetFlashSize();

size_t SystemInfo_GetMinimumFreeHeapSize();

size_t SystemInfo_GetFreeHeapSize();

esp_err_t SystemInfo_PrintTaskCpuUsage(TickType_t xTicksToWait);

void SystemInfo_PrintTaskList();

void SystemInfo_PrintHeapStats();

void SystemInfo_PrintPsramStats();

char* SystemInfo_GetMACAddress();

char* SystemInfo_GetUUID();

#ifdef __cplusplus
}
#endif

#endif // _SYSTEM_INFO_H_
#ifndef TEST_FREERTOS_TASK_H
#define TEST_FREERTOS_TASK_H
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
unsigned uxTaskGetStackHighWaterMark(void *task);
#endif

#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;

static inline void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

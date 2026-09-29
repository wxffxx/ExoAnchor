#pragma once

#include <stdbool.h>
#include <stdlib.h>

#include "FreeRTOS.h"

typedef struct {
    bool held;
} si_host_semaphore_t;

typedef si_host_semaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(si_host_semaphore_t));
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                                        TickType_t timeout)
{
    (void)timeout;
    if (!semaphore || semaphore->held) {
        return pdFALSE;
    }
    semaphore->held = true;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore || !semaphore->held) {
        return pdFALSE;
    }
    semaphore->held = false;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    free(semaphore);
}

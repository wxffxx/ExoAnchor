#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/*
 * Stream buffer whose storage lives in PSRAM. Use only for buffers that tasks
 * (never ISRs) read and write.
 *
 * ESP-IDF 5.5.5 provides xStreamBufferCreateWithCaps(), but its matching
 * vStreamBufferDeleteWithCaps() deletes the handle with vSemaphoreDelete()
 * and double-frees the heap block, which aborted the board when an SSH
 * terminal closed. Build it from the static API and free it ourselves.
 */
static inline StreamBufferHandle_t si_psram_stream_buffer_create(
    size_t size_bytes, size_t trigger_level_bytes)
{
    StaticStreamBuffer_t *control =
        heap_caps_calloc(1, sizeof(*control), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *storage = heap_caps_malloc(size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StreamBufferHandle_t handle = NULL;
    if (control && storage) {
        handle = xStreamBufferCreateStatic(size_bytes, trigger_level_bytes,
                                           storage, control);
    }
    if (!handle) {
        heap_caps_free(storage);
        heap_caps_free(control);
    }
    return handle;
}

static inline void si_psram_stream_buffer_delete(StreamBufferHandle_t handle)
{
    if (!handle) {
        return;
    }
    uint8_t *storage = NULL;
    StaticStreamBuffer_t *control = NULL;
    BaseType_t found = xStreamBufferGetStaticBuffers(handle, &storage, &control);
    vStreamBufferDelete(handle);
    if (found == pdTRUE) {
        heap_caps_free(storage);
        heap_caps_free(control);
    }
}

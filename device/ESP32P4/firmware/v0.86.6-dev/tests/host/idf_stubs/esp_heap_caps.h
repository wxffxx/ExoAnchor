#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1U << 0)
#define MALLOC_CAP_SPIRAM (1U << 1)

static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size,
                                            uint32_t caps)
{
    (void)alignment;
    (void)caps;
    return malloc(size);
}

#pragma once

/*
 * Lock-free cancellation fence for an HID command that may emit more than
 * one USB report.  The execution manager serializes command start and
 * authority changes; this generation lets a command already past that start
 * boundary observe a revoke while it is in an endpoint wait or click/combo
 * delay.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t generation;
} si_hid_abort_fence_t;

#define SI_HID_ABORT_FENCE_INITIALIZER { .generation = 1U }

uint32_t si_hid_abort_fence_snapshot(const si_hid_abort_fence_t *fence);
uint32_t si_hid_abort_fence_advance(si_hid_abort_fence_t *fence);
bool si_hid_abort_fence_is_current(const si_hid_abort_fence_t *fence,
                                   uint32_t expected_generation);

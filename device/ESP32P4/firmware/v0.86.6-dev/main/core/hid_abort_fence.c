#include "hid_abort_fence.h"

uint32_t si_hid_abort_fence_snapshot(const si_hid_abort_fence_t *fence)
{
    if (!fence) {
        return 0U;
    }
    return __atomic_load_n(&fence->generation, __ATOMIC_ACQUIRE);
}

uint32_t si_hid_abort_fence_advance(si_hid_abort_fence_t *fence)
{
    if (!fence) {
        return 0U;
    }

    uint32_t previous =
        __atomic_load_n(&fence->generation, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = previous + 1U;
        if (next == 0U) {
            next = 1U;
        }
    } while (!__atomic_compare_exchange_n(
        &fence->generation, &previous, next, false,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    return next;
}

bool si_hid_abort_fence_is_current(const si_hid_abort_fence_t *fence,
                                   uint32_t expected_generation)
{
    return expected_generation != 0U &&
           si_hid_abort_fence_snapshot(fence) == expected_generation;
}

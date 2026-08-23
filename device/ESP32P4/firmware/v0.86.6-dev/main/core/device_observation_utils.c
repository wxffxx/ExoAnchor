#include "device_observation_utils.h"

#include <inttypes.h>
#include <stdio.h>

double si_observation_used_percent(uint64_t total_bytes, uint64_t free_bytes)
{
    if (total_bytes == 0) {
        return 0.0;
    }
    if (free_bytes > total_bytes) {
        free_bytes = total_bytes;
    }
    return ((double)(total_bytes - free_bytes) * 100.0) / (double)total_bytes;
}

void si_observation_format_uptime(uint32_t seconds, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    uint32_t days = seconds / 86400U;
    uint32_t hours = (seconds % 86400U) / 3600U;
    uint32_t minutes = (seconds % 3600U) / 60U;
    snprintf(out, out_size, "%" PRIu32 "d %" PRIu32 "h %" PRIu32 "m",
             days, hours, minutes);
}

size_t si_observation_recent_window(size_t head, size_t count, size_t capacity,
                                    size_t requested, size_t *start_out)
{
    if (start_out) {
        *start_out = 0;
    }
    if (capacity == 0 || count == 0 || requested == 0) {
        return 0;
    }
    if (count > capacity) {
        count = capacity;
    }
    size_t total = requested < count ? requested : count;
    if (start_out) {
        *start_out = (head + capacity - total) % capacity;
    }
    return total;
}

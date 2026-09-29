#pragma once

#include <stddef.h>
#include <stdint.h>

double si_observation_used_percent(uint64_t total_bytes, uint64_t free_bytes);
void si_observation_format_uptime(uint32_t seconds, char *out, size_t out_size);
size_t si_observation_recent_window(size_t head, size_t count, size_t capacity,
                                    size_t requested, size_t *start_out);

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SI_DISCOVERY_RATE_WINDOW_MS 1000U
#define SI_DISCOVERY_RATE_PER_SOURCE 8U
#define SI_DISCOVERY_RATE_GLOBAL 64U
#define SI_DISCOVERY_RATE_SOURCE_SLOTS 16U

typedef struct {
    uint32_t source_ipv4;
    uint32_t window_started_ms;
    uint32_t last_seen_ms;
    uint16_t requests;
    bool used;
} si_discovery_source_rate_t;

typedef struct {
    uint32_t global_window_started_ms;
    uint16_t global_requests;
    uint8_t replacement_cursor;
    bool initialized;
    si_discovery_source_rate_t sources[SI_DISCOVERY_RATE_SOURCE_SLOTS];
} si_discovery_rate_limiter_t;

bool si_discovery_rate_limit_allow(si_discovery_rate_limiter_t *limiter,
                                   uint32_t source_ipv4,
                                   uint32_t now_ms);

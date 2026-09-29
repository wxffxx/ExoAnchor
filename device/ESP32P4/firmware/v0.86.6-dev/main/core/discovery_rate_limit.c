#include "discovery_rate_limit.h"

#include <stddef.h>

static bool window_expired(uint32_t now_ms, uint32_t started_ms)
{
    return (uint32_t)(now_ms - started_ms) >=
           SI_DISCOVERY_RATE_WINDOW_MS;
}

static si_discovery_source_rate_t *source_bucket(
    si_discovery_rate_limiter_t *limiter, uint32_t source_ipv4,
    uint32_t now_ms)
{
    si_discovery_source_rate_t *free_bucket = NULL;
    for (size_t i = 0; i < SI_DISCOVERY_RATE_SOURCE_SLOTS; ++i) {
        si_discovery_source_rate_t *bucket = &limiter->sources[i];
        if (bucket->used && bucket->source_ipv4 == source_ipv4) {
            return bucket;
        }
        if (!bucket->used && !free_bucket) {
            free_bucket = bucket;
        }
    }
    si_discovery_source_rate_t *bucket = free_bucket;
    if (!bucket) {
        bucket = &limiter->sources[limiter->replacement_cursor];
        limiter->replacement_cursor =
            (uint8_t)((limiter->replacement_cursor + 1U) %
                      SI_DISCOVERY_RATE_SOURCE_SLOTS);
    }
    *bucket = (si_discovery_source_rate_t){
        .source_ipv4 = source_ipv4,
        .window_started_ms = now_ms,
        .last_seen_ms = now_ms,
        .used = true,
    };
    return bucket;
}

bool si_discovery_rate_limit_allow(si_discovery_rate_limiter_t *limiter,
                                   uint32_t source_ipv4,
                                   uint32_t now_ms)
{
    if (!limiter || source_ipv4 == 0) {
        return false;
    }
    if (!limiter->initialized ||
        window_expired(now_ms, limiter->global_window_started_ms)) {
        limiter->initialized = true;
        limiter->global_window_started_ms = now_ms;
        limiter->global_requests = 0;
    }
    if (limiter->global_requests >= SI_DISCOVERY_RATE_GLOBAL) {
        return false;
    }

    si_discovery_source_rate_t *bucket =
        source_bucket(limiter, source_ipv4, now_ms);
    if (window_expired(now_ms, bucket->window_started_ms)) {
        bucket->window_started_ms = now_ms;
        bucket->requests = 0;
    }
    bucket->last_seen_ms = now_ms;
    if (bucket->requests >= SI_DISCOVERY_RATE_PER_SOURCE) {
        return false;
    }
    bucket->requests++;
    limiter->global_requests++;
    return true;
}

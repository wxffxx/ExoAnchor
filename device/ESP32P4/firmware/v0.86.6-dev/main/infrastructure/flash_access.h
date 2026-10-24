#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * SPI flash operations (NVS, otadata, partition reads) disable the cache.
 * The Dev H.264 profile keeps code in flash rather than PSRAM, so a task
 * whose stack lives in PSRAM cannot survive that window. Those tasks hand the
 * operation to one small internal-stack proxy task and wait for it; tasks
 * with internal stacks run it directly.
 *
 * Arguments and buffers may live on the caller's PSRAM stack: the caller is
 * blocked until the proxy finishes, and the flash driver bounces data that is
 * not in internal RAM. Calls are serialized; keep them short.
 */
typedef esp_err_t (*si_flash_access_fn_t)(void *ctx);

/* Start the proxy task. Call once at boot after nvs_flash_init(). */
esp_err_t si_flash_access_init(void);

/* True when the calling task may touch flash directly. */
bool si_flash_access_direct(void);

/* Run fn(ctx) where flash access is safe and return its result.
 * Returns ESP_ERR_INVALID_STATE if a proxy is needed but not running and
 * ESP_ERR_TIMEOUT if the proxy stays busy for too long. */
esp_err_t si_flash_access_call(si_flash_access_fn_t fn, void *ctx);

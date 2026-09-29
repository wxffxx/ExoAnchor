#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure Host display-mode planning and Linux command generation.
 *
 * Ownership/concurrency: callers own all buffers; this module owns no state
 * and is safe to call concurrently.
 * Input boundary: connector, plan id, dimensions, and refresh are strictly
 * validated before they may be interpolated into a shell command.
 * Error semantics: invalid/unsupported requests fail without partial output.
 */

#define SI_HOST_DISPLAY_CONNECTOR_MAX 31U
#define SI_HOST_DISPLAY_PLAN_ID_MAX 47U
#define SI_HOST_DISPLAY_MODE_MAX 63U
#define SI_HOST_DISPLAY_COMMAND_MAX 3072U

typedef enum {
    SI_HOST_DISPLAY_OK = 0,
    SI_HOST_DISPLAY_INVALID_ARGUMENT,
    SI_HOST_DISPLAY_UNSUPPORTED,
    SI_HOST_DISPLAY_OUTPUT_TOO_SMALL,
} si_host_display_result_t;

typedef struct {
    char connector[SI_HOST_DISPLAY_CONNECTOR_MAX + 1U];
    uint16_t width;
    uint16_t height;
    uint32_t refresh_millihz;
    bool persistent;
} si_host_display_request_t;

si_host_display_result_t
si_host_display_validate_request(const si_host_display_request_t *request);

si_host_display_result_t si_host_display_format_mode(
    const si_host_display_request_t *request, char *out, size_t out_size);

si_host_display_result_t si_host_display_build_observe_command(
    char *out, size_t out_size);

si_host_display_result_t si_host_display_build_apply_command(
    const char *plan_id, const si_host_display_request_t *request,
    char *out, size_t out_size);

si_host_display_result_t si_host_display_build_verify_command(
    const char *plan_id, const si_host_display_request_t *request,
    char *out, size_t out_size);

si_host_display_result_t si_host_display_build_rollback_command(
    const char *plan_id, char *out, size_t out_size);

const char *si_host_display_result_name(si_host_display_result_t result);

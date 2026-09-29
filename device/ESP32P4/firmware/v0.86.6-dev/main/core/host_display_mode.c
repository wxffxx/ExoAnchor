#include "host_display_mode.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool safe_identifier(const char *value, size_t max_len)
{
    if (!value || !value[0]) {
        return false;
    }
    size_t len = strlen(value);
    if (len > max_len || !isalnum((unsigned char)value[0])) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

static si_host_display_result_t checked_snprintf(
    char *out, size_t out_size, int written)
{
    if (!out || out_size == 0U || written < 0 ||
        (size_t)written >= out_size) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_OUTPUT_TOO_SMALL;
    }
    return SI_HOST_DISPLAY_OK;
}

si_host_display_result_t
si_host_display_validate_request(const si_host_display_request_t *request)
{
    if (!request ||
        !safe_identifier(request->connector,
                         SI_HOST_DISPLAY_CONNECTOR_MAX) ||
        request->width < 320U || request->width > 7680U ||
        request->height < 200U || request->height > 4320U ||
        request->refresh_millihz < 10000U ||
        request->refresh_millihz > 240000U ||
        request->refresh_millihz % 1000U != 0U) {
        return SI_HOST_DISPLAY_INVALID_ARGUMENT;
    }
    return SI_HOST_DISPLAY_OK;
}

si_host_display_result_t si_host_display_format_mode(
    const si_host_display_request_t *request, char *out, size_t out_size)
{
    if (si_host_display_validate_request(request) != SI_HOST_DISPLAY_OK) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_INVALID_ARGUMENT;
    }
    int written = snprintf(out, out_size, "%s:%ux%u@%u",
                           request->connector, (unsigned)request->width,
                           (unsigned)request->height,
                           (unsigned)(request->refresh_millihz / 1000U));
    return checked_snprintf(out, out_size, written);
}

si_host_display_result_t si_host_display_build_observe_command(
    char *out, size_t out_size)
{
    static const char command[] =
        "set -eu; "
        "printf 'schema=exoanchor.host_display.v1\\n'; "
        "printf 'kernel='; uname -r; "
        "printf 'cmdline='; cat /proc/cmdline; "
        "printf 'os_id='; "
        "( . /etc/os-release 2>/dev/null; printf '%s\\n' \"${ID:-unknown}\" ) || "
        "printf 'unknown\\n'; "
        "printf 'fb_virtual_size='; "
        "cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || printf 'unavailable\\n'; "
        "printf 'grub_dropin_support='; "
        "test -d /etc/default/grub.d && printf 'yes\\n' || printf 'no\\n'; "
        "for d in /sys/class/drm/card*-*; do "
        "test -d \"$d\" || continue; "
        "n=${d##*/}; "
        "printf 'connector=%s status=' \"$n\"; "
        "cat \"$d/status\" 2>/dev/null || printf 'unknown\\n'; "
        "printf 'modes[%s]=' \"$n\"; "
        "tr '\\n' ',' < \"$d/modes\" 2>/dev/null || true; "
        "printf '\\n'; "
        "done";
    int written = snprintf(out, out_size, "%s", command);
    return checked_snprintf(out, out_size, written);
}

si_host_display_result_t si_host_display_build_apply_command(
    const char *plan_id, const si_host_display_request_t *request,
    char *out, size_t out_size)
{
    if (!safe_identifier(plan_id, SI_HOST_DISPLAY_PLAN_ID_MAX) ||
        si_host_display_validate_request(request) != SI_HOST_DISPLAY_OK) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_INVALID_ARGUMENT;
    }
    if (!request->persistent) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_UNSUPPORTED;
    }

    char mode[SI_HOST_DISPLAY_MODE_MAX + 1U];
    si_host_display_result_t mode_result =
        si_host_display_format_mode(request, mode, sizeof(mode));
    if (mode_result != SI_HOST_DISPLAY_OK) {
        return mode_result;
    }
    int written = snprintf(
        out, out_size,
        "set -eu; "
        "cfg=/etc/default/grub.d/99-exoanchor-display.cfg; "
        "bak=/var/lib/exoanchor/display/%s; "
        "sudo install -d -m 0755 /etc/default/grub.d \"$bak\"; "
        "if sudo test -f \"$cfg\"; then "
        "sudo cp -a \"$cfg\" \"$bak/previous.cfg\"; "
        "printf 'yes\\n' | sudo tee \"$bak/previous_present\" >/dev/null; "
        "else "
        "printf 'no\\n' | sudo tee \"$bak/previous_present\" >/dev/null; "
        "fi; "
        "printf '%%s\\n' "
        "'# Managed by ExoAnchor Agent plan %s' "
        "'GRUB_CMDLINE_LINUX_DEFAULT=\"${GRUB_CMDLINE_LINUX_DEFAULT} video=%se\"' "
        "| sudo tee \"$cfg\" >/dev/null; "
        "sudo update-grub; "
        "printf 'plan_id=%%s\\nbackup=%%s\\nmode=%%s\\nreboot_required=yes\\n' "
        "'%s' \"$bak\" '%s'",
        plan_id, plan_id, mode, plan_id, mode);
    return checked_snprintf(out, out_size, written);
}

si_host_display_result_t si_host_display_build_verify_command(
    const char *plan_id, const si_host_display_request_t *request,
    char *out, size_t out_size)
{
    if (!safe_identifier(plan_id, SI_HOST_DISPLAY_PLAN_ID_MAX) ||
        si_host_display_validate_request(request) != SI_HOST_DISPLAY_OK) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_INVALID_ARGUMENT;
    }
    char mode[SI_HOST_DISPLAY_MODE_MAX + 1U];
    si_host_display_result_t mode_result =
        si_host_display_format_mode(request, mode, sizeof(mode));
    if (mode_result != SI_HOST_DISPLAY_OK) {
        return mode_result;
    }
    int written = snprintf(
        out, out_size,
        "set -eu; "
        "cfg=/etc/default/grub.d/99-exoanchor-display.cfg; "
        "printf 'schema=exoanchor.host_display.verify.v1\\nplan_id=%s\\nexpected=%s\\n'; "
        "printf 'config_present='; test -f \"$cfg\" && printf 'yes\\n' || printf 'no\\n'; "
        "printf 'config_token='; "
        "grep -F 'video=%se' \"$cfg\" >/dev/null 2>&1 && printf 'yes\\n' || printf 'no\\n'; "
        "printf 'runtime_token='; "
        "grep -F 'video=%se' /proc/cmdline >/dev/null 2>&1 && printf 'yes\\n' || printf 'no\\n'; "
        "printf 'cmdline='; cat /proc/cmdline; "
        "printf 'fb_virtual_size='; "
        "cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || printf 'unavailable\\n'; "
        "for d in /sys/class/drm/card*-*; do "
        "test -d \"$d\" || continue; n=${d##*/}; "
        "printf 'connector=%%s status=' \"$n\"; "
        "cat \"$d/status\" 2>/dev/null || printf 'unknown\\n'; "
        "done",
        plan_id, mode, mode, mode);
    return checked_snprintf(out, out_size, written);
}

si_host_display_result_t si_host_display_build_rollback_command(
    const char *plan_id, char *out, size_t out_size)
{
    if (!safe_identifier(plan_id, SI_HOST_DISPLAY_PLAN_ID_MAX)) {
        if (out && out_size > 0U) {
            out[0] = '\0';
        }
        return SI_HOST_DISPLAY_INVALID_ARGUMENT;
    }
    int written = snprintf(
        out, out_size,
        "set -eu; "
        "cfg=/etc/default/grub.d/99-exoanchor-display.cfg; "
        "bak=/var/lib/exoanchor/display/%s; "
        "test -f \"$bak/previous_present\"; "
        "if grep -qx yes \"$bak/previous_present\"; then "
        "sudo test -f \"$bak/previous.cfg\"; "
        "sudo cp -a \"$bak/previous.cfg\" \"$cfg\"; "
        "else sudo rm -f \"$cfg\"; fi; "
        "sudo update-grub; "
        "printf 'plan_id=%s\\nrollback=restored\\nreboot_required=yes\\n'",
        plan_id, plan_id);
    return checked_snprintf(out, out_size, written);
}

const char *si_host_display_result_name(si_host_display_result_t result)
{
    switch (result) {
    case SI_HOST_DISPLAY_OK:
        return "ok";
    case SI_HOST_DISPLAY_INVALID_ARGUMENT:
        return "invalid_argument";
    case SI_HOST_DISPLAY_UNSUPPORTED:
        return "unsupported";
    case SI_HOST_DISPLAY_OUTPUT_TOO_SMALL:
        return "output_too_small";
    default:
        return "unknown";
    }
}

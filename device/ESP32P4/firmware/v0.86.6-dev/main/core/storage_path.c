#include "storage_path.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SI_STORAGE_PATH_SEGMENT_MAX 192U

static bool equals_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *canonical_top_dir(const char *segment)
{
    static const char *const known[] = {
        "AGENT", "ASSETS", "EXPORTS", "LOGS", "MCP", "OTA", "SNAPSHOTS",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (equals_ci(segment, known[i])) {
            return known[i];
        }
    }
    return segment;
}

static si_storage_path_result_t append_segment(char *out, size_t out_size,
                                               const char *segment)
{
    size_t current = strlen(out);
    size_t segment_len = strlen(segment);
    size_t separator_len = current > 0 ? 1U : 0U;
    if (current + separator_len + segment_len >= out_size) {
        return SI_STORAGE_PATH_TOO_LONG;
    }
    if (separator_len) {
        out[current++] = '/';
    }
    memcpy(out + current, segment, segment_len + 1U);
    return SI_STORAGE_PATH_OK;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

si_storage_path_result_t si_storage_url_decode_inplace(char *value)
{
    if (!value) {
        return SI_STORAGE_PATH_INVALID_ARGUMENT;
    }
    char *src = value;
    char *dst = value;
    while (*src) {
        if (*src == '%') {
            if (!src[1] || !src[2]) {
                return SI_STORAGE_PATH_INVALID_ARGUMENT;
            }
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi < 0 || lo < 0) {
                return SI_STORAGE_PATH_INVALID_ARGUMENT;
            }
            int decoded = (hi << 4) | lo;
            if (decoded == 0) {
                return SI_STORAGE_PATH_INVALID_ARGUMENT;
            }
            *dst++ = (char)decoded;
            src += 3;
        } else {
            *dst++ = (*src == '+') ? ' ' : *src;
            src++;
        }
    }
    *dst = '\0';
    return SI_STORAGE_PATH_OK;
}

static si_storage_path_result_t canonicalize_relative_path(const char *input,
                                                           char *out,
                                                           size_t out_size)
{
    if (!out || out_size == 0) {
        return SI_STORAGE_PATH_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (!input || !input[0]) {
        return SI_STORAGE_PATH_OK;
    }

    const char *cursor = input;
    size_t index = 0;
    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            cursor++;
        }
        size_t len = (size_t)(cursor - start);
        if (len == 0) {
            continue;
        }
        if (len >= SI_STORAGE_PATH_SEGMENT_MAX) {
            return SI_STORAGE_PATH_TOO_LONG;
        }

        char segment[SI_STORAGE_PATH_SEGMENT_MAX];
        memcpy(segment, start, len);
        segment[len] = '\0';

        const char *canonical = segment;
        if (index == 0) {
            canonical = canonical_top_dir(segment);
        }

        si_storage_path_result_t ret = append_segment(out, out_size, canonical);
        if (ret != SI_STORAGE_PATH_OK) {
            return ret;
        }
        index++;
    }
    return SI_STORAGE_PATH_OK;
}

si_storage_path_result_t si_storage_resolve_path(const char *input,
                                                 const char *storage_root,
                                                 char *relative_path,
                                                 size_t relative_path_size,
                                                 char *absolute_path,
                                                 size_t absolute_path_size)
{
    if (!storage_root || !relative_path || !absolute_path ||
        relative_path_size == 0 || absolute_path_size == 0) {
        return SI_STORAGE_PATH_INVALID_ARGUMENT;
    }
    relative_path[0] = '\0';
    absolute_path[0] = '\0';

    const char *path = input && input[0] ? input : "";
    while (*path == '/') {
        path++;
    }
    if (strncmp(path, "sdcard/", 7) == 0) {
        path += 7;
    }
    if (strncmp(path, "EA", 2) == 0 && (path[2] == '\0' || path[2] == '/')) {
        path += 2;
        if (*path == '/') {
            path++;
        }
    }

    if (strlen(path) >= relative_path_size || strchr(path, '\\') ||
        strchr(path, ':') || strstr(path, "..")) {
        return SI_STORAGE_PATH_INVALID_ARGUMENT;
    }

    si_storage_path_result_t ret =
        canonicalize_relative_path(path, relative_path, relative_path_size);
    if (ret != SI_STORAGE_PATH_OK) {
        return ret;
    }

    int written = relative_path[0] ?
        snprintf(absolute_path, absolute_path_size, "%s/%s", storage_root, relative_path) :
        snprintf(absolute_path, absolute_path_size, "%s", storage_root);
    if (written < 0 || (size_t)written >= absolute_path_size) {
        absolute_path[0] = '\0';
        return SI_STORAGE_PATH_TOO_LONG;
    }
    return SI_STORAGE_PATH_OK;
}

bool si_storage_path_is_file_target(const char *relative_path)
{
    return relative_path && relative_path[0] != '\0';
}

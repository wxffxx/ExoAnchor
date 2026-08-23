#include "video_mjpeg.h"

#include <string.h>

static bool marker_has_length(uint8_t marker)
{
    return marker != 0x01 && marker != 0xd8 && marker != 0xd9 &&
           !(marker >= 0xd0 && marker <= 0xd7);
}

static bool marker_is_sof(uint8_t marker)
{
    return (marker >= 0xc0 && marker <= 0xc3) ||
           (marker >= 0xc5 && marker <= 0xc7) ||
           (marker >= 0xc9 && marker <= 0xcb) ||
           (marker >= 0xcd && marker <= 0xcf);
}

static bool fail(si_mjpeg_validation_t *result, size_t offset,
                 uint8_t marker)
{
    if (result) {
        result->invalid_offset = offset;
        result->invalid_marker = marker;
    }
    return false;
}

bool si_mjpeg_validate(const uint8_t *data, size_t len,
                       uint32_t expected_width, uint32_t expected_height,
                       si_mjpeg_validation_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        result->invalid_offset = SIZE_MAX;
    }
    if (!data || len < 16 || data[0] != 0xff || data[1] != 0xd8 ||
        data[len - 2] != 0xff || data[len - 1] != 0xd9) {
        return fail(result, 0, 0);
    }

    bool have_sof = false;
    bool have_sos = false;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t pos = 2;
    while (pos + 1 < len) {
        if (data[pos] != 0xff) {
            return fail(result, pos, data[pos]);
        }
        size_t marker_pos = pos;
        while (pos < len && data[pos] == 0xff) {
            pos++;
        }
        if (pos >= len) {
            return fail(result, marker_pos, 0xff);
        }
        uint8_t marker = data[pos++];
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 ||
            !marker_has_length(marker) || pos + 1 >= len) {
            return fail(result, marker_pos, marker);
        }
        uint16_t segment_len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        if (segment_len < 2 || pos + segment_len > len - 2U) {
            return fail(result, marker_pos, marker);
        }
        if (marker_is_sof(marker)) {
            if (segment_len < 7 || have_sof) {
                return fail(result, marker_pos, marker);
            }
            height = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            width = ((uint16_t)data[pos + 5] << 8) | data[pos + 6];
            have_sof = width > 0 && height > 0;
            if (result) {
                result->sof_marker = marker;
            }
        }
        if (marker == 0xda) {
            have_sos = true;
            pos += segment_len;
            if (result) {
                result->scan_offset = pos;
            }
            break;
        }
        pos += segment_len;
    }

    if (!have_sof || !have_sos ||
        (expected_width > 0 && width != expected_width) ||
        (expected_height > 0 && height != expected_height)) {
        return fail(result, pos, 0);
    }

    /* Entropy is intentionally opaque on the capture hot path. The UVC layer
     * rejects transport/header errors, while the downstream JPEG decoder
     * validates entropy before H.264 conversion (and the browser decodes the
     * MJPEG path itself). Walking a maximum-size frame here would read all of
     * PSRAM twice before returning the bounded UVC buffer and causes regular
     * isochronous underflow. Never repair or synthesize JPEG data here. */
    if (result) {
        result->width = width;
        result->height = height;
    }
    return true;
}

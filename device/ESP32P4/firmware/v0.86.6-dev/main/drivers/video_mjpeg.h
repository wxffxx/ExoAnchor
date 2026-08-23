#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t scan_offset;
    uint8_t sof_marker;
    size_t invalid_offset;
    uint8_t invalid_marker;
} si_mjpeg_validation_t;

/* Validate the bounded JPEG header emitted by the capture device: exact SOI,
 * well-formed segments, SOF dimensions, SOS, and an exact terminal EOI.  The
 * entropy payload is decoder-owned; this layer never scans, repairs, trims, or
 * synthesizes it. */
bool si_mjpeg_validate(const uint8_t *data, size_t len,
                       uint32_t expected_width, uint32_t expected_height,
                       si_mjpeg_validation_t *result);

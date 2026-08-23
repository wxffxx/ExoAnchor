#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "video_mjpeg.h"

static const uint8_t k_valid_jpeg[] = {
    0xff, 0xd8,
    0xff, 0xe0, 0x00, 0x04, 0x12, 0x34,
    0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x02, 0x00, 0x03,
    0x01, 0x01, 0x11, 0x00,
    0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00,
    0x11, 0x22, 0xff, 0x00, 0x33, 0xff, 0xd0, 0x44,
    0xff, 0xd9,
};

static void expect_valid(const uint8_t *jpeg, size_t len,
                         uint32_t width, uint32_t height)
{
    si_mjpeg_validation_t result;
    memset(&result, 0xa5, sizeof(result));
    assert(si_mjpeg_validate(jpeg, len, width, height, &result));
    assert(result.width == 3);
    assert(result.height == 2);
    assert(result.sof_marker == 0xc0);
    assert(result.scan_offset == 31);
    assert(result.invalid_offset == SIZE_MAX);
    assert(result.invalid_marker == 0);
}

static void expect_invalid(const uint8_t *jpeg, size_t len)
{
    si_mjpeg_validation_t result;
    memset(&result, 0xa5, sizeof(result));
    assert(!si_mjpeg_validate(jpeg, len, 3, 2, &result));
    assert(result.invalid_offset != SIZE_MAX);
}

static void test_valid_baseline_frame(void)
{
    expect_valid(k_valid_jpeg, sizeof(k_valid_jpeg), 3, 2);
    expect_valid(k_valid_jpeg, sizeof(k_valid_jpeg), 0, 0);
    assert(si_mjpeg_validate(k_valid_jpeg, sizeof(k_valid_jpeg), 3, 2,
                             NULL));
}

static void test_outer_markers_and_dimensions(void)
{
    uint8_t jpeg[sizeof(k_valid_jpeg)];

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[0] = 0;
    expect_invalid(jpeg, sizeof(jpeg));

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[sizeof(jpeg) - 1] = 0;
    expect_invalid(jpeg, sizeof(jpeg));

    assert(!si_mjpeg_validate(NULL, sizeof(jpeg), 3, 2, NULL));
    assert(!si_mjpeg_validate(jpeg, 8, 3, 2, NULL));
    assert(!si_mjpeg_validate(k_valid_jpeg, sizeof(k_valid_jpeg), 4, 2,
                              NULL));
    assert(!si_mjpeg_validate(k_valid_jpeg, sizeof(k_valid_jpeg), 3, 4,
                              NULL));
}

static void test_header_segments_are_bounded(void)
{
    uint8_t jpeg[sizeof(k_valid_jpeg)];

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[4] = 0;
    jpeg[5] = 1;
    expect_invalid(jpeg, sizeof(jpeg));

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[4] = 0x7f;
    jpeg[5] = 0xff;
    expect_invalid(jpeg, sizeof(jpeg));

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[8] = 0x12;
    expect_invalid(jpeg, sizeof(jpeg));

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[9] = 0xda;
    expect_invalid(jpeg, sizeof(jpeg));
}

static void test_sos_segment_cannot_cover_terminal_eoi(void)
{
    uint8_t jpeg[sizeof(k_valid_jpeg)];
    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));

    /* SOS starts at offset 21 and its length field starts at 23.  Extending
     * that segment through the first byte of the terminal EOI must not be
     * mistaken for a valid bounded header followed by opaque entropy. */
    jpeg[23] = 0x00;
    jpeg[24] = 0x11;
    expect_invalid(jpeg, sizeof(jpeg));
}

static void test_entropy_is_decoder_owned(void)
{
    uint8_t jpeg[sizeof(k_valid_jpeg)];
    const size_t entropy_marker = 33;

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[entropy_marker] = 0xff;
    jpeg[entropy_marker + 1] = 0xc4;
    expect_valid(jpeg, sizeof(jpeg), 3, 2);

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[entropy_marker] = 0xff;
    jpeg[entropy_marker + 1] = 0xd8;
    expect_valid(jpeg, sizeof(jpeg), 3, 2);

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[entropy_marker] = 0xff;
    jpeg[entropy_marker + 1] = 0xd9;
    expect_valid(jpeg, sizeof(jpeg), 3, 2);

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[entropy_marker] = 0xff;
    jpeg[entropy_marker + 1] = 0xd7;
    expect_valid(jpeg, sizeof(jpeg), 3, 2);

    memcpy(jpeg, k_valid_jpeg, sizeof(jpeg));
    jpeg[entropy_marker] = 0xff;
    jpeg[entropy_marker + 1] = 0x00;
    expect_valid(jpeg, sizeof(jpeg), 3, 2);
}

int main(void)
{
    test_valid_baseline_frame();
    test_outer_markers_and_dimensions();
    test_header_segments_are_bounded();
    test_sos_segment_cannot_cover_terminal_eoi();
    test_entropy_is_decoder_owned();
    puts("video MJPEG validator tests: PASS");
    return 0;
}

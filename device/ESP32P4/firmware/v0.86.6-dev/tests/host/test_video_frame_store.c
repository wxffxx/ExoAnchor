#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "video_frame_store.h"

static void fill(uint8_t *data, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(seed + i * 17U);
    }
}

static void assert_view(const si_video_frame_store_view_t *view,
                        const uint8_t *expected, size_t len,
                        uint32_t frame_id)
{
    assert(view->data != NULL);
    assert(view->token != NULL);
    assert(view->len == len);
    assert(view->frame_id == frame_id);
    assert(memcmp(view->data, expected, len) == 0);
}

static void test_latest_and_refcount_no_overwrite(void)
{
    uint8_t a[32], b[32], c[32], d[32];
    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 2);
    fill(c, sizeof(c), 3);
    fill(d, sizeof(d), 4);

    uint32_t a_id = 0;
    assert(si_video_frame_store_publish(a, sizeof(a), &a_id) == ESP_OK);
    assert(a_id != 0);

    si_video_frame_store_view_t a_view = {0};
    assert(si_video_frame_store_acquire(0, &a_view) == ESP_OK);
    assert_view(&a_view, a, sizeof(a), a_id);
    si_video_frame_store_view_t same = {0};
    assert(si_video_frame_store_acquire(a_id, &same) ==
           ESP_ERR_NOT_FINISHED);
    assert(same.data == NULL && same.token == NULL);

    uint32_t b_id = 0;
    assert(si_video_frame_store_publish(b, sizeof(b), &b_id) == ESP_OK);
    assert(b_id > a_id);
    assert_view(&a_view, a, sizeof(a), a_id);

    si_video_frame_store_view_t b_view = {0};
    assert(si_video_frame_store_acquire(a_id, &b_view) == ESP_OK);
    assert_view(&b_view, b, sizeof(b), b_id);

    assert(si_video_frame_store_publish(c, sizeof(c), NULL) ==
           ESP_ERR_NOT_FINISHED);
    assert_view(&a_view, a, sizeof(a), a_id);
    assert_view(&b_view, b, sizeof(b), b_id);

    si_video_frame_store_release(&b_view);
    assert(b_view.data == NULL && b_view.token == NULL && b_view.len == 0);

    uint32_t c_id = 0;
    assert(si_video_frame_store_publish(c, sizeof(c), &c_id) == ESP_OK);
    assert(c_id > b_id);
    assert_view(&a_view, a, sizeof(a), a_id);

    si_video_frame_store_view_t c_view_1 = {0};
    si_video_frame_store_view_t c_view_2 = {0};
    assert(si_video_frame_store_acquire(b_id, &c_view_1) == ESP_OK);
    assert(si_video_frame_store_acquire(UINT32_MAX, &c_view_2) == ESP_OK);
    assert_view(&c_view_1, c, sizeof(c), c_id);
    assert_view(&c_view_2, c, sizeof(c), c_id);

    si_video_frame_store_release(&a_view);
    uint32_t d_id = 0;
    assert(si_video_frame_store_publish(d, sizeof(d), &d_id) == ESP_OK);
    assert(d_id > c_id);
    assert_view(&c_view_1, c, sizeof(c), c_id);
    assert_view(&c_view_2, c, sizeof(c), c_id);

    si_video_frame_store_view_t latest = {0};
    assert(si_video_frame_store_acquire(c_id, &latest) == ESP_OK);
    assert_view(&latest, d, sizeof(d), d_id);
    si_video_frame_store_release(&latest);
    si_video_frame_store_release(&c_view_1);
    si_video_frame_store_release(&c_view_2);
}

static void test_copy_and_clear_generation(void)
{
    uint8_t frame[48];
    fill(frame, sizeof(frame), 0x40);
    uint32_t frame_id = 0;
    assert(si_video_frame_store_publish(frame, sizeof(frame), &frame_id) ==
           ESP_OK);

    uint8_t copy[64] = {0};
    size_t copied = 0;
    uint32_t copied_id = 0;
    assert(si_video_frame_store_copy(0, false, copy, sizeof(copy), &copied,
                                     &copied_id) == ESP_OK);
    assert(copied == sizeof(frame));
    assert(copied_id == frame_id);
    assert(memcmp(copy, frame, sizeof(frame)) == 0);
    assert(si_video_frame_store_copy(frame_id, true, copy, sizeof(copy),
                                     &copied, NULL) == ESP_ERR_NOT_FINISHED);

    copied = 0;
    assert(si_video_frame_store_copy(0, false, copy, 8, &copied, NULL) ==
           ESP_ERR_INVALID_SIZE);
    assert(copied == sizeof(frame));

    si_video_frame_store_view_t held = {0};
    assert(si_video_frame_store_acquire(0, &held) == ESP_OK);
    assert_view(&held, frame, sizeof(frame), frame_id);
    si_video_frame_store_clear();
    assert(si_video_frame_store_acquire(0, &(si_video_frame_store_view_t){0}) ==
           ESP_ERR_NOT_FOUND);
    assert_view(&held, frame, sizeof(frame), frame_id);

    uint8_t replacement[16];
    fill(replacement, sizeof(replacement), 0x90);
    uint32_t replacement_id = 0;
    assert(si_video_frame_store_publish(replacement, sizeof(replacement),
                                        &replacement_id) == ESP_OK);
    assert(replacement_id > frame_id);
    assert_view(&held, frame, sizeof(frame), frame_id);
    si_video_frame_store_release(&held);
}

static void test_arguments_and_capacity(void)
{
    uint8_t byte = 1;
    assert(si_video_frame_store_publish(NULL, 1, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(si_video_frame_store_publish(&byte, 0, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(si_video_frame_store_publish(&byte, 65, NULL) ==
           ESP_ERR_INVALID_SIZE);
    assert(si_video_frame_store_acquire(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(si_video_frame_store_copy(0, false, NULL, 0, &(size_t){0}, NULL) ==
           ESP_ERR_INVALID_ARG);
    assert(si_video_frame_store_copy(0, false, &byte, 1, NULL, NULL) ==
           ESP_ERR_INVALID_ARG);
    assert(si_video_frame_store_init(32) == ESP_OK);
    assert(si_video_frame_store_init(128) == ESP_ERR_INVALID_SIZE);
    assert(si_video_frame_store_capacity() == 64);
}

static void test_max_frame_and_variable_packing(void)
{
    uint8_t maximum[64];
    uint8_t small[16];
    uint8_t complement[48];
    fill(maximum, sizeof(maximum), 0x31);
    fill(small, sizeof(small), 0x62);
    fill(complement, sizeof(complement), 0x93);

    si_video_frame_store_clear();
    uint32_t maximum_id = 0;
    assert(si_video_frame_store_publish(maximum, sizeof(maximum),
                                        &maximum_id) ==
           ESP_OK);

    si_video_frame_store_view_t held_maximum = {0};
    assert(si_video_frame_store_acquire(0, &held_maximum) == ESP_OK);
    assert_view(&held_maximum, maximum, sizeof(maximum), maximum_id);

    /* A borrowed maximum frame occupies the complete arena.  Coalesce new
     * input until that immutable view is released; never overwrite it. */
    assert(si_video_frame_store_publish(small, sizeof(small),
                                        NULL) == ESP_ERR_NOT_FINISHED);
    assert_view(&held_maximum, maximum, sizeof(maximum), maximum_id);
    si_video_frame_store_release(&held_maximum);

    uint32_t small_id = 0;
    assert(si_video_frame_store_publish(small, sizeof(small), &small_id) ==
           ESP_OK);
    si_video_frame_store_view_t held_small = {0};
    assert(si_video_frame_store_acquire(0, &held_small) == ESP_OK);

    /* Front/back packing retains ordinary double buffering inside the same
     * maximum-frame arena when both immutable ranges fit. */
    uint32_t complement_id = 0;
    assert(si_video_frame_store_publish(complement, sizeof(complement),
                                        &complement_id) == ESP_OK);
    assert(complement_id > small_id);
    assert_view(&held_small, small, sizeof(small), small_id);
    si_video_frame_store_view_t held_complement = {0};
    assert(si_video_frame_store_acquire(small_id, &held_complement) == ESP_OK);
    assert_view(&held_complement, complement, sizeof(complement),
                complement_id);
    assert(si_video_frame_store_publish(small, 1, NULL) ==
           ESP_ERR_NOT_FINISHED);

    si_video_frame_store_release(&held_small);
    si_video_frame_store_release(&held_complement);
    uint32_t replacement_id = 0;
    assert(si_video_frame_store_publish(maximum, sizeof(maximum),
                                        &replacement_id) == ESP_OK);
    assert(replacement_id > complement_id);
    si_video_frame_store_view_t replacement = {0};
    assert(si_video_frame_store_acquire(complement_id, &replacement) ==
           ESP_OK);
    assert_view(&replacement, maximum, sizeof(maximum), replacement_id);
    si_video_frame_store_release(&replacement);
}

static void test_scratch_is_explicitly_unsupported(void)
{
    si_video_frame_store_scratch_t scratch = {
        .data = (uint8_t *)(uintptr_t)1,
        .capacity = 1,
        .token = 1,
    };
    assert(si_video_frame_store_scratch_acquire(10, &scratch) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(scratch.data == NULL && scratch.capacity == 0 && scratch.token == 0);
    assert(si_video_frame_store_scratch_release(&scratch) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(si_video_frame_store_scratch_acquire(0, NULL) ==
           ESP_ERR_INVALID_ARG);
    assert(si_video_frame_store_scratch_release(NULL) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    assert(si_video_frame_store_init(0) == ESP_ERR_INVALID_ARG);
    assert(si_video_frame_store_init(64) == ESP_OK);
    test_latest_and_refcount_no_overwrite();
    test_copy_and_clear_generation();
    test_arguments_and_capacity();
    test_max_frame_and_variable_packing();
    test_scratch_is_explicitly_unsupported();
    puts("video frame store tests: PASS");
    return 0;
}

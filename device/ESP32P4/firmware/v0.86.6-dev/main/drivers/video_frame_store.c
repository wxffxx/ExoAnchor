#include "video_frame_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SI_VIDEO_FRAME_STORE_SLOTS 2U

typedef struct {
    uint8_t *data;
    size_t offset;
    size_t len;
    uint32_t frame_id;
    uint32_t generation;
    uint32_t refs;
    bool valid;
    bool writing;
} si_video_frame_slot_t;

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t *arena;
    size_t capacity;
    uint32_t generation;
    uint32_t next_frame_id;
    int current;
    si_video_frame_slot_t slots[SI_VIDEO_FRAME_STORE_SLOTS];
} si_video_frame_store_t;

static si_video_frame_store_t s_store = {
    .current = -1,
};

esp_err_t si_video_frame_store_init(size_t capacity)
{
    if (capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_store.arena) {
        return s_store.capacity >= capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    s_store.lock = xSemaphoreCreateMutex();
    if (!s_store.lock) {
        return ESP_ERR_NO_MEM;
    }
    /*
     * `capacity` is both the largest accepted frame and the complete PSRAM
     * budget for this store.  Two metadata slots share that one arena: slot 0
     * grows from the front and slot 1 grows from the back.  Normal compressed
     * frames therefore retain double-buffering when their combined length
     * fits, while a single maximum-size frame can use the entire arena.
     *
     * Allocating two maximum-size slots here would duplicate the three full
     * UVC driver buffers and leave no PSRAM for the optional H.264 pipeline.
     */
    s_store.arena = heap_caps_aligned_alloc(
        128, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_store.arena) {
        vSemaphoreDelete(s_store.lock);
        memset(&s_store, 0, sizeof(s_store));
        s_store.current = -1;
        return ESP_ERR_NO_MEM;
    }
    s_store.capacity = capacity;
    s_store.generation = 1;
    return ESP_OK;
}

void si_video_frame_store_clear(void)
{
    if (!s_store.lock ||
        xSemaphoreTake(s_store.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (++s_store.generation == 0) {
        s_store.generation = 1;
    }
    s_store.current = -1;
    for (size_t i = 0; i < SI_VIDEO_FRAME_STORE_SLOTS; i++) {
        si_video_frame_slot_t *slot = &s_store.slots[i];
        if (slot->refs == 0 && !slot->writing) {
            slot->valid = false;
            slot->len = 0;
        }
    }
    xSemaphoreGive(s_store.lock);
}

static bool slot_occupies_arena_locked(size_t index)
{
    const si_video_frame_slot_t *slot = &s_store.slots[index];
    const bool visible = (int)index == s_store.current && slot->valid &&
                         slot->generation == s_store.generation;
    return slot->writing || slot->refs > 0 || visible;
}

static bool layout_slot_locked(size_t target, size_t len, size_t *offset)
{
    if (!offset || target >= SI_VIDEO_FRAME_STORE_SLOTS ||
        len > s_store.capacity) {
        return false;
    }

    const size_t other_index = 1U - target;
    const si_video_frame_slot_t *other = &s_store.slots[other_index];
    const bool other_occupied = slot_occupies_arena_locked(other_index);

    if (target == 0U) {
        *offset = 0U;
        return !other_occupied || len <= other->offset;
    }

    *offset = s_store.capacity - len;
    return !other_occupied ||
           *offset >= other->offset + other->len;
}

static int select_write_slot_locked(size_t len, size_t *offset)
{
    if (s_store.slots[0].writing || s_store.slots[1].writing) {
        return -1;
    }

    /* Prefer the non-current slot so acquire() can keep exposing the previous
     * immutable frame while the next copy runs without the metadata lock. */
    const int first = s_store.current >= 0 ? 1 - s_store.current : 0;
    const int second = s_store.current >= 0 ? s_store.current : 1;
    const int candidates[SI_VIDEO_FRAME_STORE_SLOTS] = {first, second};
    for (size_t i = 0; i < SI_VIDEO_FRAME_STORE_SLOTS; i++) {
        const int candidate = candidates[i];
        si_video_frame_slot_t *slot = &s_store.slots[candidate];
        size_t candidate_offset = 0U;
        if (slot->refs != 0 || slot->writing ||
            !layout_slot_locked((size_t)candidate, len,
                                &candidate_offset)) {
            continue;
        }
        *offset = candidate_offset;
        return candidate;
    }
    return -1;
}

esp_err_t si_video_frame_store_publish(const uint8_t *data, size_t len,
                                       uint32_t *frame_id)
{
    if (!data || len == 0 || !s_store.lock || !s_store.arena) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > s_store.capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(s_store.lock, 0) != pdTRUE) {
        return ESP_ERR_NOT_FINISHED;
    }
    size_t target_offset = 0U;
    const int target = select_write_slot_locked(len, &target_offset);
    if (target < 0) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NOT_FINISHED;
    }
    si_video_frame_slot_t *slot = &s_store.slots[target];
    if (target == s_store.current) {
        /* acquire() must not see a current slot while its bytes are being
         * overwritten with the lock released. */
        s_store.current = -1;
    }
    slot->valid = false;
    slot->writing = true;
    slot->offset = target_offset;
    slot->data = s_store.arena + target_offset;
    slot->len = len;
    const uint32_t generation = s_store.generation;
    xSemaphoreGive(s_store.lock);

    memmove(slot->data, data, len);

    /* The writer owns this slot until publication completes.  Reacquiring
     * the metadata lock must not time out and mutate `writing` without the
     * lock: that would allow a second writer to overwrite an in-flight copy. */
    if (xSemaphoreTake(s_store.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (generation != s_store.generation) {
        slot->writing = false;
        slot->valid = false;
        slot->len = 0;
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int previous_current = s_store.current;
    if (previous_current >= 0 && previous_current != target) {
        si_video_frame_slot_t *previous =
            &s_store.slots[previous_current];
        if (previous->refs == 0) {
            previous->valid = false;
            previous->len = 0;
        }
    }
    if (++s_store.next_frame_id == 0) {
        s_store.next_frame_id = 1;
    }
    slot->frame_id = s_store.next_frame_id;
    slot->generation = generation;
    slot->valid = true;
    slot->writing = false;
    s_store.current = target;
    if (frame_id) {
        *frame_id = slot->frame_id;
    }
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

esp_err_t si_video_frame_store_acquire(uint32_t last_frame_id,
                                       si_video_frame_store_view_t *view)
{
    if (!view) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(view, 0, sizeof(*view));
    if (!s_store.lock ||
        xSemaphoreTake(s_store.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_store.current < 0) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NOT_FOUND;
    }
    si_video_frame_slot_t *slot = &s_store.slots[s_store.current];
    if (!slot->valid || slot->writing ||
        slot->generation != s_store.generation || slot->len == 0) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (last_frame_id != UINT32_MAX && last_frame_id == slot->frame_id) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NOT_FINISHED;
    }
    slot->refs++;
    view->data = slot->data;
    view->len = slot->len;
    view->frame_id = slot->frame_id;
    view->token = slot;
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

void si_video_frame_store_release(si_video_frame_store_view_t *view)
{
    if (!view || !view->token || !s_store.lock) {
        return;
    }
    if (xSemaphoreTake(s_store.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    si_video_frame_slot_t *slot = (si_video_frame_slot_t *)view->token;
    if (slot >= &s_store.slots[0] &&
        slot < &s_store.slots[SI_VIDEO_FRAME_STORE_SLOTS] &&
        slot->refs > 0) {
        slot->refs--;
        const size_t index = (size_t)(slot - &s_store.slots[0]);
        if (slot->refs == 0 &&
            ((int)index != s_store.current ||
             slot->generation != s_store.generation || !slot->valid)) {
            slot->valid = false;
            slot->len = 0;
        }
    }
    xSemaphoreGive(s_store.lock);
    memset(view, 0, sizeof(*view));
}

esp_err_t si_video_frame_store_copy(uint32_t last_frame_id, bool require_new,
                                    uint8_t *buffer, size_t capacity,
                                    size_t *out_len, uint32_t *out_frame_id)
{
    if (!buffer || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    si_video_frame_store_view_t view = {0};
    esp_err_t ret = si_video_frame_store_acquire(
        require_new ? last_frame_id : UINT32_MAX, &view);
    if (ret != ESP_OK) {
        return ret;
    }
    if (view.len > capacity) {
        *out_len = view.len;
        si_video_frame_store_release(&view);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, view.data, view.len);
    *out_len = view.len;
    if (out_frame_id) {
        *out_frame_id = view.frame_id;
    }
    si_video_frame_store_release(&view);
    return ESP_OK;
}

esp_err_t si_video_frame_store_scratch_acquire(
    uint32_t timeout_ms, si_video_frame_store_scratch_t *scratch)
{
    (void)timeout_ms;
    if (!scratch) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(scratch, 0, sizeof(*scratch));
    /* A variable-packed arena cannot hand out an independent full-capacity
     * workspace without either overlapping a borrowed frame or allocating a
     * second maximum-size buffer.  No production caller uses this legacy API;
     * reject it explicitly instead of exposing a false ownership promise. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t si_video_frame_store_scratch_release(
    si_video_frame_store_scratch_t *scratch)
{
    if (!scratch) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

size_t si_video_frame_store_capacity(void)
{
    return s_store.capacity;
}

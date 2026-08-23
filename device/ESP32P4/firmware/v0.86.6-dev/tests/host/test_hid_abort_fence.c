#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hid_abort_fence.h"

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    si_hid_abort_fence_t fence;
    uint32_t command_generation;
    bool command_captured;
    bool revoke_complete;
    bool stale_report_restored;
} fence_fixture_t;

static void *delayed_composite_command(void *opaque)
{
    fence_fixture_t *fixture = opaque;
    pthread_mutex_lock(&fixture->lock);
    fixture->command_generation =
        si_hid_abort_fence_snapshot(&fixture->fence);
    fixture->command_captured = true;
    pthread_cond_broadcast(&fixture->condition);
    while (!fixture->revoke_complete) {
        pthread_cond_wait(&fixture->condition, &fixture->lock);
    }
    pthread_mutex_unlock(&fixture->lock);

    /* Models the report restoration after click/combo delay. */
    fixture->stale_report_restored =
        si_hid_abort_fence_is_current(
            &fixture->fence, fixture->command_generation);
    return NULL;
}

static void *concurrent_revoke(void *opaque)
{
    fence_fixture_t *fixture = opaque;
    pthread_mutex_lock(&fixture->lock);
    while (!fixture->command_captured) {
        pthread_cond_wait(&fixture->condition, &fixture->lock);
    }
    (void)si_hid_abort_fence_advance(&fixture->fence);
    fixture->revoke_complete = true;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->lock);
    return NULL;
}

int main(void)
{
    fence_fixture_t fixture = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
        .fence = SI_HID_ABORT_FENCE_INITIALIZER,
    };
    pthread_t command_thread;
    pthread_t revoke_thread;

    assert(pthread_create(&command_thread, NULL,
                          delayed_composite_command, &fixture) == 0);
    assert(pthread_create(&revoke_thread, NULL,
                          concurrent_revoke, &fixture) == 0);
    assert(pthread_join(command_thread, NULL) == 0);
    assert(pthread_join(revoke_thread, NULL) == 0);
    assert(!fixture.stale_report_restored);

    uint32_t current = si_hid_abort_fence_snapshot(&fixture.fence);
    assert(current != 0U);
    assert(si_hid_abort_fence_is_current(&fixture.fence, current));
    assert(!si_hid_abort_fence_is_current(&fixture.fence, 0U));

    pthread_cond_destroy(&fixture.condition);
    pthread_mutex_destroy(&fixture.lock);
    puts("hid abort fence concurrency tests: PASS");
    return 0;
}

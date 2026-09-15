#!/usr/bin/env python3
"""Execute production network transactions with fake persistence and ACD events.

Only the IDF/backend boundaries are stubbed. The stage/apply/commit/rollback/
reset, ACD callback and completion logic are extracted unchanged and compiled.
"""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "main/drivers/net_manager.c").read_text()
header = (ROOT / "main/drivers/net_manager.h").read_text()


def function(signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


enum_start = source.index("typedef enum {")
enum_end = source.index("} static_acd_result_t;", enum_start) + len("} static_acd_result_t;")
status_start = header.index("typedef struct {")
status_end = header.index("} si_net_status_t;", status_start) + len("} si_net_status_t;")
defines = "\n".join(re.findall(
    r"^#define NETWORK_(?:CONFIRM_TIMEOUT_SECONDS|STATIC_ACD_TIMEOUT_SECONDS) .*",
    source, re.MULTILINE,
))
production = "\n".join(function(signature) for signature in (
    "static void static_acd_callback(",
    "static void finish_static_acd(",
    "static void poll_static_acd(",
    "static esp_err_t rollback_pending_locked(",
    "esp_err_t si_net_stage_config(",
    "esp_err_t si_net_apply_staged(",
    "esp_err_t si_net_commit_pending(",
    "esp_err_t si_net_rollback_pending(",
    "esp_err_t si_net_reset_config(",
))
harness = r'''
static int s_operation_lock = 1;
static int lock_depth;
static bool s_pending_applied;
static int64_t s_pending_deadline_us;
static int64_t s_static_acd_deadline_us;
static volatile static_acd_result_t s_static_acd_result;
static si_network_config_t s_runtime_config;
static si_network_config_t s_factory_config;
static si_net_status_t s_status;
static si_network_settings_status_t persisted;
static int64_t now_us;
static int applies, confirms, rollbacks, stages;
static esp_err_t apply_result, confirm_result, rollback_result, reset_result;
static bool callback_during_clock;
static void (*before_lock)(void);
struct netif { int unused; };
typedef enum { ACD_IP_OK, ACD_DECLINE, ACD_RESTART_CLIENT, ACD_OTHER } acd_callback_enum_t;
static void static_acd_callback(struct netif *, acd_callback_enum_t);
#define portMAX_DELAY 0xffffffffU
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static void xSemaphoreTake(int semaphore, unsigned timeout)
{
    (void)timeout;
    assert(semaphore == 1 && lock_depth == 0);
    if (before_lock) {
        void (*hook)(void) = before_lock;
        before_lock = NULL;
        hook();
    }
    lock_depth++;
}
static void xSemaphoreGive(int semaphore)
{
    assert(semaphore == 1 && lock_depth == 1);
    lock_depth--;
}
static void status_lock(void) {}
static void status_unlock(void) {}
static size_t test_strlcpy(char *destination, const char *value, size_t capacity)
{
    size_t length = strlen(value);
    if (capacity) snprintf(destination, capacity, "%s", value);
    return length;
}
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
static int64_t esp_timer_get_time(void)
{
    if (callback_during_clock) {
        callback_during_clock = false;
        static_acd_callback(NULL, ACD_IP_OK);
    }
    return now_us;
}
static void set_last_error(esp_err_t result, const char *context)
{
    (void)result;
    (void)context;
}
static void update_config_status_locked(const si_network_config_t *config, const char *state)
{
    assert(lock_depth == 1);
    s_status.config_generation = config->generation;
    strlcpy(s_status.config_state, state, sizeof(s_status.config_state));
}
bool si_network_config_validate(const si_network_config_t *config, char *error, size_t length)
{
    (void)error;
    (void)length;
    return config != NULL;
}
esp_err_t si_network_settings_get(si_network_settings_status_t *out)
{
    assert(lock_depth == 1);
    *out = persisted;
    return ESP_OK;
}
esp_err_t si_network_settings_stage(const si_network_config_t *config)
{
    assert(lock_depth == 1);
    stages++;
    persisted.staged = *config;
    persisted.staged.generation = persisted.active.generation + 1U;
    persisted.have_staged = true;
    persisted.pending = false;
    return ESP_OK;
}
esp_err_t si_network_settings_mark_pending(void)
{
    assert(lock_depth == 1 && persisted.have_staged);
    persisted.pending = true;
    return ESP_OK;
}
esp_err_t si_network_settings_confirm(void)
{
    assert(lock_depth == 1);
    confirms++;
    if (confirm_result != ESP_OK) return confirm_result;
    assert(persisted.pending && persisted.have_staged);
    persisted.active = persisted.staged;
    persisted.pending = persisted.have_staged = false;
    return ESP_OK;
}
esp_err_t si_network_settings_rollback(void)
{
    assert(lock_depth == 1);
    rollbacks++;
    if (rollback_result != ESP_OK) return rollback_result;
    persisted.pending = persisted.have_staged = false;
    return ESP_OK;
}
esp_err_t si_network_settings_reset(const si_network_config_t *factory)
{
    assert(lock_depth == 1);
    if (reset_result != ESP_OK) return reset_result;
    persisted.active = *factory;
    persisted.pending = persisted.have_staged = false;
    return ESP_OK;
}
static esp_err_t rollback_settings_on_internal_stack(void)
{
    return si_network_settings_rollback();
}
static esp_err_t apply_config(const si_network_config_t *config)
{
    assert(lock_depth == 1);
    applies++;
    if (apply_result != ESP_OK && config->generation != persisted.active.generation)
        return apply_result;
    s_runtime_config = *config;
    return ESP_OK;
}
static void stop_static_acd(void)
{
    assert(lock_depth == 1);
    s_static_acd_result = STATIC_ACD_IDLE;
    s_static_acd_deadline_us = 0;
}
static esp_err_t start_static_acd(const si_network_config_t *config)
{
    assert(lock_depth == 1 && config->mode == SI_NETWORK_MODE_STATIC);
    assert(s_static_acd_result == STATIC_ACD_IDLE);
    s_static_acd_result = STATIC_ACD_WAITING;
    s_static_acd_deadline_us = now_us + (int64_t)NETWORK_STATIC_ACD_TIMEOUT_SECONDS * 1000000;
    return ESP_OK;
}
'''
tests = r'''
static si_network_config_t candidate;
static void reset_test(void)
{
    assert(lock_depth == 0);
    memset(&s_status, 0, sizeof(s_status));
    memset(&persisted, 0, sizeof(persisted));
    memset(&candidate, 0, sizeof(candidate));
    memset(&s_factory_config, 0, sizeof(s_factory_config));
    s_status.link_up = true;
    s_pending_applied = false;
    s_pending_deadline_us = s_static_acd_deadline_us = 0;
    s_static_acd_result = STATIC_ACD_IDLE;
    persisted.active.generation = 10;
    persisted.active.mode = SI_NETWORK_MODE_DHCP;
    s_runtime_config = persisted.active;
    candidate.mode = SI_NETWORK_MODE_STATIC;
    strlcpy(candidate.address, "192.0.2.21", sizeof(candidate.address));
    now_us = 1000000;
    applies = confirms = rollbacks = stages = 0;
    apply_result = confirm_result = rollback_result = reset_result = ESP_OK;
    callback_during_clock = false;
    before_lock = NULL;
}
static void begin_static(void)
{
    assert(si_net_stage_config(&candidate) == ESP_OK);
    assert(si_net_apply_staged() == ESP_OK);
    assert(persisted.pending && s_status.pending_confirmation);
    assert(!s_pending_applied && s_static_acd_result == STATIC_ACD_WAITING);
}
static void complete_static(void)
{
    static_acd_callback(NULL, ACD_IP_OK);
    poll_static_acd();
    assert(s_pending_applied && applies == 1);
}
static void replace_before_poll_lock(void)
{
    assert(si_net_rollback_pending() == ESP_OK);
    strlcpy(candidate.address, "192.0.2.22", sizeof(candidate.address));
    begin_static();
}
int main(void)
{
    reset_test();
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    begin_static();
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    /* Repeated apply must not cancel or extend the first transaction. */
    int64_t deadline = s_pending_deadline_us;
    assert(si_net_apply_staged() == ESP_ERR_INVALID_STATE);
    assert(rollbacks == 0 && s_pending_deadline_us == deadline);
    assert(si_net_stage_config(&candidate) == ESP_ERR_INVALID_STATE && stages == 1);
    static_acd_callback(NULL, ACD_IP_OK);
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    poll_static_acd();
    assert(s_pending_applied && applies == 1 && s_static_acd_result == STATIC_ACD_IDLE);
    confirm_result = ESP_FAIL;
    assert(si_net_commit_pending() == ESP_FAIL && s_pending_applied);
    confirm_result = ESP_OK;
    assert(si_net_commit_pending() == ESP_OK);
    assert(!s_pending_applied && !persisted.pending && persisted.active.generation == 11);
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 2);

    for (int event = ACD_DECLINE; event <= ACD_RESTART_CLIENT; event++) {
        reset_test();
        begin_static();
        static_acd_callback(NULL, (acd_callback_enum_t)event);
        assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
        poll_static_acd();
        assert(rollbacks == 1 && applies == 0 && !s_pending_applied);
        assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    }
    reset_test();
    begin_static();
    now_us = s_static_acd_deadline_us;
    poll_static_acd();
    assert(rollbacks == 1 && !s_pending_applied && applies == 0);
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    /* A timeout observation cannot overwrite a callback that won the CAS. */
    reset_test();
    begin_static();
    now_us = s_static_acd_deadline_us;
    callback_during_clock = true;
    poll_static_acd();
    assert(s_pending_applied && applies == 1 && rollbacks == 0);

    /* A replacement started while the manager waits for its mutex remains checking. */
    reset_test();
    begin_static();
    static_acd_callback(NULL, ACD_IP_OK);
    before_lock = replace_before_poll_lock;
    poll_static_acd();
    assert(s_static_acd_result == STATIC_ACD_WAITING && !s_pending_applied);
    assert(strcmp(persisted.staged.address, "192.0.2.22") == 0);
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);

    reset_test();
    begin_static();
    apply_result = ESP_FAIL;
    static_acd_callback(NULL, ACD_IP_OK);
    poll_static_acd();
    assert(!s_pending_applied && rollbacks == 1 && applies == 2);
    assert(s_runtime_config.generation == 10); /* Restore after a partial apply. */
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);

    reset_test();
    begin_static();
    rollback_result = ESP_FAIL;
    static_acd_callback(NULL, ACD_DECLINE);
    poll_static_acd();
    assert(persisted.pending && !s_pending_applied);
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    assert(si_net_stage_config(&candidate) == ESP_ERR_INVALID_STATE);

    reset_test();
    candidate.mode = SI_NETWORK_MODE_DHCP;
    assert(si_net_stage_config(&candidate) == ESP_OK && si_net_apply_staged() == ESP_OK);
    assert(s_pending_applied && applies == 1);
    assert(si_net_commit_pending() == ESP_OK);
    reset_test();
    s_status.link_up = false;
    assert(si_net_stage_config(&candidate) == ESP_OK && si_net_apply_staged() == ESP_OK);
    assert(s_pending_applied && applies == 1);
    assert(si_net_commit_pending() == ESP_OK);

    reset_test();
    begin_static();
    complete_static();
    s_runtime_config.generation--;
    assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);

    for (int operation = 0; operation < 2; operation++) {
        reset_test();
        begin_static();
        complete_static();
        rollback_result = reset_result = ESP_FAIL;
        assert((operation == 0 ? si_net_rollback_pending() : si_net_reset_config()) == ESP_FAIL);
        assert(!s_pending_applied && persisted.pending);
        assert(si_net_commit_pending() == ESP_ERR_INVALID_STATE && confirms == 0);
    }
    reset_test();
    begin_static();
    assert(si_net_rollback_pending() == ESP_OK);
    static_acd_callback(NULL, ACD_IP_OK);
    poll_static_acd();
    assert(s_static_acd_result == STATIC_ACD_IDLE && !s_pending_applied);
    assert(si_net_reset_config() == ESP_OK && !s_pending_applied);
    puts("network manager transaction runtime: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="exoanchor-network-manager-") as tmp:
    test_c = Path(tmp) / "test.c"
    test_bin = Path(tmp) / "test"
    test_c.write_text(
        '#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n#include "network_settings.h"\n'
        + source[enum_start:enum_end] + "\n" + header[status_start:status_end]
        + "\n" + defines + "\n" + harness + "\n" + production + tests
    )
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "tests/host/idf_stubs"),
        "-I" + str(ROOT / "main/application"), "-I" + str(ROOT / "main/core"),
        str(test_c), "-o", str(test_bin),
    ], check=True)
    subprocess.run([str(test_bin)], check=True)

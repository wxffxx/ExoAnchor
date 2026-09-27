#!/usr/bin/env python3
"""Run production HID handshake/cleanup with allocation and claim failures.

The HTTP and driver boundaries are fake; the adapter functions, context layout,
authorization predicate and exact-token owner state machine are production code.
"""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "main/services/status_ws.c").read_text()


def function(signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


context_start = source.index("typedef struct {")
context_end = source.index("} hid_ws_ctx_t;", context_start) + len("} hid_ws_ctx_t;")
production = "\n".join(function(signature) for signature in (
    "static void hid_ws_ctx_free(",
    "static bool hid_ws_handshake_auth_guard(",
    "static esp_err_t hid_ws_reject_after_send(",
    "esp_err_t hid_ws_pre_handshake(",
))
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth_service.h"
#include "control_lease.h"
#include "hid_device.h"

typedef struct {
    void *sess_ctx;
    void (*free_ctx)(void *);
} httpd_req_t;
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403

static bool allocation_fails;
static bool claim_allowed;
static bool input_control_active;
static bool auth_live;
static unsigned allocations, frees, claims, releases;
static void *allocated_context;
static si_hid_owner_state_t owner_state;
static si_hid_owner_token_t last_release;
static si_auth_session_context_t session;

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

static void *test_calloc(size_t count, size_t size)
{
    allocations++;
    if (allocation_fails) return NULL;
    assert(allocated_context == NULL);
    allocated_context = calloc(count, size);
    assert(allocated_context != NULL);
    return allocated_context;
}
static void test_free(void *pointer)
{
    assert(pointer != NULL && pointer == allocated_context);
    frees++;
    free(pointer);
    allocated_context = NULL;
}
#define calloc test_calloc
#define free test_free

static esp_err_t hid_ws_stream_id(httpd_req_t *req, uint32_t *id, bool *present)
{
    (void)req;
    *id = 42U;
    *present = true;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *text)
{
    (void)req; (void)status; (void)text;
    return ESP_OK;
}
static esp_err_t si_http_send_text_status(httpd_req_t *req, const char *status,
                                         const char *text)
{
    (void)req; (void)status; (void)text;
    return ESP_OK;
}
static esp_err_t si_http_require_capability(httpd_req_t *req, si_capability_t cap,
                                           si_auth_session_context_t *out)
{
    (void)req;
    assert(cap == SI_CAPABILITY_HID);
    *out = session;
    return ESP_OK;
}
bool si_auth_session_get_live_context(const char *id, uint32_t generation,
                                      si_auth_session_context_t *out)
{
    assert(strcmp(id, session.session_id) == 0 && generation == session.generation);
    *out = session;
    return auth_live;
}
void si_control_lease_get_status(si_control_lease_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->input_control_active = input_control_active;
}
bool si_control_lease_claim_kvm_hid_owner(
    uint32_t stream_id, const char *id, uint32_t generation,
    si_hid_live_guard_fn guard, void *guard_context, si_hid_owner_token_t *owner)
{
    claims++;
    if (!claim_allowed || !guard(guard_context)) return false;
    si_hid_owner_claim_t claim = {
        .kind = SI_HID_OWNER_KVM_STREAM,
        .auth_generation = generation,
        .resource_id = stream_id,
        .authority_epoch = 7U,
    };
    strlcpy(claim.session_id, id, sizeof(claim.session_id));
    return si_hid_owner_state_install(&owner_state, &claim, true, owner);
}
bool si_hid_owner_release_if_current(const si_hid_owner_token_t *owner)
{
    releases++;
    last_release = *owner;
    return si_hid_owner_state_revoke_current(&owner_state, owner, NULL);
}
static void si_web_log(const char *level, const char *text)
{
    (void)level; (void)text;
}
'''
tests = r'''
static si_hid_owner_token_t reset(void)
{
    assert(allocated_context == NULL);
    allocations = frees = claims = releases = 0;
    allocation_fails = input_control_active = false;
    claim_allowed = auth_live = true;
    memset(&owner_state, 0, sizeof(owner_state));
    session = (si_auth_session_context_t) {
        .authenticated = true,
        .principal = SI_PRINCIPAL_BROWSER,
        .capabilities = SI_CAPABILITY_HID,
        .generation = 3U,
        .session_id = "browser-session",
    };
    si_hid_owner_claim_t previous_claim = {
        .kind = SI_HID_OWNER_KVM_STREAM,
        .auth_generation = session.generation,
        .resource_id = 42U,
        .authority_epoch = 7U,
    };
    strlcpy(previous_claim.session_id, session.session_id,
             sizeof(previous_claim.session_id));
    si_hid_owner_token_t previous;
    assert(si_hid_owner_state_install(&owner_state, &previous_claim, true, &previous));
    return previous;
}
int main(void)
{
    /* A failed replacement connection must leave the existing viewer usable. */
    si_hid_owner_token_t previous = reset();
    httpd_req_t req = {0};
    allocation_fails = true;
    assert(hid_ws_pre_handshake(&req) == ESP_ERR_NO_MEM);
    assert(si_hid_owner_state_is_current(&owner_state, &previous));
    assert(claims == 0 && allocations == 1 && frees == 0 && releases == 0);
    assert(req.sess_ctx == NULL && req.free_ctx == NULL);

    /* Both a stale lease and a session revoked at the claim boundary clean up. */
    for (unsigned revoked = 0; revoked < 2; revoked++) {
        previous = reset();
        claim_allowed = revoked != 0;
        auth_live = revoked == 0;
        assert(hid_ws_pre_handshake(&req) == ESP_ERR_INVALID_STATE);
        assert(si_hid_owner_state_is_current(&owner_state, &previous));
        assert(claims == 1 && allocations == 1 && frees == 1 && releases == 0);
        assert(req.sess_ctx == NULL && req.free_ctx == NULL);
    }

    /* An active automated-input lease is rejected before allocating or claiming. */
    previous = reset();
    input_control_active = true;
    assert(hid_ws_pre_handshake(&req) == ESP_ERR_INVALID_STATE);
    assert(si_hid_owner_state_is_current(&owner_state, &previous));
    assert(claims == 0 && allocations == 0 && releases == 0);

    /* The installed cleanup owns only its token, including after replacement. */
    for (unsigned replaced = 0; replaced < 2; replaced++) {
        previous = reset();
        assert(hid_ws_pre_handshake(&req) == ESP_OK);
        assert(!si_hid_owner_state_is_current(&owner_state, &previous));
        assert(req.sess_ctx != NULL && req.free_ctx == hid_ws_ctx_free);
        hid_ws_ctx_t *ctx = req.sess_ctx;
        si_hid_owner_token_t connected = ctx->owner;
        assert(ctx->stream_id == 42U && ctx->stream_owner_epoch == 7U);
        assert(ctx->auth_generation == session.generation);
        assert(strcmp(ctx->session_id, session.session_id) == 0);
        assert(si_hid_owner_state_is_current(&owner_state, &connected));
        si_hid_owner_token_t replacement = {0};
        if (replaced) {
            assert(si_hid_owner_state_install(&owner_state, &connected.claim,
                                               true, &replacement));
        }
        req.free_ctx(req.sess_ctx);
        req = (httpd_req_t){0};
        assert(claims == 1 && allocations == 1 && frees == 1 && releases == 1);
        assert(si_hid_owner_token_equal(&last_release, &connected));
        if (replaced) {
            assert(si_hid_owner_state_is_current(&owner_state, &replacement));
        } else {
            assert(owner_state.current.generation == 0U);
        }
    }
    puts("HID WebSocket handshake allocation runtime: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="exoanchor-hid-handshake-") as tmp:
    test_c = Path(tmp) / "test.c"
    test_bin = Path(tmp) / "test"
    test_c.write_text(harness + "\n" + source[context_start:context_end] +
                      "\n" + production + tests)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "tests/host/idf_stubs"),
        "-I" + str(ROOT / "main/application"),
        "-I" + str(ROOT / "main/core"),
        "-I" + str(ROOT / "main/drivers"),
        str(test_c), str(ROOT / "main/core/hid_owner.c"),
        str(ROOT / "main/core/authorization.c"), "-o", str(test_bin),
    ], check=True)
    subprocess.run([str(test_bin)], check=True)

#!/usr/bin/env python3
"""Run the production OTA guard; sending a denial must stop its caller."""
from pathlib import Path
import subprocess
import tempfile
from test_http_security_runtime import extract

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
typedef struct { int unused; } httpd_req_t;
typedef struct { bool enabled, using_default; } si_auth_status_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 5
#define SI_CAPABILITY_OTA 1
static int capability_result, send_result, replies, status_reads;
static si_auth_status_t input;
static int si_http_require_capability(httpd_req_t *r, int cap, void *ctx) {
    (void)r; (void)cap; (void)ctx; return capability_result;
}
static void si_auth_get_status(si_auth_status_t *out) { *out=input; status_reads++; }
static int si_http_send_text_status(httpd_req_t *r, const char *s, const char *m) {
    (void)r; (void)s; (void)m; replies++; return send_result;
}
'''
CASES = r'''
int main(void) {
    httpd_req_t req={0};
    input.enabled=true; input.using_default=true;
    assert(ota_require_auth(&req) != ESP_OK); assert(replies==1);
    send_result=-9;
    assert(ota_require_auth(&req)==-9);
    capability_result=-7; replies=status_reads=0;
    assert(ota_require_auth(&req)==-7); assert(!replies && !status_reads);
    capability_result=0; send_result=0; input.using_default=false;
    assert(ota_require_auth(&req)==ESP_OK); assert(!replies);
    input.enabled=false; input.using_default=true;
    assert(ota_require_auth(&req)==ESP_OK); assert(!replies);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='exoanchor-ota-auth-') as tmp:
    source=Path(tmp)/'guard.c'; binary=Path(tmp)/'guard'
    source.write_text(PRELUDE+extract(ROOT/'v0.86.6-dev/main/services/web/ota_settings_module.inc','ota_require_auth')+CASES)
    subprocess.run(['cc','-std=c11',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
print('OTA denial propagation: PASS')

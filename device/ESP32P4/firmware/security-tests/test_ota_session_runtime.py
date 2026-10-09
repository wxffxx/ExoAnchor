#!/usr/bin/env python3
"""Exercise production expiry/cleanup with a fake clock and contended lock."""
from pathlib import Path
import subprocess
import tempfile
from test_http_security_runtime import extract

ROOT=Path(__file__).resolve().parents[1]
PRELUDE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#define OTA_UPLOAD_SESSION_TIMEOUT_MS 300000
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static struct { bool active, sha_started; int handle, sha; int64_t last_progress_us; } s_ota_upload_session;
static struct { unsigned generation; } s_ota_upload_maintenance;
static bool s_ota_busy, suspended;
static int s_ota_lock, lock_available=1, gives, aborts, frees, finishes, resumes;
static int64_t now;
static jmp_buf stop;
static int ticks;
static int esp_ota_abort(int handle) { (void)handle; aborts++; return 0; }
static void mbedtls_sha256_free(int *sha) { (void)sha; frees++; }
static int resource_operation_finish(void *token) { (void)token; finishes++; return 0; }
static int64_t esp_timer_get_time(void) { return now; }
static int si_video_control_set_suspended(bool value) { suspended=value; resumes++; return 0; }
static void ota_set_last_message(const char *m) { (void)m; }
static void si_web_log(const char *level,const char *m) { (void)level; (void)m; }
static void vTaskDelay(int ms) { assert(ms==1000); if(ticks++) longjmp(stop,1); }
static int xSemaphoreTake(int lock,int wait) { (void)lock; assert(wait==0); return lock_available; }
static void xSemaphoreGive(int lock) { (void)lock; gives++; }
'''
CASES=r'''
static void tick(void) { ticks=0; if(!setjmp(stop)) ota_upload_session_watchdog_task(NULL); }
int main(void) {
    s_ota_upload_session.active=s_ota_upload_session.sha_started=true;
    s_ota_upload_maintenance.generation=8; s_ota_busy=suspended=true;
    now=299999000; tick();
    assert(s_ota_upload_session.active && s_ota_busy && suspended && !aborts);
    now=300000000; lock_available=0; tick();
    assert(s_ota_upload_session.active && !aborts && gives==1);
    lock_available=1; tick();
    assert(!s_ota_upload_session.active && !s_ota_busy && !suspended);
    assert(!s_ota_upload_maintenance.generation);
    assert(aborts==1 && frees==1 && finishes==1 && resumes==1 && gives==2);
    now+=1000000000; tick();
    assert(aborts==1 && frees==1 && finishes==1 && resumes==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='exoanchor-ota-expiry-') as tmp:
    source=Path(tmp)/'expiry.c'; binary=Path(tmp)/'expiry'
    path=ROOT/'v0.86.6-dev/main/services/web/ota_runtime_module.inc'
    source.write_text(PRELUDE+''.join(extract(path,name) for name in (
        'ota_upload_session_clear','ota_upload_session_expire_locked',
        'ota_upload_session_watchdog_task'))+CASES)
    subprocess.run(['cc','-std=c11',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
print('OTA automatic expiry, lock exclusion, and one-time cleanup: PASS')

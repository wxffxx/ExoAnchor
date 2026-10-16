#!/usr/bin/env python3
"""Run production TOTP/enrollment/persistence state machines on the host.

Crypto uses OpenSSL; clock, RNG, NVS, and RTOS are deterministic doubles.
No device or network is contacted.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from test_auth_runtime import STUBS, FIRMWARE

MOCKS = r'''
#include <assert.h>
#include <stdlib.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
static int64_t wall = 1800000000, mono = 1000000;
static uint32_t generation = 1, revoked;
static unsigned rng;
static bool exists, fail_read, fail_commit;
static char persisted[1024], staged[1024];
time_t mock_time(time_t *out) {if(out)*out=wall;return wall;}
int64_t esp_timer_get_time(void) {return mono;}
void esp_fill_random(void *out,size_t n) {unsigned char *p=out;for(size_t i=0;i<n;i++)p[i]=(unsigned char)++rng;}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return (void *)1;}
int xSemaphoreTake(SemaphoreHandle_t p,unsigned ms) {(void)ms;return p!=NULL;}
void xSemaphoreGive(SemaphoreHandle_t p) {(void)p;}
uint32_t si_auth_credential_generation(void) {return generation;}
esp_err_t si_auth_security_changed(uint32_t expected) {if(expected!=generation)return ESP_ERR_INVALID_STATE;generation++;revoked++;return ESP_OK;}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {(void)type;return (void *)1;}
int mbedtls_md_hmac(const mbedtls_md_info_t *md,const unsigned char *key,size_t kn,const unsigned char *data,size_t n,unsigned char *out) {
    (void)md;return HMAC(EVP_sha1(),key,(int)kn,data,n,out,NULL)?0:-1;
}
int mbedtls_sha256(const unsigned char *data,size_t n,unsigned char *out,int mode) {(void)mode;return SHA256(data,n,out)?0:-1;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) {(void)c;return ESP_OK;}
esp_err_t si_settings_store_open_read(si_settings_store_t *s,const char *ns) {(void)ns;if(fail_read)return ESP_FAIL;if(!exists)return ESP_ERR_NOT_FOUND;s->open=true;return ESP_OK;}
esp_err_t si_settings_store_open_write(si_settings_store_t *s,const char *ns) {(void)ns;s->open=true;return ESP_OK;}
void si_settings_store_close(si_settings_store_t *s) {s->open=false;}
esp_err_t si_settings_store_get_string(si_settings_store_t *s,const char *k,char *out,size_t n) {(void)s;(void)k;if(!persisted[0])return ESP_ERR_NOT_FOUND;if(strlen(persisted)>=n)return ESP_ERR_INVALID_SIZE;strcpy(out,persisted);return ESP_OK;}
esp_err_t si_settings_store_set_string(si_settings_store_t *s,const char *k,const char *v) {(void)s;(void)k;strcpy(staged,v);return ESP_OK;}
esp_err_t si_settings_store_commit(si_settings_store_t *s) {(void)s;if(fail_commit)return ESP_FAIL;strcpy(persisted,staged);exists=true;return ESP_OK;}
esp_err_t si_settings_store_get_string_size(si_settings_store_t *s,const char *k,size_t *n) {(void)s;(void)k;*n=strlen(persisted)+1;return ESP_OK;}
esp_err_t si_settings_store_erase_key(si_settings_store_t *s,const char *k) {(void)s;(void)k;return ESP_OK;}
static void reboot(void) {
    s_two_loaded=s_two_storage_ok=false;
    memset(&s_two_state,0,sizeof(s_two_state));memset(&s_two_pending,0,sizeof(s_two_pending));
    memset(s_two_pending_id,0,sizeof(s_two_pending_id));s_two_failures=0;s_two_locked_until=0;
}
static void code_for(const two_step_state_t *state,int offset,char code[7]) {
    uint32_t value;assert(two_hotp(state->secret,(uint64_t)(wall/30)+offset,6,&value));snprintf(code,7,"%06u",value);
}
int main(void) {
    const uint8_t key[20]="12345678901234567890";
    const uint32_t hotp[]={755224,287082,359152,969429,338314,254676,287922,162583,399871,520489};
    for(unsigned i=0;i<10;i++){uint32_t n;assert(two_hotp(key,i,6,&n)&&n==hotp[i]);}
    const uint64_t times[]={59,1111111109,1111111111,1234567890,2000000000,20000000000ULL};
    const uint32_t totp[]={94287082,7081804,14050471,89005924,69279037,65353130};
    for(unsigned i=0;i<6;i++){uint32_t n;assert(two_hotp(key,times[i]/30,8,&n)&&n==totp[i]);}
    char b32[33];two_base32(key,b32);assert(strcmp(b32,"GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")==0);
    assert(si_two_step_initialize()==ESP_OK);
    assert(si_two_step_verify(NULL,generation)); /* Legacy password-only device. */
    si_two_step_enrollment_t enrollment;char code[7];
    wall=0;assert(si_two_step_begin(generation,&enrollment)!=ESP_OK);wall=1800000000;
    assert(si_two_step_begin(generation+1,&enrollment)!=ESP_OK);
    assert(si_two_step_begin(generation,&enrollment)==ESP_OK);
    code_for(&s_two_pending,0,code);
    mono+=ENROLLMENT_US;assert(si_two_step_confirm(enrollment.id,code,generation)!=ESP_OK);
    assert(si_two_step_begin(generation,&enrollment)==ESP_OK);
    code_for(&s_two_pending,0,code);
    assert(si_two_step_confirm("wrong",code,generation)!=ESP_OK);
    assert(si_two_step_confirm(enrollment.id,code,generation+1)!=ESP_OK);
    assert(si_two_step_confirm(enrollment.id,code,generation)==ESP_OK && revoked==1);
    assert(!si_two_step_verify(code,generation)); /* Enrollment consumes its code. */
    assert(!si_two_step_verify(NULL,generation));
    assert(!si_two_step_verify("1234567",generation));
    reboot();assert(si_two_step_initialize()==ESP_OK);
    assert(!si_two_step_verify(code,generation)); /* Durable replay fence. */
    wall+=30;code_for(&s_two_state,0,code);
    assert(!si_two_step_verify(code,generation-1));
    assert(si_two_step_verify(code,generation));
    assert(!si_two_step_verify(code,generation));
    wall+=30;code_for(&s_two_state,1,code);assert(si_two_step_verify(code,generation));
    code_for(&s_two_state,0,code);assert(!si_two_step_verify(code,generation)); /* Clock rollback. */
    wall+=90;code_for(&s_two_state,-1,code);assert(si_two_step_verify(code,generation));
    wall=0;assert(!si_two_step_verify(code,generation));
    assert(si_two_step_verify(enrollment.recovery[0],generation));
    assert(!si_two_step_verify(enrollment.recovery[0],generation));
    reboot();assert(si_two_step_initialize()==ESP_OK);
    assert(!si_two_step_verify(enrollment.recovery[0],generation));
    si_two_step_status_t status;si_two_step_get_status(&status);
    assert(status.enabled && status.storage_ok && !status.time_ready && status.recovery_remaining==7);
    for(unsigned i=0;i<5;i++)assert(!si_two_step_verify("not-a-code",generation));
    assert(!si_two_step_verify(enrollment.recovery[1],generation));
    mono+=31000000;assert(si_two_step_verify(enrollment.recovery[1],generation));
    fail_commit=true;assert(!si_two_step_verify(enrollment.recovery[2],generation));
    assert(!s_two_storage_ok && !si_two_step_verify(enrollment.recovery[3],generation));
    fail_commit=false;reboot();assert(si_two_step_initialize()==ESP_OK);
    assert(si_two_step_disable(enrollment.recovery[3],generation)==ESP_OK && revoked==2);
    reboot();assert(si_two_step_initialize()==ESP_OK && si_two_step_verify(NULL,generation));
    fail_read=true;reboot();assert(si_two_step_initialize()!=ESP_OK);assert(!si_two_step_verify(NULL,generation));
    fail_read=false;strcpy(persisted,"corrupt");reboot();assert(si_two_step_initialize()!=ESP_OK);
    persisted[0]=0;reboot();assert(si_two_step_initialize()!=ESP_OK); /* Missing record in existing namespace. */
    si_two_step_get_status(&status);assert(status.enabled&&!status.storage_ok);
    puts("RFC vectors, enrollment, windows, replay/reboot, recovery, throttling, storage failures: PASS");
}
'''

def main():
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
    stubs=dict(STUBS)
    stubs['mbedtls/md.h']='typedef int mbedtls_md_info_t;\n#define MBEDTLS_MD_SHA1 1\nconst mbedtls_md_info_t *mbedtls_md_info_from_type(int);\nint mbedtls_md_hmac(const mbedtls_md_info_t *,const unsigned char *,size_t,const unsigned char *,size_t,unsigned char *);\n'
    stubs['esp_netif_sntp.h']='typedef struct {bool wait_for_sync;} esp_sntp_config_t;\n#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) {true}\nesp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);\n'
    with tempfile.TemporaryDirectory(prefix='exoanchor-two-step-') as directory:
        tmp=Path(directory)
        for name,contents in stubs.items():
            p=tmp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(contents)
        for version in ('v0.86.6-dev','v0.86-stable-kvm'):
            base=FIRMWARE/version/'main'
            source=tmp/'test.c';source.write_text('#define time mock_time\n'+f'#include "{base}/application/two_step.c"\n'+MOCKS)
            binary=tmp/'test'
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(tmp),'-I'+str(FIRMWARE/'v0.86.6-dev/tests/host/idf_stubs'),'-I'+str(base/'core'),'-I'+str(base/'infrastructure'),str(source),str(base/'infrastructure/secret_store.c'),*flags,'-o',str(binary)],check=True)
            print(version,flush=True);subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()

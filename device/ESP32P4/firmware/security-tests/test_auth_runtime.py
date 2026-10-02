#!/usr/bin/env python3
"""Execute both complete auth services with host crypto and mocked SDK/NVS.

OpenSSL supplies SHA256/PBKDF2; RNG, time, NVS and RTOS are deterministic
test doubles. This exercises production state transitions, not board hardware.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

FIRMWARE = Path(__file__).resolve().parents[1]
STUBS = {
    "app_config.h": '#define SI_CFG_AUTH_USERNAME "admin"\n#define SI_CFG_AUTH_PASSWORD ""\n',
    "esp_attr.h": "#define RTC_NOINIT_ATTR\n",
    "esp_random.h": "#include <stddef.h>\nvoid esp_fill_random(void *, size_t);\n#include <stdint.h>\nuint32_t esp_random(void);\n",
    "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_system.h": "#pragma once\ntypedef enum {ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT} esp_reset_reason_t;\nesp_reset_reason_t esp_reset_reason(void);\n",
    "device_settings.h": "#include <stdbool.h>\n#include <stdint.h>\ntypedef struct {bool auto_logout_enabled;uint32_t auto_logout_minutes;} si_session_settings_t;\nvoid si_session_settings_get(si_session_settings_t *);\n",
    "control_lease.h": "#include <stdint.h>\nbool si_control_lease_revoke_auth_session(const char *, uint32_t);\n",
    "hid_device.h": "",
    "freertos/FreeRTOS.h": "#pragma once\ntypedef void *SemaphoreHandle_t;\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffU\n#define pdMS_TO_TICKS(x) (x)\n",
    "freertos/semphr.h": "#include \"FreeRTOS.h\"\nSemaphoreHandle_t xSemaphoreCreateMutex(void);\nint xSemaphoreTake(SemaphoreHandle_t, unsigned);\nvoid xSemaphoreGive(SemaphoreHandle_t);\n",
    "mbedtls/md.h": "#define MBEDTLS_MD_SHA256 1\n",
    "mbedtls/sha256.h": "#include <stddef.h>\nint mbedtls_sha256(const unsigned char *, size_t, unsigned char *, int);\n",
    "mbedtls/pkcs5.h": "#include <stddef.h>\n#include <stdint.h>\nint mbedtls_pkcs5_pbkdf2_hmac_ext(int, const uint8_t *, size_t, const uint8_t *, size_t, unsigned, unsigned, uint8_t *);\n",
}
MOCKS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
static int64_t now_us = 1000000;
static unsigned random_counter;
static unsigned kdf_calls;
static int storage_mode;
static char stored_username[33], stored_verifier[65], stored_salt[33];
static uint32_t stored_iterations, stored_login_count;
static uint8_t stored_bootstrap, stored_setup;
static bool has_setup;
int64_t esp_timer_get_time(void) {return now_us;}
esp_reset_reason_t esp_reset_reason(void) {return ESP_RST_POWERON;}
uint32_t esp_random(void) {return ++random_counter;}
void esp_fill_random(void *out, size_t n) {
    unsigned char *p = out;
    for (size_t i=0;i<n;++i) p[i]=(unsigned char)++random_counter;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return malloc(1);}
int xSemaphoreTake(SemaphoreHandle_t p, unsigned ms) {(void)ms;return p!=NULL;}
void xSemaphoreGive(SemaphoreHandle_t p) {(void)p;}
void si_session_settings_get(si_session_settings_t *s) {s->auto_logout_enabled=true;s->auto_logout_minutes=1;}
bool si_control_lease_revoke_auth_session(const char *s,uint32_t g) {(void)s;(void)g;return 0;}
int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char *out,int mode) {(void)mode;return SHA256(p,n,out)?0:-1;}
int mbedtls_pkcs5_pbkdf2_hmac_ext(int md,const uint8_t *p,size_t pn,const uint8_t *salt,size_t sn,unsigned iterations,unsigned n,uint8_t *out) {
    (void)md;kdf_calls++;
    return PKCS5_PBKDF2_HMAC((const char *)p,(int)pn,salt,(int)sn,(int)iterations,EVP_sha256(),(int)n,out)==1?0:-1;
}
const char *esp_err_to_name(esp_err_t e) {(void)e;return "mock";}
esp_err_t si_settings_store_open_read(si_settings_store_t *s,const char *ns) {
    (void)ns;if(storage_mode==1)return ESP_FAIL;
    if(storage_mode==2||stored_verifier[0]){s->open=true;return ESP_OK;}
    return ESP_ERR_NOT_FOUND;
}
esp_err_t si_settings_store_open_write(si_settings_store_t *s,const char *ns) {(void)ns;s->open=true;return ESP_OK;}
void si_settings_store_close(si_settings_store_t *s){memset(s,0,sizeof(*s));}
esp_err_t si_settings_store_get_string(si_settings_store_t *s,const char *key,char *out,size_t n) {
    (void)s;
    if(storage_mode==2)return ESP_ERR_INVALID_SIZE;
    const char *value=strcmp(key,"username")==0?stored_username:strcmp(key,"password_hash")==0?stored_verifier:stored_salt;
    if(!value[0])return ESP_ERR_NOT_FOUND;
    strlcpy(out,value,n);return ESP_OK;
}
esp_err_t si_settings_store_set_string(si_settings_store_t *s,const char *key,const char *v) {
    (void)s;char *out=strcmp(key,"username")==0?stored_username:strcmp(key,"password_hash")==0?stored_verifier:stored_salt;
    strlcpy(out,v,strcmp(key,"username")==0?33:strcmp(key,"password_hash")==0?65:33);return ESP_OK;
}
esp_err_t si_settings_store_get_u32(si_settings_store_t *s,const char *key,uint32_t *v) {(void)s;*v=strcmp(key,"password_iter")==0?stored_iterations:stored_login_count;return *v?ESP_OK:ESP_ERR_NOT_FOUND;}
esp_err_t si_settings_store_set_u32(si_settings_store_t *s,const char *key,uint32_t v) {(void)s;if(strcmp(key,"password_iter")==0)stored_iterations=v;else stored_login_count=v;return ESP_OK;}
esp_err_t si_settings_store_get_u8(si_settings_store_t *s,const char *key,uint8_t *v) {(void)s;if(strcmp(key,"setup")==0){if(!has_setup)return ESP_ERR_NOT_FOUND;*v=stored_setup;}else *v=stored_bootstrap;return ESP_OK;}
esp_err_t si_settings_store_set_u8(si_settings_store_t *s,const char *key,uint8_t v) {(void)s;if(strcmp(key,"setup")==0){stored_setup=v;has_setup=true;}else stored_bootstrap=v;return ESP_OK;}
esp_err_t si_settings_store_get_string_size(si_settings_store_t *s,const char *key,size_t *n) {(void)s;(void)key;*n=0;return ESP_ERR_NOT_FOUND;}
esp_err_t si_settings_store_erase_key(si_settings_store_t *s,const char *key) {(void)s;(void)key;return ESP_ERR_NOT_FOUND;}
esp_err_t si_settings_store_commit(si_settings_store_t *s) {(void)s;return storage_mode==3?ESP_FAIL:ESP_OK;}
'''
CASES = r'''
int main(int argc,char **argv) {
    assert(argc==2);
    if(strcmp(argv[1],"read-failure")==0)storage_mode=1;
    if(strcmp(argv[1],"corrupt")==0)storage_mode=2;
    if(strcmp(argv[1],"save-failure")==0)storage_mode=3;
    if(strcmp(argv[1],"persisted-bootstrap")==0 || strcmp(argv[1],"legacy")==0) {
        strlcpy(stored_username,"admin",sizeof(stored_username));
        if(strcmp(argv[1],"legacy")==0) {
            uint8_t digest[32];mbedtls_sha256((const uint8_t *)"legacy-password",15,digest,0);
            bytes_to_hex(digest,sizeof(digest),stored_verifier,sizeof(stored_verifier));
        } else {
            strlcpy(stored_salt,"0102030405060708090a0b0c0d0e0f10",sizeof(stored_salt));
            uint8_t salt[16],digest[32];assert(hex_to_bytes(stored_salt,16,salt));
            mbedtls_pkcs5_pbkdf2_hmac_ext(1,(const uint8_t *)"persisted-password",18,salt,16,60000,32,digest);
            bytes_to_hex(digest,32,stored_verifier,sizeof(stored_verifier));
            stored_iterations=60000;stored_bootstrap=1;
        }
    }
    esp_err_t ret=si_auth_initialize();
    if(storage_mode){assert(ret!=ESP_OK);assert(si_auth_initialize()!=ESP_OK);assert(!si_auth_token_matches("not-a-session"));char token[65];assert(si_auth_create_session(token)!=ESP_OK);return 0;}
    assert(ret==ESP_OK);
    if(strcmp(argv[1],"persisted-bootstrap")==0) {
        si_auth_status_t status;si_auth_get_status(&status);assert(status.using_default);
        assert(status.setup_required && !si_auth_credentials_match("admin","persisted-password"));
        assert(si_auth_setup_credentials("owner","browser-created-password",si_auth_credential_generation())==ESP_OK);
        assert(!si_auth_setup_required());return 0;
    }
    if(strcmp(argv[1],"legacy")==0) {
        uint32_t generation=si_auth_credential_generation();
        assert(si_auth_credentials_match("admin","legacy-password"));
        assert(strlen(stored_salt)==32 && si_auth_credential_generation()==generation);
        char token[65];assert(si_auth_create_session_for_generation("web",generation,token)==ESP_OK);return 0;
    }
    assert(si_auth_setup_required());
    assert(!si_auth_credentials_match("admin","admin"));
    char unclaimed_token[65];assert(si_auth_create_session(unclaimed_token)!=ESP_OK);
    uint32_t setup_generation=si_auth_credential_generation();
    assert(si_auth_setup_credentials("owner","browser-created-password",setup_generation+1)!=ESP_OK);
    assert(si_auth_setup_required());
    assert(si_auth_setup_credentials("owner","browser-created-password",setup_generation)==ESP_OK);
    assert(!si_auth_setup_required() && !stored_bootstrap && !stored_setup);
    s_loaded=false;
#ifndef DEV_PROFILE
    s_initialized=false;
#endif
    assert(si_auth_initialize()==ESP_OK && !si_auth_setup_required());
    assert(si_auth_setup_credentials("attacker","second-claim-password",si_auth_credential_generation())!=ESP_OK);
    assert(!si_auth_credentials_match("attacker","second-claim-password"));
    assert(strlen(stored_salt)==32 && strlen(stored_verifier)==64);
    assert(si_auth_set_credentials("owner","new-local-password")==ESP_OK);
    assert(si_auth_credentials_match("owner","new-local-password"));
    char a[65],b[65];
    assert(si_auth_create_session(a)==ESP_OK);
    assert(si_auth_create_session(b)==ESP_OK);
    assert(strcmp(a,b)!=0 && strcmp(a,stored_verifier)!=0);
    assert(!si_auth_token_matches(stored_verifier));
    assert(si_auth_token_matches(a));
    si_auth_revoke_session(a);
    assert(!si_auth_token_matches(a) && si_auth_token_matches(b));
    now_us+=61000000;
    assert(!si_auth_token_matches(b));
    assert(si_auth_create_session(b)==ESP_OK);
    uint32_t old_generation = si_auth_credential_generation();
    assert(si_auth_set_credentials("owner","replacement-password")==ESP_OK);
    assert(!si_auth_token_matches(b));
    assert(si_auth_create_session_for_generation("web",old_generation,b)!=ESP_OK);
    for(int i=0;i<5;++i){
        assert(!si_auth_credentials_match("owner","incorrect"));
#ifdef DEV_PROFILE
        si_auth_record_login_failure();
#endif
    }
    assert(si_auth_login_retry_after_ms()>0);
    unsigned before=kdf_calls;
    if(si_auth_login_retry_after_ms()==0) (void)si_auth_credentials_match("owner","incorrect");
    assert(kdf_calls==before);
    now_us+=31000000;
    assert(si_auth_login_retry_after_ms()==0);
    assert(si_auth_credentials_match("owner","replacement-password"));
    return 0;
}
'''

def main():
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
    with tempfile.TemporaryDirectory(prefix="exoanchor-auth-security-") as tmp:
        tmp = Path(tmp)
        for name, content in STUBS.items():
            p = tmp / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        idf = FIRMWARE / "v0.86.6-dev/tests/host/idf_stubs"
        for version in ("v0.86-stable-kvm", "v0.86.6-dev"):
            main_dir = FIRMWARE / version / "main"
            source = tmp / "test.c"
            source.write_text(f'#include "{main_dir / "application/auth_service.c"}"\n' + MOCKS + CASES)
            binary = tmp / ("auth-tests-" + version)
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-include", "stdio.h", "-include", "string.h", "-include", "esp_log.h", "-I" + str(tmp), "-I" + str(idf),
                       "-I" + str(main_dir / "core"), "-I" + str(main_dir / "infrastructure"),
                       str(source), str(main_dir / "infrastructure/secret_store.c"), *flags, "-o", str(binary)]
            if "dev" in version:
                command.insert(1, "-DDEV_PROFILE")
            subprocess.run(command, check=True)
            for case in ("sessions", "read-failure", "corrupt", "save-failure", "persisted-bootstrap", "legacy"):
                result = subprocess.run([str(binary), case], check=True, stdout=subprocess.PIPE)
                assert result.stdout == b"", "auth service must not print initial credentials"
            print(version + " auth state/security tests: PASS")

if __name__ == "__main__":
    main()

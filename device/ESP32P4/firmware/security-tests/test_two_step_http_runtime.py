#!/usr/bin/env python3
"""Execute actual asynchronous login handlers with cJSON and mocked SDK/auth."""
from pathlib import Path
import subprocess
import tempfile
from test_http_security_runtime import extract

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef struct {const char *uri;const char *body;int method;} httpd_req_t;
typedef enum {SI_AUTH_METHOD_PASSWORD=1,SI_AUTH_METHOD_PASSKEY=2} si_auth_method_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_404_NOT_FOUND 404
#define SI_AUTH_TOKEN_LEN 64
#define SI_AUTH_USERNAME_MAX_LEN 32
#define SI_AUTH_PASSWORD_MAX_LEN 64
#define SI_TWO_STEP_CODE_MAX 32
#define SI_TWO_STEP_CLIENT_TIME_MAX 4102444800LL
#define ESP_ERR_INVALID_STATE 0x103
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define SCRATCH_BUFSIZE 2048
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define tskIDLE_PRIORITY 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static unsigned generations=1, verified, minted, clear_failures, failures;
static bool password_ok=true, factor_ok, mcp;
static int status;
static char seen_code[33], query_id[33];
static int64_t seen_time=-1;
static uint32_t si_monotonic_ms(void) {return 1000;}
static uint32_t si_auth_credential_generation(void) {return generations;}
static bool si_auth_is_enabled(void) {return true;}
static bool si_mcp_is_enabled(void) {return true;}
static bool si_auth_setup_required(void) {return false;}
static int si_auth_validate_username(const char *s) {(void)s;return ESP_OK;}
static int si_auth_validate_password(const char *s) {(void)s;return ESP_OK;}
static int si_auth_setup_credentials(const char *u,const char *p,uint32_t g) {(void)u;(void)p;(void)g;return ESP_FAIL;}
static bool si_auth_credentials_match(const char *u,const char *p) {(void)u;(void)p;return password_ok;}
static bool si_two_step_verify(const char *code,int64_t client_time,uint32_t generation) {verified++;strcpy(seen_code,code);seen_time=client_time;return factor_ok&&generation==generations;}
static void si_secret_store_clear(void *p,size_t n) {memset(p,0,n);}
static void si_auth_clear_login_failures(void) {clear_failures++;}
static void si_auth_record_login_failure(void) {failures++;}
static unsigned si_auth_login_retry_after_ms(void) {return 0;}
static int xSemaphoreTake(void *p,unsigned n) {(void)p;(void)n;return pdTRUE;}
static void xSemaphoreGive(void *p) {(void)p;}
static void vTaskDeleteWithCaps(void *p) {(void)p;}
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned n,void *arg,unsigned pri,void *out,unsigned caps) {(void)fn;(void)name;(void)n;(void)arg;(void)pri;(void)out;(void)caps;return pdPASS;}
static bool auth_request_is_mcp(httpd_req_t *r) {(void)r;return mcp;}
static void auth_login_job_make_id(char *id) {memset(id,'a',32);id[32]=0;}
static esp_err_t auth_login_send_pending(httpd_req_t *r,const char *id) {(void)r;strcpy(query_id,id);status=202;return ESP_OK;}
static esp_err_t auth_login_send_success(httpd_req_t *r,bool client,uint32_t g,si_auth_method_t m) {(void)r;(void)client;assert(g==generations&&m==SI_AUTH_METHOD_PASSWORD);minted++;status=200;return ESP_OK;}
static esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *message) {(void)r;(void)message;status=code;return ESP_OK;}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) {(void)r;(void)k;(void)v;return ESP_OK;}
static esp_err_t si_http_send_text_status(httpd_req_t *r,const char *s,const char *m) {(void)r;(void)m;status=atoi(s);return ESP_OK;}
static esp_err_t si_http_recv_json(httpd_req_t *r,char *buf,size_t n,cJSON **out) {(void)buf;(void)n;*out=cJSON_Parse(r->body);return *out?ESP_OK:ESP_FAIL;}
static int httpd_req_get_url_query_str(httpd_req_t *r,char *buf,size_t n) {(void)r;(void)n;strcpy(buf,query_id);return ESP_OK;}
static int httpd_query_key_value(const char *q,const char *k,char *out,size_t n) {(void)k;(void)n;strcpy(out,q);return ESP_OK;}
'''
CASES = r'''
int main(void) {
    s_auth_login_lock=(void *)1;
    httpd_req_t req={.uri="/api/auth/login",.body="{\"username\":\"owner\",\"password\":\"password\"}"};
    auth_login_handler(&req);assert(status==202);
    auth_login_worker(NULL);assert(verified==1&&seen_code[0]==0&&failures==1&&clear_failures==0);
    auth_login_status_handler(&req);assert(status==401&&minted==0);
    req.body="{\"username\":\"owner\",\"password\":\"password\",\"otp\":123456}";
    auth_login_handler(&req);assert(status==400);
    req.body="{\"username\":\"owner\",\"password\":\"password\",\"otp\":\"012345678901234567890123456789012\"}";
    auth_login_handler(&req);assert(status==400);
    req.body="{\"username\":\"owner\",\"password\":\"password\",\"otp\":\"012345\"}";
    password_ok=false;auth_login_handler(&req);auth_login_worker(NULL);
    assert(verified==1);auth_login_status_handler(&req);assert(status==401&&minted==0);
    password_ok=true;factor_ok=true;
    for(unsigned i=0;i<2;i++) {
        mcp=i==1;auth_login_handler(&req);assert(status==202);
        assert(strcmp(s_auth_login_job.otp,"012345")==0);
        auth_login_worker(NULL);assert(s_auth_login_job.otp[0]==0&&s_auth_login_job.password[0]==0);
        auth_login_status_handler(&req);assert(status==200&&minted==i+1);
    }
    assert(strcmp(seen_code,"012345")==0&&seen_time==0);
    const char *times[][2]={{"1900000000","1900000000"},{"\"1900000000\"","0"},{"-5","0"},{"5000000000","0"},{"null","0"}};
    for(unsigned i=0;i<5;i++) {
        static char body[160];
        snprintf(body,sizeof(body),"{\"username\":\"owner\",\"password\":\"password\",\"otp\":\"012345\",\"client_time\":%s}",times[i][0]);
        req.body=body;auth_login_handler(&req);assert(status==202);auth_login_worker(NULL);
        assert(seen_time==atoll(times[i][1]));auth_login_status_handler(&req);
    }
    minted=2;req.body="{\"username\":\"owner\",\"password\":\"password\",\"otp\":\"012345\"}";
    auth_login_handler(&req);auth_login_worker(NULL);generations++;
    auth_login_status_handler(&req);assert(status==401&&minted==2); /* Factor changed before poll. */
    puts("password + second factor required for browser/MCP; typed input, secret clearing, generation fence: PASS");
}
'''


MANAGEMENT = r'''
#define HTTP_GET 0
#define HTTP_POST 1
#define ESP_ERR_NO_MEM -2
#define SI_TWO_STEP_RECOVERY_COUNT 8
typedef struct {bool enabled,storage_ok,time_ready;unsigned recovery_remaining;} si_two_step_status_t;
typedef struct {char id[33],secret[33],recovery[8][33];} si_two_step_enrollment_t;
typedef struct {bool using_default;char username[33];} si_auth_status_t;
static bool browser_session, recent_session;
static unsigned begun, confirmed, disabled, cookies_cleared;
static bool storage_fail;
static cJSON *response;
static bool si_http_get_auth_token(httpd_req_t *r,char *out,size_t n) {(void)r;(void)n;strcpy(out,"test-token");return browser_session||mcp;}
static bool si_auth_token_is_browser(const char *t) {(void)t;return browser_session&&!mcp;}
static bool si_auth_token_is_recent(const char *t) {(void)t;return recent_session;}
static void si_auth_get_status(si_auth_status_t *s) {s->using_default=false;strcpy(s->username,"owner");}
static void si_two_step_get_status(si_two_step_status_t *s) {*s=(si_two_step_status_t){.storage_ok=true,.time_ready=true};}
static void si_http_clear_session_cookie(httpd_req_t *r) {(void)r;cookies_cleared++;}
static esp_err_t si_http_send_json(httpd_req_t *r,cJSON *j) {(void)r;cJSON_Delete(response);response=cJSON_Duplicate(j,true);status=200;return ESP_OK;}
static esp_err_t si_two_step_begin(uint32_t g,si_two_step_enrollment_t *out) {
    assert(g==generations);begun++;memset(out,0,sizeof(*out));
    memset(out->id,'a',32);memset(out->secret,'B',32);
    for(unsigned i=0;i<8;i++)memset(out->recovery[i],'c',32);
    return ESP_OK;
}
static esp_err_t si_two_step_confirm(const char *id,const char *code,int64_t t,uint32_t g) {assert(g==generations);confirmed++;seen_time=t;return storage_fail?ESP_FAIL:factor_ok&&id&&code?ESP_OK:ESP_ERR_INVALID_STATE;}
static esp_err_t si_two_step_disable(const char *code,int64_t t,uint32_t g) {assert(g==generations);disabled++;seen_time=t;return storage_fail?ESP_FAIL:factor_ok&&code?ESP_OK:ESP_ERR_INVALID_STATE;}
'''
MANAGEMENT_CASES = r'''
int main(void) {
    httpd_req_t req={.method=HTTP_GET};
    two_step_handler(&req);assert(status==401);
    mcp=true;two_step_handler(&req);assert(status==401);
    mcp=false;browser_session=true;two_step_handler(&req);assert(status==200);
    assert(!cJSON_GetObjectItem(response,"secret")&&!cJSON_GetObjectItem(response,"recovery_codes"));
    req.method=HTTP_POST;req.body="{\"action\":\"begin\",\"current_password\":\"test-password\"}";
    two_step_handler(&req);assert(status==403&&begun==0);
    recent_session=true;password_ok=false;two_step_handler(&req);assert(status==403&&begun==0);
    password_ok=true;two_step_handler(&req);assert(status==200&&begun==1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(response,"recovery_codes"))==8);
    assert(strlen(cJSON_GetObjectItem(response,"secret")->valuestring)==32);
    recent_session=false;req.body="{\"action\":\"confirm\",\"current_password\":\"test-password\"}";
    two_step_handler(&req);assert(status==403&&cookies_cleared==0);
    req.body="{\"action\":\"confirm\",\"current_password\":\"test-password\",\"otp\":\"123456\",\"enrollment_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
    two_step_handler(&req);assert(status==403&&cookies_cleared==0); /* Wrong code. */
    storage_fail=true;two_step_handler(&req);assert(status==500&&cookies_cleared==1); /* Save failed after revoke. */
    storage_fail=false;factor_ok=true;req.body="{\"action\":\"confirm\",\"current_password\":\"test-password\",\"otp\":\"123456\",\"client_time\":1900000000,\"enrollment_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
    two_step_handler(&req);assert(status==200&&cookies_cleared==2&&seen_time==1900000000);
    req.body="{\"action\":\"disable\",\"current_password\":\"test-password\",\"otp\":\"123456\"}";
    two_step_handler(&req);assert(status==403&&disabled==0);
    recent_session=true;two_step_handler(&req);assert(status==200&&disabled==1&&cookies_cleared==3&&seen_time==0);
    cJSON_Delete(response);
    puts("management: browser-only, recent login, password, enrollment capability, status secrecy, cookie revocation: PASS");
}
'''

def main():
    vendor=ROOT/'v0.86.6-dev/tests/host/vendor/cjson'
    with tempfile.TemporaryDirectory(prefix='exoanchor-two-http-') as directory:
        tmp=Path(directory)
        for profile in ('v0.86.6-dev','v0.86-stable-kvm'):
            path=ROOT/profile/'main/services/web/base_settings_http_module.inc'
            contents=path.read_text()
            types=contents[contents.index('typedef enum {\n    AUTH_LOGIN_JOB_IDLE'):contents.index('static bool auth_request_is_mcp')]
            code=PRELUDE+types
            for name in ('auth_login_job_reset_locked','auth_login_job_expired_locked','auth_client_time','auth_login_worker','auth_login_handler','auth_login_status_handler'):
                code+=extract(path,name)
            c=tmp/'test.c';c.write_text(code+CASES);binary=tmp/'test'
            subprocess.run(['cc','-std=c11','-I'+str(vendor),str(c),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
            print(profile,flush=True);subprocess.run([str(binary)],check=True)
            code=PRELUDE+MANAGEMENT+extract(path,'json_string_any')+extract(path,'auth_client_time')
            code+=extract(path.parent/'two_step_http_module.inc','two_step_handler')+MANAGEMENT_CASES
            c.write_text(code)
            subprocess.run(['cc','-std=c11','-I'+str(vendor),str(c),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()

#!/usr/bin/env python3
"""Run the real passkey HTTP handler and service behind mocked HTTP/auth APIs."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from test_auth_runtime import STUBS, FIRMWARE
from test_passkey_runtime import PK_STUB, MD_STUB, MOCKS
from test_http_security_runtime import extract

HTTP = r'''
typedef struct {int method;size_t content_len;const char *body,*origin,*host;} httpd_req_t;
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_403_FORBIDDEN 403
#define SI_TWO_STEP_CODE_MAX 32
#define SI_TWO_STEP_CLIENT_TIME_MAX 4102444800LL
static int status;
static bool browser, recent, password_ok=true, factor_ok=true, locked, mcp;
static unsigned minted, failures, cleared, checks;
static cJSON *reply;
static bool si_http_get_auth_token(httpd_req_t *r,char *out,size_t n) {(void)r;(void)n;strcpy(out,"browser-token");return browser||mcp;}
bool si_auth_token_is_browser(const char *token) {(void)token;return browser&&!mcp;}
bool si_auth_token_is_recent(const char *token) {(void)token;return recent;}
void si_secret_store_clear(void *p,size_t n) {memset(p,0,n);}
uint32_t si_auth_login_retry_after_ms(void) {return locked?1000:0;}
bool si_auth_credentials_match(const char *u,const char *p) {(void)u;(void)p;checks++;return password_ok;}
bool si_two_step_verify(const char *otp,int64_t t,uint32_t g) {(void)otp;(void)t;return factor_ok&&g==generation;}
void si_auth_record_login_failure(void) {failures++;}
void si_auth_clear_login_failures(void) {}
static esp_err_t si_http_send_json(httpd_req_t *r,cJSON *j) {(void)r;cJSON_Delete(reply);reply=cJSON_Duplicate(j,true);status=200;return ESP_OK;}
static esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *message) {(void)r;(void)message;status=code;return ESP_OK;}
static esp_err_t si_http_send_text_status(httpd_req_t *r,const char *code,const char *message) {return httpd_resp_send_err(r,atoi(code),message);}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) {(void)r;(void)k;(void)v;return ESP_OK;}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t n) {
    const char *v=!strcmp(name,"Origin")?r->origin:r->host;if(!v||strlen(v)>=n)return ESP_FAIL;strcpy(out,v);return ESP_OK;
}
static esp_err_t si_http_recv_body(httpd_req_t *r,char *out,size_t n) {if(r->content_len>=n)return ESP_FAIL;memcpy(out,r->body,r->content_len);out[r->content_len]=0;return ESP_OK;}
static void si_http_clear_session_cookie(httpd_req_t *r) {(void)r;cleared++;}
static esp_err_t auth_login_send_success(httpd_req_t *r,bool client,uint32_t g) {(void)r;assert(!client&&g==generation);minted++;status=200;return ESP_OK;}
'''
CASES = r'''
static void post(httpd_req_t *r,const char *body,int expected) {
    r->method=HTTP_POST;r->body=body;r->content_len=strlen(body);passkey_handler(r);assert(status==expected);
}
int main(void) {
    assert(si_passkey_initialize()==ESP_OK);
    httpd_req_t req={.origin="https://kvm.example.test",.host="kvm.example.test"};
    passkey_handler(&req);assert(status==401);mcp=true;passkey_handler(&req);assert(status==401);mcp=false;
    browser=true;passkey_handler(&req);assert(status==200&&!cJSON_GetObjectItem(reply,"user"));
    const char *configure="{\"action\":\"configure\",\"current_password\":\"test-password\"}";
    browser=false;post(&req,configure,401);browser=true;
    post(&req,configure,403);recent=true;
    password_ok=false;post(&req,configure,403);password_ok=true;
    factor_ok=false;post(&req,configure,403);factor_ok=true;
    locked=true;post(&req,configure,429);locked=false;
    req.origin=NULL;post(&req,configure,403);req.origin="https://evil.test";post(&req,configure,403);
    req.origin="http://kvm.example.test";post(&req,configure,403);
    req.origin="https://127.0.0.1";req.host="127.0.0.1";post(&req,configure,403);
    req.origin="https://kvm.example.test";req.host="kvm.example.test";
    post(&req,"[]",400);post(&req,"{\"action\":12}",400);
    post(&req,"{\"action\":\"configure\",\"action\":\"remove\"}",400);
    post(&req,"{\"action\":\"configure\",\"current_password\":\"test\\u0000password\"}",400);
    post(&req,"{\"action\":\"configure\",\"current_password\":\"test-password\"} {}",400);
    char deep[1200];memset(deep,'[',500);memset(deep+500,']',500);deep[1000]=0;post(&req,deep,400);
    char huge[6145];memset(huge,' ',sizeof(huge)-1);huge[sizeof(huge)-1]=0;post(&req,huge,400);
    post(&req,configure,200);assert(cleared==1&&generation==2);
    cJSON *options=NULL;assert(si_passkey_begin(false,req.origin,generation,"",NULL,&options)!=ESP_OK); /* no registered keys */
    unsigned before=checks;
    browser=false;post(&req,"{\"action\":\"register_options\",\"label\":\"Mac\",\"current_password\":\"test-password\"}",401);
    assert(checks==before);browser=true;
    post(&req,"{\"action\":\"register_options\",\"label\":\"Mac\",\"current_password\":\"test-password\"}",200);
    assert(cJSON_GetObjectItem(reply,"challenge")&&cJSON_GetObjectItem(reply,"user"));
    assert(!strcmp(pk_string(cJSON_GetObjectItem(reply,"authenticatorSelection"),"userVerification"),"required"));
    req.origin="https://other.example.test";req.host="other.example.test";
    post(&req,"{\"action\":\"register_options\",\"label\":\"Mac\",\"current_password\":\"test-password\"}",403);
    req.origin="https://kvm.example.test";req.host="kvm.example.test";
    browser=false;post(&req,"{\"action\":\"register_finish\"}",401);
    post(&req,"{\"action\":\"login_finish\"}",403);assert(minted==0&&failures>=3);
    cJSON_Delete(reply);puts("HTTP: browser/recent/password/TOTP gates, exact Origin+Host, bounds, strict JSON, generation, session revocation: PASS");
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='exo-passkey-http-') as temp:
        tmp=Path(temp);vendor=FIRMWARE/'v0.86.6-dev/tests/host/vendor/cjson'
        for name,content in {**STUBS,'mbedtls/pk.h':PK_STUB,'mbedtls/md.h':MD_STUB}.items():
            p=tmp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
        for profile in ('v0.86.6-dev','v0.86-stable-kvm'):
            b=FIRMWARE/profile/'main'
            mock=MOCKS[:MOCKS.index('int main(void)')]
            source=f'#include "{b}/application/passkey.c"\n'+mock+HTTP
            source+=extract(b/'services/web/base_settings_http_module.inc','json_string_any')
            source+=extract(b/'services/web/base_settings_http_module.inc','auth_client_time')
            source+=(b/'services/web/passkey_http_module.inc').read_text()+CASES
            c=tmp/'http.c';c.write_text(source);exe=tmp/'http'
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wno-deprecated-declarations','-fsanitize=undefined,bounds',
                '-I'+str(tmp),'-I'+str(FIRMWARE/'v0.86.6-dev/tests/host/idf_stubs'),'-I'+str(b/'core'),'-I'+str(b/'infrastructure'),'-I'+str(vendor),str(c),str(vendor/'cJSON.c'),*flags,'-o',str(exe)],check=True)
            print(profile,flush=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()

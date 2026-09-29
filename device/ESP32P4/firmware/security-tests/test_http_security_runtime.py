#!/usr/bin/env python3
"""Raw handler regression tests with real cJSON; SDK and hardware are mocks."""
from pathlib import Path
import re
import subprocess
import tempfile
import hashlib

evidence=[]
def extract(path, name):
    text = path.read_text()
    # Preserve positions while masking strings and comments for brace matching.
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text)
    match = re.search(r'(?m)^(?:static\s+)?(?:const\s+)?[\w *]+\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', masked)
    if not match:
        raise ValueError(f"function missing: {path}:{name}")
    start = match.start()
    brace = masked.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    source = text[start:end]
    evidence.append({"file": str(path), "function": name,
                     "line": text[:start].count('\n') + 1,
                     "sha256": hashlib.sha256(source.encode()).hexdigest()})
    return '\n' + source + '\n'
COMMON = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdarg.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_STATE 5
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define SCRATCH_BUFSIZE 2048
#define SI_AUTH_TOKEN_LEN 64
#define SI_HTTP_SESSION_COOKIE_MAX_LEN 160
#define SI_AUTH_USERNAME_MAX_LEN 32
#define SI_AUTH_PASSWORD_MIN_LEN 6
#define SI_AUTH_PASSWORD_MAX_LEN 64
#define TAG "repro"
#define ESP_RETURN_ON_ERROR(x, ...) do { int er=(x); if(er != ESP_OK) return er; } while(0)
typedef struct { int content_len; const char *body; } httpd_req_t;
static int denied, side_effect, power_result, token_created, credential_changed;
static int input_mcp=1, comparisons, throttle_checks, failures;
static const char *created_client;
static bool auth_allowed;
static esp_err_t httpd_resp_send_err(httpd_req_t *r, int status, const char *m) {
    (void)r; (void)m; if(status==401) denied++; return ESP_OK;
}
static esp_err_t si_http_recv_json(httpd_req_t *r, char *b, size_t n, cJSON **out) {
    (void)b; (void)n; *out=cJSON_Parse(r->body); return *out ? ESP_OK : ESP_FAIL;
}
static esp_err_t si_http_send_text_status(httpd_req_t *r,const char *s,const char *m) {
    (void)r;(void)s;(void)m;return ESP_OK;
}
static esp_err_t si_http_send_json(httpd_req_t *r,cJSON *j) { (void)r;(void)j;return ESP_OK; }
static void si_web_log(const char *level,const char *msg) { (void)level;(void)msg; }
static esp_err_t power_status_handler(httpd_req_t *r) {(void)r;return ESP_OK;}
static esp_err_t si_power_press_power(uint32_t m) {(void)m;side_effect++;return power_result;}
static esp_err_t si_power_press_reset(uint32_t m) {(void)m;side_effect++;return power_result;}
static esp_err_t si_power_force_off(uint32_t m) {(void)m;side_effect++;return power_result;}
static esp_err_t si_power_set_locator(bool m) {(void)m;side_effect++;return power_result;}
static esp_err_t si_power_toggle_locator(void) {side_effect++;return power_result;}
static const char *esp_err_to_name(int e) {(void)e;return "mock error";}
'''
DEV_MOCKS = r'''#define SI_PRINCIPAL_BROWSER 1
#define SI_CAPABILITY_SETTINGS 128
static int input_settings;
static int64_t authenticated_at=99000000;
static int64_t esp_timer_get_time(void) {return 100000000;}

typedef struct { bool authenticated; int principal; uint32_t capabilities; int64_t authenticated_at_us; } si_auth_session_context_t;
typedef struct { bool enabled; char username[33]; } si_auth_status_t;
static bool si_http_get_session_context(httpd_req_t *r,si_auth_session_context_t *c) {
    (void)r; c->authenticated=auth_allowed; c->principal=input_mcp?2:1;
    c->authenticated_at_us=authenticated_at; c->capabilities=input_settings?SI_CAPABILITY_SETTINGS:1; /* observe only; no SETTINGS bit */
    return auth_allowed;
}
static void si_auth_get_status(si_auth_status_t *s) {s->enabled=true;strcpy(s->username,"owner");}
static bool si_auth_credentials_match(const char *u,const char *p) {(void)u;(void)p;comparisons++;return false;}
static esp_err_t si_auth_validate_username(const char *u) {return u&&strlen(u)?ESP_OK:ESP_ERR_INVALID_ARG;}
static esp_err_t si_auth_validate_password(const char *p) {return p&&strlen(p)>=6?ESP_OK:ESP_ERR_INVALID_ARG;}
static esp_err_t si_auth_set_credentials(const char *u,const char *p) {(void)u;(void)p;credential_changed++;return ESP_OK;}
static esp_err_t si_auth_create_session_for_client(const char *c,char out[65]) {
    created_client=c;token_created++;strcpy(out,"synthetic-browser-token");return ESP_OK;
}
static esp_err_t si_http_set_session_cookie(httpd_req_t *r,const char *t,char *c,size_t n) {
    (void)r;(void)t;(void)c;(void)n;return ESP_OK;
}
static void si_auth_revoke_session(const char *t) {(void)t;}
static uint32_t si_auth_login_retry_after_ms(void) {throttle_checks++;return 30000;}
static void si_auth_record_login_failure(void) {failures++;}
'''

ROOT = Path(__file__).resolve().parents[1]
TRACKER = r'''
struct allocation { void *ptr; bool freed; };
static struct allocation allocations[128];
static int allocation_count, log_seen;
static void *tracked_malloc(size_t n) {
    void *p=malloc(n); assert(allocation_count<128);
    allocations[allocation_count++]=(struct allocation){p,false};return p;
}
static void tracked_free(void *p) {
    for(int i=allocation_count-1;i>=0;i--) if(allocations[i].ptr==p&&!allocations[i].freed) {
        allocations[i].freed=true;break;
    }
    free(p);
}
static int tracked_snprintf(char *out,size_t n,const char *fmt,...) {
    (void)fmt;log_seen++;
    va_list ap;va_start(ap,fmt);const char *action=va_arg(ap,const char *);va_end(ap);
    for(int i=allocation_count-1;i>=0;i--) if(allocations[i].ptr==action) {
        assert(!allocations[i].freed);break;
    }
    assert(strcmp(action,"reset")==0);
    if(n) out[0]='\0';return 0;
}
#undef snprintf
#define snprintf tracked_snprintf
'''

def main():
    stable=ROOT/'v0.86-stable-kvm/main'
    dev=ROOT/'v0.86.6-dev/main'
    vendor=ROOT/'v0.86.6-dev/tests/host/vendor/cjson'
    json_helper=extract(dev/'services/web/base_settings_http_module.inc','json_string_any')
    source=COMMON+'\nstatic bool si_http_check_auth(httpd_req_t *r) {(void)r;return auth_allowed;}\n'
    source+=extract(stable/'adapters/http_api.c','si_http_require_auth')+json_helper+TRACKER
    source+=extract(stable/'services/web/power_http_module.inc','power_action_handler')
    source+=r"""
int main(void) {
    cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
    httpd_req_t req={.body="{\"action\":\"reset\"}",.content_len=18};
    assert(power_action_handler(&req)!=ESP_OK);
    assert(denied==1 && side_effect==0 && log_seen==0);
    auth_allowed=true;power_result=ESP_OK;
    assert(power_action_handler(&req)==ESP_OK);
    assert(side_effect==1 && log_seen==1);
    return 0;
}
"""
    dev_source=COMMON+DEV_MOCKS+extract(dev/'application/auth_service.c','si_auth_create_session')+json_helper
    dev_source+=extract(dev/'services/web/base_settings_http_module.inc','settings_password_handler')
    dev_source+=r"""
int main(void) {
    httpd_req_t req={.body="{\"username\":\"owner\",\"new_password\":\"synthetic-password\"}",.content_len=60};
    auth_allowed=true;input_mcp=1;input_settings=1;
    settings_password_handler(&req);assert(credential_changed==0 && token_created==0);
    input_mcp=0;input_settings=0;
    settings_password_handler(&req);assert(credential_changed==0 && token_created==0);
    auth_allowed=false;
    req.body="{\"current_username\":\"owner\",\"current_password\":\"wrong-guess\",\"new_password\":\"replacement\"}";
    for(int i=0;i<10;i++)settings_password_handler(&req);
    assert(comparisons==0 && credential_changed==0 && token_created==0);
    auth_allowed=true;input_settings=1;authenticated_at=1;
    settings_password_handler(&req);assert(credential_changed==0);
    authenticated_at=99000000;
    settings_password_handler(&req);assert(credential_changed==1 && token_created==1 && created_client==NULL);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix='exoanchor-http-security-') as temp:
        temp=Path(temp)
        for label,code in [('stable',source),('dev',dev_source)]:
            c=temp/(label+'.c');c.write_text(code);binary=temp/label
            subprocess.run(['cc','-std=c11','-I'+str(vendor),str(c),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
            print(label+' HTTP authorization/memory tests: PASS')

if __name__=='__main__':main()

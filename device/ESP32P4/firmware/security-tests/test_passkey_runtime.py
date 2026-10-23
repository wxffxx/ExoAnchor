#!/usr/bin/env python3
"""Production WebAuthn verifier/state machine with real ES256 signatures.

OpenSSL adapts the mbedTLS API; SDK/NVS/clock are host doubles. No hardware.
The line-oriented fixture also supports browser QA with virtual authenticators.
"""
from pathlib import Path
import base64
import hashlib
import json
import os
import shlex
import struct
import subprocess
import tempfile
from test_auth_runtime import STUBS, FIRMWARE

PK_STUB = r'''
#include <openssl/evp.h>
typedef struct {EVP_PKEY *key;} mbedtls_pk_context;
#define MBEDTLS_MD_SHA256 1
void mbedtls_pk_init(mbedtls_pk_context *);
void mbedtls_pk_free(mbedtls_pk_context *);
int mbedtls_pk_parse_public_key(mbedtls_pk_context *,const unsigned char *,size_t);
int mbedtls_pk_verify(mbedtls_pk_context *,int,const unsigned char *,size_t,const unsigned char *,size_t);
'''
MD_STUB = r'''
typedef int mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
const mbedtls_md_info_t *mbedtls_md_info_from_type(int);
int mbedtls_md_hmac(const mbedtls_md_info_t *,const unsigned char *,size_t,const unsigned char *,size_t,unsigned char *);
'''
MOCKS = r'''
#include <assert.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
static int64_t mono=1000000;
static uint32_t generation=1;
static bool exists,fail_commit,fail_read,rotate_commit;
static char persisted[PK_RECORD_MAX], staged[PK_RECORD_MAX];
int64_t esp_timer_get_time(void) {return mono;}
void esp_fill_random(void *out,size_t n) {assert(RAND_bytes(out,(int)n)==1);}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return (void *)1;}
int xSemaphoreTake(SemaphoreHandle_t p,unsigned ms) {(void)ms;return p!=NULL;}
void xSemaphoreGive(SemaphoreHandle_t p) {(void)p;}
uint32_t si_auth_credential_generation(void) {return generation;}
void si_auth_get_status(si_auth_status_t *s) {memset(s,0,sizeof(*s));s->enabled=true;strcpy(s->username,"owner");}
esp_err_t si_auth_security_commit(uint32_t g,esp_err_t (*commit)(void *),void *context) {
    if(rotate_commit){generation++;rotate_commit=false;}
    if(g!=generation)return ESP_ERR_INVALID_STATE;
    generation++;return commit(context);
}
int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char *out,int mode) {(void)mode;return SHA256(p,n,out)?0:-1;}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {static const mbedtls_md_info_t info=1;return type==MBEDTLS_MD_SHA256?&info:NULL;}
int mbedtls_md_hmac(const mbedtls_md_info_t *md,const unsigned char *key,size_t kn,const unsigned char *data,size_t n,unsigned char *out) {
    (void)md;return HMAC(EVP_sha256(),key,(int)kn,data,n,out,NULL)?0:-1;
}
void mbedtls_pk_init(mbedtls_pk_context *k) {k->key=NULL;}
void mbedtls_pk_free(mbedtls_pk_context *k) {EVP_PKEY_free(k->key);k->key=NULL;}
int mbedtls_pk_parse_public_key(mbedtls_pk_context *k,const unsigned char *p,size_t n) {
    const unsigned char *end=p+n;k->key=d2i_PUBKEY(NULL,&p,(long)n);
    if(!k->key || p!=end)return -1;
    EVP_PKEY_CTX *ctx=EVP_PKEY_CTX_new(k->key,NULL);int ret=ctx?EVP_PKEY_public_check(ctx):0;EVP_PKEY_CTX_free(ctx);return ret==1?0:-1;
}
int mbedtls_pk_verify(mbedtls_pk_context *k,int md,const unsigned char *hash,size_t n,const unsigned char *sig,size_t sn) {
    (void)md;EVP_PKEY_CTX *ctx=EVP_PKEY_CTX_new(k->key,NULL);
    int ok=ctx&&EVP_PKEY_verify_init(ctx)==1&&EVP_PKEY_CTX_set_signature_md(ctx,EVP_sha256())==1&&EVP_PKEY_verify(ctx,sig,sn,hash,n)==1;
    EVP_PKEY_CTX_free(ctx);return ok?0:-1;
}
esp_err_t si_settings_store_open_read(si_settings_store_t *s,const char *ns) {(void)ns;if(fail_read)return ESP_FAIL;if(!exists)return ESP_ERR_NOT_FOUND;s->open=true;return ESP_OK;}
esp_err_t si_settings_store_open_write(si_settings_store_t *s,const char *ns) {(void)ns;s->open=true;return ESP_OK;}
void si_settings_store_close(si_settings_store_t *s) {s->open=false;}
esp_err_t si_settings_store_get_string(si_settings_store_t *s,const char *k,char *out,size_t n) {(void)s;(void)k;if(!persisted[0])return ESP_ERR_NOT_FOUND;if(strlen(persisted)>=n)return ESP_ERR_INVALID_SIZE;strcpy(out,persisted);return ESP_OK;}
esp_err_t si_settings_store_set_string(si_settings_store_t *s,const char *k,const char *v) {(void)s;(void)k;assert(strlen(v)<sizeof(staged));strcpy(staged,v);return ESP_OK;}
esp_err_t si_settings_store_commit(si_settings_store_t *s) {(void)s;if(fail_commit)return ESP_FAIL;strcpy(persisted,staged);exists=true;return ESP_OK;}
static void reboot(void) {
    memset(&s_pk,0,sizeof(s_pk));memset(s_pk_pending,0,sizeof(s_pk_pending));
    memset(s_pk_spent,0,sizeof(s_pk_spent));
    s_pk_loaded=s_pk_storage_ok=s_pk_login_key_ready=false;generation=1;
}
int main(void) {
    assert(si_passkey_initialize()==ESP_OK);
    char line[16384];
    while(fgets(line,sizeof(line),stdin)) {
        cJSON *j=cJSON_Parse(line),*out=NULL;assert(j);
        const char *op=pk_string(j,"op"),*origin=pk_string(j,"origin");
        if(!origin)origin="https://kvm.example.test";
        const char *session=pk_string(j,"session");if(!session)session="browser-session";
        cJSON *g=cJSON_GetObjectItem(j,"generation");uint32_t expected=cJSON_IsNumber(g)?(uint32_t)g->valuedouble:generation;
        bool registration=cJSON_IsTrue(cJSON_GetObjectItem(j,"registration"));
        int ret=ESP_OK;
        if(!strcmp(op,"configure"))ret=si_passkey_configure(origin,expected);
        else if(!strcmp(op,"begin"))ret=si_passkey_begin(registration,origin,expected,session,pk_string(j,"label")?pk_string(j,"label"):"My passkey",&out);
        else if(!strcmp(op,"finish"))ret=si_passkey_finish(registration,origin,session,cJSON_GetObjectItem(j,"response"),&expected);
        else if(!strcmp(op,"remove"))ret=si_passkey_remove(pk_string(j,"id"),expected);
        else if(!strcmp(op,"status"))out=si_passkey_status(true);
        else if(!strcmp(op,"rotate"))generation++;
        else if(!strcmp(op,"tick"))mono+=(int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j,"us"));
        else if(!strcmp(op,"fail_commit"))fail_commit=cJSON_IsTrue(cJSON_GetObjectItem(j,"value"));
        else if(!strcmp(op,"rotate_commit"))rotate_commit=true;
        else if(!strcmp(op,"reboot")) {reboot();ret=si_passkey_initialize();}
        else if(!strcmp(op,"corrupt")) {exists=true;strcpy(persisted,pk_string(j,"value"));reboot();ret=si_passkey_initialize();}
        else if(!strcmp(op,"read_failure")) {fail_read=true;reboot();ret=si_passkey_initialize();}
        else if(!strcmp(op,"origin"))ret=si_passkey_origin_valid(origin)?ESP_OK:ESP_FAIL;
        else assert(false);
        cJSON *reply=cJSON_CreateObject();cJSON_AddBoolToObject(reply,"ok",ret==ESP_OK);
        cJSON_AddNumberToObject(reply,"generation",generation);
        if(out)cJSON_AddItemToObject(reply,"result",out);
        char *text=cJSON_PrintUnformatted(reply);puts(text);fflush(stdout);cJSON_free(text);cJSON_Delete(reply);cJSON_Delete(j);
        mono+=2000000;
    }
}
'''

def build(profile, tmp):
    tmp=Path(tmp);tmp.mkdir(parents=True,exist_ok=True)
    stubs={**STUBS,'mbedtls/pk.h':PK_STUB,'mbedtls/md.h':MD_STUB}
    for name, content in stubs.items():
        p=tmp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
    base=FIRMWARE/profile/'main';vendor=FIRMWARE/'v0.86.6-dev/tests/host/vendor/cjson'
    source=tmp/'passkey-host.c';source.write_text(f'#include "{base}/application/passkey.c"\n'+MOCKS)
    binary=tmp/'passkey-host'
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fsanitize=undefined,bounds','-g',
        '-I'+str(tmp),'-I'+str(FIRMWARE/'v0.86.6-dev/tests/host/idf_stubs'),'-I'+str(base/'core'),'-I'+str(base/'infrastructure'),
        '-I'+str(vendor),str(source),str(vendor/'cJSON.c'),*flags,'-o',str(binary)],check=True)
    return binary

class Host:
    def __init__(self,binary):
        self.p=subprocess.Popen([str(binary)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def call(self,op,**kwargs):
        self.p.stdin.write(json.dumps({'op':op,**kwargs})+'\n');self.p.stdin.flush()
        line=self.p.stdout.readline()
        assert line, 'host stopped unexpectedly'
        return json.loads(line)
    def close(self):
        self.p.stdin.close();assert self.p.wait()==0


def b64(b):return base64.urlsafe_b64encode(b).decode().rstrip('=')
def un64(s):return base64.urlsafe_b64decode(s+'='*(-len(s)%4))
def cbor(v):
    def head(t,n):return bytes([(t<<5)|n]) if n<24 else bytes([(t<<5)|24,n]) if n<256 else bytes([(t<<5)|25])+struct.pack('>H',n)
    if isinstance(v,int):return head(0,v) if v>=0 else head(1,-1-v)
    if isinstance(v,bytes):return head(2,len(v))+v
    if isinstance(v,str):return head(3,len(v.encode()))+v.encode()
    if isinstance(v,dict):return head(5,len(v))+b''.join(cbor(k)+cbor(value) for k,value in v.items())
    raise ValueError(v)

class Authenticator:
    def __init__(self,tmp):
        self.key=Path(tmp)/'key.pem'
        subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',str(self.key)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.point=subprocess.check_output(['openssl','pkey','-in',str(self.key),'-pubout','-outform','DER'])[-65:]
        self.id=os.urandom(32);self.user=None
    def response(self,options,registration=False,**changes):
        rp=options.get('rp',{}).get('id',options.get('rpId'))
        client={'type':'webauthn.create' if registration else 'webauthn.get','challenge':options['challenge'],'origin':'https://kvm.example.test','crossOrigin':False}
        client.update(changes.get('client',{}));raw=changes.get('raw_client',json.dumps(client,separators=(',',':')).encode())
        flags=changes.get('flags',0x4d if registration else 0x0d)
        data=hashlib.sha256(changes.get('rp',rp).encode()).digest()+bytes([flags])+struct.pack('>I',changes.get('counter',0))
        if registration:
            self.user=options['user']['id']
            cose={1:2,3:changes.get('alg',-7),-1:1,-2:self.point[1:33],-3:self.point[33:]}
            data+=b'\0'*16+struct.pack('>H',len(self.id))+self.id+cbor(cose)
            att=changes.get('att',cbor({'fmt':'none','attStmt':{},'authData':data}))
            return {'type':'public-key','rawId':b64(self.id),'clientDataJSON':b64(raw),'attestationObject':b64(att)}
        sig=subprocess.check_output(['openssl','dgst','-sha256','-sign',str(self.key)],input=data+hashlib.sha256(raw).digest())
        return {'type':'public-key','rawId':b64(self.id),'clientDataJSON':b64(raw),'authenticatorData':b64(data),'signature':b64(sig),'userHandle':self.user}

def cases(binary,tmp):
    h=Host(binary);a=Authenticator(tmp)
    def begin(reg=False,**kw):
        r=h.call('begin',registration=reg,**kw);assert r['ok'],r;return r['result']
    def finish(options,reg=False,good=True,**kw):
        response=a.response(options,reg,**kw);r=h.call('finish',registration=reg,response=response);assert r['ok']==good,r;return response
    try:
        for origin in ('https://kvm.example.test','https://exoanchor-example.local','https://localhost:8443'):
            assert h.call('origin',origin=origin)['ok']
        for origin in ('http://kvm.test','https://127.0.0.1','https://[::1]','https://foo.123','https://a.test/','https://A.test','https://a.test:443','https://a.test:0','https://a.test:0443','https://a.test:65536','https://-a.test','https://a-.test','https://a..test','https://a.test@evil.test','https://a.test?evil','https://a.test#evil'):
            assert not h.call('origin',origin=origin)['ok'],origin
        assert not h.call('begin')['ok']
        assert not h.call('configure',generation=99)['ok']
        assert h.call('configure')['ok']
        assert not h.call('begin',origin='https://evil.test',registration=True)['ok']
        opts=begin(True);h.call('rotate');finish(opts,True,False)
        opts=begin(True);h.call('tick',us=120000000);finish(opts,True,False)
        opts=begin(True);response=a.response(opts,True)
        assert not h.call('finish',registration=True,session='another-session',response=response)['ok']
        assert not h.call('finish',registration=True,response=response)['ok']
        for change in ({'flags':0x49},{'flags':0x4c},{'flags':0x55},{'rp':'evil.test'},{'alg':-257},{'client':{'origin':'https://evil.test'}},{'client':{'crossOrigin':True}},{'att':b'\xa3\x63fmt\x64none'}):
            finish(begin(True),True,False,**change)
        opts=begin(True);h.call('rotate_commit');finish(opts,True,False)
        assert not h.call('status')['result']['enabled']
        finish(begin(True),True)
        saved=h.call('status')['result'];assert saved['enabled'] and len(saved['credentials'])==1
        assert not h.call('configure')['ok']
        finish(begin(True),True,False) # duplicate ID
        response=finish(begin());assert not h.call('finish',response=response)['ok']
        for change in ({'flags':9},{'flags':12},{'flags':0x15},{'flags':5},{'rp':'evil.test'},{'client':{'origin':'https://evil.test'}},{'client':{'type':'webauthn.create'}},{'client':{'crossOrigin':True}},{'client':{'crossOrigin':'false'}},{'client':{'topOrigin':'https://kvm.example.test'}}):
            finish(begin(),False,False,**change)
        for field,value in [('signature',b64(os.urandom(70))),('rawId',b64(os.urandom(32))),('userHandle',b64(os.urandom(32))),('userHandle',''),('type','password')]:
            response=a.response(begin());response[field]=value;assert not h.call('finish',response=response)['ok']
        opts=begin();raw=json.dumps({'type':'webauthn.get','challenge':opts['challenge'],'origin':'https://kvm.example.test'})[:-1]+',"origin":"https://evil.test"}'
        finish(opts,False,False,raw_client=raw.encode())
        opts=begin();finish(opts,False,False,client={'origin':'https://kvm.example.test\0.evil.test'})
        h.call('tick',us=120000000)
        finish(begin(),counter=0) # Apple-style synced counter=0
        finish(begin(),counter=1);finish(begin(),False,False,counter=1);finish(begin(),False,False,counter=0)
        assert h.call('reboot')['ok'];finish(begin(),False,False,counter=1);finish(begin(),counter=2)
        response=a.response(begin(),counter=3);h.call('rotate');assert not h.call('finish',response=response)['ok']
        h.call('fail_commit',value=True);finish(begin(),False,False,counter=3)
        assert not h.call('status')['result']['storage_ok'];assert not h.call('begin')['ok']
        h.call('fail_commit',value=False);assert h.call('reboot')['ok'];finish(begin(),counter=3)
        # Anonymous login options are stateless: a flood cannot exhaust slots or
        # evict a real user's challenge, and enrollment stays available.
        options=[begin() for _ in range(20)]
        assert h.call('begin',registration=True)['ok'];finish(options[0],counter=4)
        response=a.response(options[1],counter=5);assert h.call('finish',response=response)['ok']
        assert not h.call('finish',response=response)['ok'] # verified challenge is single-use
        forged=dict(options[2]);raw=bytearray(un64(forged['challenge']));raw[0]^=1;forged['challenge']=b64(bytes(raw))
        finish(forged,False,False,counter=6) # MAC binds nonce/expiry/generation
        raw=bytearray(un64(options[2]['challenge']));raw[16:24]=struct.pack('>q',2**62);forged['challenge']=b64(bytes(raw))
        finish(forged,False,False,counter=6) # extended expiry is rejected
        for i in range(5):finish(options[3+i],counter=6+i) # 8 accepted logins are now live
        finish(options[8],False,False,counter=12) # spent set full: refuse rather than forget a replay
        h.call('tick',us=120000000);finish(begin(),counter=12)
        stale=begin();h.call('rotate');finish(stale,False,False,counter=13) # generation fenced
        late=begin();assert h.call('reboot')['ok'];finish(late,False,False,counter=13) # boot key rotates
        h.call('tick',us=120000000)
        assert not h.call('remove',id=b64(a.id),generation=999)['ok']
        assert h.call('remove',id=b64(a.id))['ok'];assert not h.call('begin')['ok']
        assert h.call('reboot')['ok'];assert not h.call('status')['result']['enabled']
        assert not h.call('corrupt',value='{}')['ok'];assert not h.call('begin',registration=True)['ok']
        assert not h.call('corrupt',value='')['ok'];assert not h.call('read_failure')['ok']
    finally:h.close()
    # Maximum-sized credentials, full store, removal, configuration changes.
    h=Host(binary)
    try:
        assert h.call('configure')['ok']
        ids=[]
        for i in range(4):
            a.id=os.urandom(256);ids.append(b64(a.id));r=h.call('begin',registration=True);assert r['ok']
            assert h.call('finish',registration=True,response=a.response(r['result'],True))['ok']
        assert not h.call('begin',registration=True)['ok'];assert h.call('reboot')['ok']
        assert len(h.call('status')['result']['credentials'])==4
        for id_ in ids:assert h.call('remove',id=id_)['ok']
        assert h.call('configure',origin='https://other.example.test')['ok']
    finally:h.close()


def main():
    with tempfile.TemporaryDirectory(prefix='exo-passkey-') as temp:
        for profile in ('v0.86.6-dev','v0.86-stable-kvm'):
            tmp=Path(temp)/profile;binary=build(profile,tmp);cases(binary,tmp)
            print(profile+': ES256 registration/login, origin/RP/UV/user binding, replay, expiry, generations, CBOR, counters/reboot, storage failure, limits: PASS',flush=True)
if __name__=='__main__':main()

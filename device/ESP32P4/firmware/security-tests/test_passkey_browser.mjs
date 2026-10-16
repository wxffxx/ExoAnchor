// Exercise the shipped browser client in both profiles. Crypto is covered by
// test_passkey_runtime.py; this checks protocol encoding and cancellation.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
for (const profile of ['v0.86.6-dev','v0.86-stable-kvm']) {
  const values=new Map(),storage={getItem:k=>values.get(k)||null,setItem:(k,v)=>values.set(k,String(v)),removeItem:k=>values.delete(k)};
  const window=Object.assign(new EventTarget(),{isSecureContext:true,PublicKeyCredential:function(){},setTimeout,clearTimeout,setInterval,clearInterval});
  const document=Object.assign(new EventTarget(),{hidden:false,getElementById:()=>null});
  const location={protocol:'https:',hostname:'kvm.example.test',origin:'https://kvm.example.test'};
  let pending=false, navigated=false;const requests=[];
  const credential={type:'public-key',rawId:new Uint8Array([255,254]).buffer,response:{clientDataJSON:new Uint8Array([1,2]).buffer,attestationObject:new Uint8Array([3,4]).buffer,authenticatorData:new Uint8Array([5,6]).buffer,signature:new Uint8Array([7,8]).buffer,userHandle:new Uint8Array([9,10]).buffer}};
  const navigator={credentials:{get:async options=>{
    assert.equal(options.publicKey.challenge.length,32);
    if(pending)return new Promise((resolve,reject)=>{options.signal.addEventListener('abort',()=>reject(new DOMException('cancelled','AbortError')));navigated=true;});
    return credential;
  },create:async options=>{
    assert.equal(options.publicKey.user.id.length,32);assert.equal(options.publicKey.excludeCredentials[0].id[0],255);return credential;
  }}};
  const context=vm.createContext({window,document,location,navigator,localStorage:storage,sessionStorage:storage,AbortController,DOMException,Uint8Array,atob,btoa,setTimeout,clearTimeout,setInterval,clearInterval,console,fetch:async (url,options)=>{
    const body=JSON.parse(options.body);requests.push(body);
    assert.equal(options.credentials,'same-origin');assert.equal(options.cache,'no-store');
    const data=body.action.endsWith('_options')?{challenge:btoa('\0'.repeat(32)).replace(/=+$/,''),user:{id:btoa('\0'.repeat(32)).replace(/=+$/,'')},excludeCredentials:[{type:'public-key',id:'_w'}]}:{token:'new-session',username:'owner'};
    return {ok:true,json:async()=>data};
  }});
  vm.runInContext(fs.readFileSync(path.join(root,profile,'main/www/assets/ui-core.js'),'utf8'),context);
  const ui=window.ExoAnchorUI,pk=ui.passkeys;
  assert.equal(pk.problem(),'');location.hostname='127.0.0.1';assert.match(pk.problem(),/IP/);location.hostname='kvm.example.test';window.isSecureContext=false;assert.match(pk.problem(),/HTTPS/);window.isSecureContext=true;
  const body={current_password:'test-password',otp:'123456'};
  await pk.perform('register',body);assert.equal(body.current_password,'');assert.equal(body.otp,'');
  assert.equal(requests[1].attestationObject,'AwQ');assert.equal(requests[1].rawId,'__4');
  await pk.perform('login');assert.equal(ui.api.token,'new-session');assert.equal(requests.at(-1).userHandle,'CQo');
  for(const event of ['exoanchor:logout','exoanchor:session-expired','pagehide']) {
    pending=true;navigated=false;const start=requests.length,login=pk.perform('login');
    while(!navigated)await new Promise(resolve=>setTimeout(resolve,1));
    (event==='pagehide'?window:document).dispatchEvent(new Event(event));
    await assert.rejects(login,/取消/);assert.equal(pk.controller,null);assert.equal(requests.length,start+1);
  }
  pending=false;await pk.perform('login');assert.equal(pk.controller,null);
  console.log(profile+': browser secure-context checks, base64url, registration/login, cancellation and credential clearing: PASS');
}

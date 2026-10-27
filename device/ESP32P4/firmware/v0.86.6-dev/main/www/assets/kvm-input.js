/* ExoAnchor KVM page module. Loaded as a classic script in a fixed order after
 * ui-core.js and ui-shell.js; the modules share one global lexical scope, so
 * declarations here are visible to the modules loaded after it. Order:
 * kvm-core -> kvm-video -> kvm-lease -> kvm-modes -> kvm-input -> kvm-status.
 * Human input: direct click, pointer/keyboard capture, clipboard typing, joystick, touchpad, shortcuts and the soft keyboard.
 */
function clamp(v,min,max){return Math.max(min,Math.min(max,v))}
function speed(){return Number(mouseSpeed.value||5)}
const direct={enabled:localStorage.getItem("si_direct_click")!=="0",hover:null,drag:false,pointer:null,button:0};
function activeVideoRect(){const active=!h264Video.hidden?h264Video:!h264MseVideo.hidden?h264MseVideo:video,r=active.getBoundingClientRect();const vw=active===h264Video?(h264Video.width||16):active===h264MseVideo?(h264MseVideo.videoWidth||16):(video.naturalWidth||16),vh=active===h264Video?(h264Video.height||9):active===h264MseVideo?(h264MseVideo.videoHeight||9):(video.naturalHeight||9),ar=vw/vh;let w=r.width,h=w/ar;if(h>r.height){h=r.height;w=h*ar}const left=r.left+(r.width-w)/2,top=r.top+(r.height-h)/2;return{left,top,width:w,height:h}}
let agentMirrorTimer=0;
function mirrorAgentHidEvent(event,statusValue){
  if(localStorage.getItem("si_agent_action_visualization")==="0")return;
  if(event?.channel!=="hid")return;
  const text=event?.redacted?"[sensitive action redacted]":String(event?.text||"Agent HID action");
  const phase=String(event?.phase||event?.kind||"event");
  agentTakeoverBanner.classList.remove("hide");
  reclaimKvm.hidden=false;
  agentTakeoverDetail.textContent=`INPUT · ${phase.toUpperCase()} / VIDEO · ${browserVideoReady()?"LIVE":"RECOVERING"}`;
  agentActionToast.textContent=`AGENT · ${phase} · ${text}`;
  agentActionToast.classList.toggle("bad",phase==="failed");
  agentActionToast.classList.add("show");
  clearTimeout(agentMirrorTimer);
  agentMirrorTimer=setTimeout(()=>{agentActionToast.classList.remove("show");if(!statusValue?.running)agentCursor.classList.remove("show")},2200);
  if(phase!=="input_sent")return;
  const match=/\bx=(-?\d+)\s+y=(-?\d+)/.exec(text);
  if(match){
    const videoRect=activeVideoRect(),screenRect=screenEl.getBoundingClientRect();
    const u=clamp(Number(match[1])/32767,0,1),v=clamp(Number(match[2])/32767,0,1);
    agentCursor.style.left=(videoRect.left-screenRect.left+u*videoRect.width)+"px";
    agentCursor.style.top=(videoRect.top-screenRect.top+v*videoRect.height)+"px";
    agentCursor.classList.add("show");
  }
  if(/\b(click|mousedown|mouseup)\b/i.test(text)){
    agentCursor.classList.remove("click");
    void agentCursor.offsetWidth;
    agentCursor.classList.add("click");
  }
}
function screenPoint(e){const r=activeVideoRect();const x=e.clientX-r.left,y=e.clientY-r.top;if(x<0||y<0||x>r.width||y>r.height)return null;return{x,y,u:r.width?x/r.width:.5,v:r.height?y/r.height:.5,rect:r}}
function setDirectEnabled(on){direct.enabled=!!on;localStorage.setItem("si_direct_click",direct.enabled?"1":"0");directClickToggle.checked=direct.enabled;directClickText.textContent=direct.enabled?"开启":"关闭"}
function absPoint(p){return{x:Math.round(clamp(p.u,0,1)*32767),y:Math.round(clamp(p.v,0,1)*32767)}}
function showHidGestureBlocked(){setKeyboardCaptureMsg("HID 正在连接，请重试","warn");if(hidGestureNoticeTimer)UI.lifecycle.clearTimer(hidGestureNoticeTimer);hidGestureNoticeTimer=UI.lifecycle.timeout(()=>{hidGestureNoticeTimer=0;if(keyboardCaptureMsg.textContent==="HID 正在连接，请重试")setKeyboardCaptureMsg(hidSocketIsCurrent()?"HID 已连接，请重试":"HID 未连接","warn")},1800)}
function beginHidGesture(){if(hidSocketIsCurrent())return true;showHidGestureBlocked();if(hidMayConnect())void API.connect();return false}
function directMoveTo(p){const a=absPoint(p);return API.send({type:"absmove",x:a.x,y:a.y})}
function syncDirectPointer(){if(direct.hover)directMoveTo(direct.hover);syncPointer.classList.add("active");setTimeout(()=>syncPointer.classList.remove("active"),220)}
function sendMove(dx,dy){const x=Math.round(clamp(dx,-127,127));const y=Math.round(clamp(dy,-127,127));return x||y?API.send({type:"mousemove",unit:"hid",dx:x,dy:y}):false}
function sendWheel(deltaY){if(!beginHidGesture())return false;return API.send({type:"wheel",deltaY:deltaY,deltaX:0})}
function sendClick(button=0){if(!beginHidGesture())return false;return API.send({type:"click",button})}
let inputGeneration=1;
const inputCancelers=new Set(),inputTimers=new Set();
function scheduleInputAction(fn,delay){const generation=inputGeneration,timer=setTimeout(()=>{inputTimers.delete(timer);if(generation===inputGeneration)fn()},delay);inputTimers.add(timer);return timer}
function cancelPendingInputActions(){inputGeneration++;for(const timer of inputTimers)clearTimeout(timer);inputTimers.clear();for(const cancel of inputCancelers)cancel()}
function sendDblClick(button=0){sendClick(button);scheduleInputAction(()=>sendClick(button),90)}
const textKeys={" ":"Space","\n":"Enter","\t":"Tab","a":"KeyA","b":"KeyB","c":"KeyC","d":"KeyD","e":"KeyE","f":"KeyF","g":"KeyG","h":"KeyH","i":"KeyI","j":"KeyJ","k":"KeyK","l":"KeyL","m":"KeyM","n":"KeyN","o":"KeyO","p":"KeyP","q":"KeyQ","r":"KeyR","s":"KeyS","t":"KeyT","u":"KeyU","v":"KeyV","w":"KeyW","x":"KeyX","y":"KeyY","z":"KeyZ","0":"Digit0","1":"Digit1","2":"Digit2","3":"Digit3","4":"Digit4","5":"Digit5","6":"Digit6","7":"Digit7","8":"Digit8","9":"Digit9","-":"Minus","=":"Equal","[":"BracketLeft","]":"BracketRight","\\":"Backslash",";":"Semicolon","'":"Quote","`":"Backquote",",":"Comma",".":"Period","/":"Slash"};
const shiftedTextKeys={"A":"KeyA","B":"KeyB","C":"KeyC","D":"KeyD","E":"KeyE","F":"KeyF","G":"KeyG","H":"KeyH","I":"KeyI","J":"KeyJ","K":"KeyK","L":"KeyL","M":"KeyM","N":"KeyN","O":"KeyO","P":"KeyP","Q":"KeyQ","R":"KeyR","S":"KeyS","T":"KeyT","U":"KeyU","V":"KeyV","W":"KeyW","X":"KeyX","Y":"KeyY","Z":"KeyZ","!":"Digit1","@":"Digit2","#":"Digit3","$":"Digit4","%":"Digit5","^":"Digit6","&":"Digit7","*":"Digit8","(":"Digit9",")":"Digit0","_":"Minus","+":"Equal","{":"BracketLeft","}":"BracketRight","|":"Backslash",":":"Semicolon","\"":"Quote","~":"Backquote","<":"Comma",">":"Period","?":"Slash"};
function wait(ms){return new Promise(resolve=>setTimeout(resolve,ms))}
async function tapCode(code,mods=[],generation=inputGeneration){if(generation!==inputGeneration||!beginHidGesture())return false;for(const mod of mods)API.send({type:"keydown",code:mod});if(!API.send({type:"keydown",code}))return false;await wait(5);if(generation!==inputGeneration)return false;API.send({type:"keyup",code});for(let i=mods.length-1;i>=0;i--)API.send({type:"keyup",code:mods[i]});await wait(6);return generation===inputGeneration}
async function typeText(value,generation=inputGeneration){const text=String(value||"").replace(/\r\n/g,"\n").replace(/\r/g,"\n");let sent=0,skipped=0;for(const ch of text){if(generation!==inputGeneration)break;if(textKeys[ch]){if(await tapCode(textKeys[ch],[],generation))sent++;continue}if(shiftedTextKeys[ch]){if(await tapCode(shiftedTextKeys[ch],["ShiftLeft"],generation))sent++;continue}if(ch==="\b"){if(await tapCode("Backspace",[],generation))sent++;continue}skipped++}return{sent,skipped}}
const modifierCodes=new Set(["ControlLeft","ControlRight","ShiftLeft","ShiftRight","AltLeft","AltRight","MetaLeft","MetaRight"]);
const keyboardCapture={enabled:false,pendingMods:new Set(),pressed:new Set(),pastePending:false,pasteTimer:0,pasting:false};
function keyboardWsReady(){return hidSocketIsCurrent()}
function setKeyboardCaptureMsg(text,kind=""){keyboardCaptureMsg.textContent=text||"";keyboardCaptureMsg.className="capture-msg "+kind}
function isCaptureEditableTarget(e){const t=e.target;if(document.getElementById("authModal")?.classList.contains("show"))return true;if(!t||!t.closest)return false;return !!(t.closest("input,textarea,select")||t.isContentEditable)}
function isLocalPasteShortcut(e){return e.code==="KeyV"&&(e.ctrlKey||e.metaKey)&&!e.altKey}
function releaseKeyboardCaptureKeys(){const keys=Array.from(keyboardCapture.pressed).reverse();keyboardCapture.pendingMods.clear();for(const code of keys)API.send({type:"keyup",code});keyboardCapture.pressed.clear()}
function flushPendingModifiers(){for(const code of Array.from(keyboardCapture.pendingMods)){if(!keyboardCapture.pressed.has(code)){API.send({type:"keydown",code});keyboardCapture.pressed.add(code)}keyboardCapture.pendingMods.delete(code)}}
function updateKeyboardCaptureState(){keyboardPassthrough.checked=keyboardCapture.enabled;keyboardPassthroughText.textContent=keyboardCapture.enabled?"开启":"关闭";document.body.classList.toggle("keyboard-capture",keyboardCapture.enabled)}
function setKeyboardCapture(on){keyboardCapture.enabled=!!on;updateKeyboardCaptureState();if(keyboardCapture.enabled){screenPad.focus({preventScroll:true});setKeyboardCaptureMsg(keyboardWsReady()?"捕获中":"HID 未连接",keyboardWsReady()?"ok":"bad")}else{releaseKeyboardCaptureKeys();setKeyboardCaptureMsg("待机")}}
function syncKeyboardCaptureConnection(){const ready=keyboardWsReady();if(!keyboardCapture.enabled){if(ready&&keyboardCaptureMsg.textContent==="HID 正在连接，请重试")setKeyboardCaptureMsg("HID 已连接，请重试","ok");return}if(ready&&(keyboardCaptureMsg.textContent==="HID 未连接"||keyboardCaptureMsg.textContent==="HID 正在连接，请重试"))setKeyboardCaptureMsg("捕获中","ok");else if(!ready&&keyboardCaptureMsg.textContent!=="HID 未连接"&&keyboardCaptureMsg.textContent!=="HID 正在连接，请重试")setKeyboardCaptureMsg("HID 未连接","bad")}
function restoreKeyboardCaptureAfterFocus(){if(!keyboardCapture.enabled){setKeyboardCaptureMsg("待机");return}const ready=keyboardWsReady();setKeyboardCaptureMsg(ready?"捕获中":"HID 未连接",ready?"ok":"bad")}
UI.lifecycle.interval(syncKeyboardCaptureConnection,500);
function releaseKeyboardCapture(reason=""){releaseKeyboardCaptureKeys();keyboardCapture.pastePending=false;clearTimeout(keyboardCapture.pasteTimer);if(keyboardCapture.enabled&&reason)setKeyboardCaptureMsg(reason,"warn")}
async function sendLocalClipboardText(text,generation=inputGeneration){if(generation!==inputGeneration)return;keyboardCapture.pastePending=false;clearTimeout(keyboardCapture.pasteTimer);releaseKeyboardCaptureKeys();const value=String(text||"");if(!value){setKeyboardCaptureMsg("剪贴板为空","warn");return}if(keyboardCapture.pasting)return;keyboardCapture.pasting=true;setKeyboardCaptureMsg("粘贴中","warn");try{const r=await typeText(value,generation);if(generation===inputGeneration)setKeyboardCaptureMsg(r.skipped?`已输入 ${r.sent} 跳过 ${r.skipped}`:`已输入 ${r.sent}`,"ok")}catch(err){if(generation===inputGeneration)setKeyboardCaptureMsg(err.message||"粘贴失败","bad")}finally{keyboardCapture.pasting=false}}
async function readClipboardAfterShortcut(){if(!keyboardCapture.pastePending)return;const generation=inputGeneration;keyboardCapture.pastePending=false;if(navigator.clipboard&&navigator.clipboard.readText){try{const text=await navigator.clipboard.readText();if(generation===inputGeneration)await sendLocalClipboardText(text,generation);return}catch(e){}}if(generation!==inputGeneration)return;releaseKeyboardCaptureKeys();setKeyboardCaptureMsg("剪贴板不可用","warn")}
function handleKeyboardCaptureKeydown(e){if(!keyboardCapture.enabled||isCaptureEditableTarget(e))return;if(keyboardCapture.pasting){e.preventDefault();e.stopImmediatePropagation();return}if(isLocalPasteShortcut(e)){keyboardCapture.pastePending=true;clearTimeout(keyboardCapture.pasteTimer);keyboardCapture.pasteTimer=setTimeout(readClipboardAfterShortcut,80);e.stopImmediatePropagation();return}e.preventDefault();e.stopImmediatePropagation();const code=e.code;if(!code)return;if(!keyboardWsReady()){setKeyboardCaptureMsg("HID 未连接","bad");return}if(modifierCodes.has(code)){if(!keyboardCapture.pressed.has(code))keyboardCapture.pendingMods.add(code);setKeyboardCaptureMsg("捕获中","ok");return}flushPendingModifiers();if(e.repeat&&keyboardCapture.pressed.has(code))return;if(!keyboardCapture.pressed.has(code)){keyboardCapture.pressed.add(code);API.send({type:"keydown",code})}setKeyboardCaptureMsg("捕获中","ok")}
function handleKeyboardCaptureKeyup(e){if(!keyboardCapture.enabled||isCaptureEditableTarget(e))return;e.preventDefault();e.stopImmediatePropagation();const code=e.code;if(!code)return;if(modifierCodes.has(code)&&keyboardCapture.pendingMods.delete(code)){setKeyboardCaptureMsg("捕获中","ok");return}if(keyboardCapture.pressed.has(code)){API.send({type:"keyup",code});keyboardCapture.pressed.delete(code)}setKeyboardCaptureMsg("捕获中","ok")}
function handleKeyboardCapturePaste(e){if(!keyboardCapture.enabled||isCaptureEditableTarget(e))return;e.preventDefault();e.stopImmediatePropagation();const text=e.clipboardData?e.clipboardData.getData("text/plain"):"";sendLocalClipboardText(text)}
keyboardPassthrough.onchange=()=>{localStorage.setItem("si_keyboard_capture",keyboardPassthrough.checked?"1":"0");setKeyboardCapture(keyboardPassthrough.checked)};
document.addEventListener("keydown",handleKeyboardCaptureKeydown,true);
document.addEventListener("keyup",handleKeyboardCaptureKeyup,true);
document.addEventListener("paste",handleKeyboardCapturePaste,true);
window.addEventListener("blur",()=>releaseKeyboardCapture("窗口失焦"));
document.addEventListener("visibilitychange",()=>{if(document.hidden)releaseKeyboardCapture("页面后台")});
UI.lifecycle.addCleanup(()=>{releaseKeyboardCapture("");releaseHoldKeys()});
setKeyboardCapture(localStorage.getItem("si_keyboard_capture")!=="0");
function stopDirectDrag(e,sendUp=true){if(!direct.drag)return;const p=e?screenPoint(e):null;if(p)directMoveTo(p);if(sendUp)API.send({type:"absmouseup",button:direct.button});try{screenPad.releasePointerCapture(direct.pointer)}catch(err){}direct.drag=false;direct.pointer=null}
setDirectEnabled(direct.enabled);
directClickToggle.onchange=()=>setDirectEnabled(directClickToggle.checked);
syncPointer.onclick=syncDirectPointer;
screenPad.addEventListener("mouseenter",()=>screenPad.focus({preventScroll:true}));
screenPad.addEventListener("pointerdown",e=>{e.preventDefault();screenPad.focus({preventScroll:true});if(!direct.enabled||e.button>2)return;const p=screenPoint(e);if(!p||!beginHidGesture())return;direct.drag=true;direct.pointer=e.pointerId;direct.button=e.button;screenPad.setPointerCapture(e.pointerId);if(!directMoveTo(p)||!API.send({type:"absmousedown",button:e.button}))stopDirectDrag(null,false)});
screenPad.addEventListener("pointermove",e=>{const r=screenEl.getBoundingClientRect();cursor.style.left=e.clientX-r.left+"px";cursor.style.top=e.clientY-r.top+"px";cursor.style.opacity=1;const p=screenPoint(e);if(p)direct.hover={u:p.u,v:p.v};if(direct.enabled&&direct.drag&&e.pointerId===direct.pointer&&p){e.preventDefault();directMoveTo(p)}});
screenPad.addEventListener("pointerup",e=>{if(direct.drag&&e.pointerId===direct.pointer){e.preventDefault();stopDirectDrag(e,true)}});
screenPad.addEventListener("pointercancel",e=>{if(direct.drag&&e.pointerId===direct.pointer){e.preventDefault();stopDirectDrag(e,true)}});
screenPad.addEventListener("pointerleave",()=>{if(!direct.drag)cursor.style.opacity=0});
screenPad.addEventListener("wheel",e=>{e.preventDefault();if(direct.enabled)sendWheel(Math.sign(e.deltaY)*-1)},{passive:false});
screenPad.addEventListener("contextmenu",e=>e.preventDefault());
const joystick=$("joystick"),joyThumb=$("joyThumb");
const joy={active:false,pointer:null,x:0,y:0,timer:0};
function setJoy(x,y){joy.x=x;joy.y=y;joyThumb.style.left=(50+x*34)+"%";joyThumb.style.top=(50+y*34)+"%"}
function updateJoy(e){const r=joystick.getBoundingClientRect();const radius=Math.min(r.width,r.height)*0.38;let x=(e.clientX-r.left-r.width/2)/radius;let y=(e.clientY-r.top-r.height/2)/radius;const mag=Math.hypot(x,y);if(mag>1){x/=mag;y/=mag}setJoy(x,y)}
function stopJoy(){if(joy.pointer!==null){try{joystick.releasePointerCapture(joy.pointer)}catch(e){}}joy.active=false;joy.pointer=null;setJoy(0,0);UI.lifecycle.clearTimer(joy.timer);joy.timer=0}
joystick.addEventListener("pointerdown",e=>{e.preventDefault();joystick.focus();if(!beginHidGesture())return;joy.active=true;joy.pointer=e.pointerId;joystick.setPointerCapture(e.pointerId);updateJoy(e);if(!joy.timer)joy.timer=UI.lifecycle.interval(()=>sendMove(joy.x*speed()*4,joy.y*speed()*4),32)});
joystick.addEventListener("pointermove",e=>{if(joy.active&&e.pointerId===joy.pointer)updateJoy(e)});
joystick.addEventListener("pointerup",stopJoy);joystick.addEventListener("pointercancel",stopJoy);joystick.addEventListener("contextmenu",e=>e.preventDefault());
const touchPad=$("touchPad"),touchDot=$("touchDot");
const touch={active:false,pointer:null,lastX:0,lastY:0,startX:0,startY:0,moved:0,startAt:0,locked:false,centered:false};
function setTouchDot(e,visible=true){const r=touchPad.getBoundingClientRect();touchDot.style.left=e.clientX-r.left+"px";touchDot.style.top=e.clientY-r.top+"px";touchDot.style.opacity=visible?1:0}
function setTouchCenter(visible=true){const r=touchPad.getBoundingClientRect();touchDot.style.left=r.width/2+"px";touchDot.style.top=r.height/2+"px";touchDot.style.opacity=visible?1:0}
function requestTouchLock(e){if(e.pointerType!=="mouse"||!touchPad.requestPointerLock)return false;setTouchCenter(true);try{const ret=touchPad.requestPointerLock();if(ret&&ret.catch)ret.catch(()=>{if(touch.active&&!touch.locked){try{touchPad.setPointerCapture(touch.pointer)}catch(err){}setTouchDot({clientX:touch.lastX,clientY:touch.lastY},true)}});return true}catch(err){return false}}
function stopTouch(e,allowTap=true){if(!touch.active)return;const wasTap=allowTap&&touch.moved<7&&(Date.now()-touch.startAt)<320;if(document.pointerLockElement===touchPad&&document.exitPointerLock)document.exitPointerLock();if(e&&!touch.locked&&!touch.centered)setTouchDot(e,false);else setTouchCenter(false);try{touchPad.releasePointerCapture(touch.pointer)}catch(err){}touch.active=false;touch.pointer=null;touch.locked=false;touch.centered=false;touchPad.classList.remove("locked");document.body.classList.remove("touch-capture");if(wasTap)sendClick(0)}
touchPad.addEventListener("pointerdown",e=>{if(e.pointerType==="mouse"&&e.button!==0)return;e.preventDefault();touchPad.focus();if(!beginHidGesture())return;touch.active=true;touch.pointer=e.pointerId;touch.lastX=e.clientX;touch.lastY=e.clientY;touch.startX=e.clientX;touch.startY=e.clientY;touch.moved=0;touch.startAt=Date.now();touch.centered=e.pointerType==="mouse";document.body.classList.toggle("touch-capture",touch.centered);if(!requestTouchLock(e)){touchPad.setPointerCapture(e.pointerId);touch.centered?setTouchCenter(true):setTouchDot(e,true)}});
touchPad.addEventListener("pointermove",e=>{if(!touch.active||touch.locked||e.pointerId!==touch.pointer)return;e.preventDefault();const dx=e.clientX-touch.lastX;const dy=e.clientY-touch.lastY;touch.lastX=e.clientX;touch.lastY=e.clientY;touch.moved=Math.max(touch.moved,Math.hypot(e.clientX-touch.startX,e.clientY-touch.startY));touch.centered?setTouchCenter(true):setTouchDot(e,true);sendMove(dx*speed()*0.55,dy*speed()*0.55)});
touchPad.addEventListener("pointerup",stopTouch);touchPad.addEventListener("pointercancel",stopTouch);touchPad.addEventListener("wheel",e=>{e.preventDefault();sendWheel(Math.sign(e.deltaY)*-1)},{passive:false});touchPad.addEventListener("contextmenu",e=>e.preventDefault());
document.addEventListener("pointerlockchange",()=>{const locked=document.pointerLockElement===touchPad;const wasLocked=touch.locked;touch.locked=locked;touchPad.classList.toggle("locked",locked);if(locked)setTouchCenter(true);else if(wasLocked&&touch.active)stopTouch(null,false)});
document.addEventListener("mousemove",e=>{if(!touch.active||document.pointerLockElement!==touchPad)return;e.preventDefault();const dx=e.movementX||0,dy=e.movementY||0;touch.moved+=Math.hypot(dx,dy);setTouchCenter(true);sendMove(dx*speed()*0.55,dy*speed()*0.55)});
document.addEventListener("mouseup",e=>{if(touch.active&&document.pointerLockElement===touchPad&&e.button===0){e.preventDefault();stopTouch(e)}});
function setMouseMode(mode){modeJoy.classList.toggle("active",mode==="joy");modeTouch.classList.toggle("active",mode==="touch");joyPane.classList.toggle("hide",mode!=="joy");touchPane.classList.toggle("hide",mode!=="touch");(mode==="joy"?joystick:touchPad).focus()}
modeJoy.onclick=()=>setMouseMode("joy");modeTouch.onclick=()=>setMouseMode("touch");mouseSpeed.oninput=()=>speedText.textContent=mouseSpeed.value;
document.querySelectorAll("[data-mouse-button]").forEach(b=>{let held=false;const button=Number(b.dataset.mouseButton);const up=(send=true)=>{if(!held)return;held=false;b.classList.remove("active");if(send)API.send({type:"mouseup",button})};const normalUp=()=>up(true);inputCancelers.add(()=>up(false));b.addEventListener("pointerdown",e=>{e.preventDefault();if(!beginHidGesture())return;held=true;b.classList.add("active");b.setPointerCapture(e.pointerId);if(!API.send({type:"mousedown",button}))up(false)});b.addEventListener("pointerup",normalUp);b.addEventListener("pointercancel",normalUp);b.addEventListener("pointerleave",normalUp)});
document.querySelectorAll("[data-mouse-click]").forEach(b=>b.onclick=()=>sendClick(Number(b.dataset.mouseClick)));
document.querySelectorAll("[data-mouse-dblclick]").forEach(b=>b.onclick=()=>sendDblClick(Number(b.dataset.mouseDblclick)));
document.querySelectorAll("[data-wheel]").forEach(b=>{let timer=0;const step=()=>sendWheel(Number(b.dataset.wheel));const stop=()=>{UI.lifecycle.clearTimer(timer);timer=0;b.classList.remove("active")};inputCancelers.add(stop);b.addEventListener("pointerdown",e=>{e.preventDefault();if(!beginHidGesture())return;b.classList.add("active");b.setPointerCapture(e.pointerId);step();timer=UI.lifecycle.interval(step,90)});b.addEventListener("pointerup",stop);b.addEventListener("pointercancel",stop);b.addEventListener("pointerleave",stop)});
dragLock.onclick=()=>{const active=!dragLock.classList.contains("active");if(active&&!beginHidGesture())return;dragLock.classList.toggle("active",active);if(!API.send({type:active?"mousedown":"mouseup",button:0}))dragLock.classList.remove("active")};
function sendHidReleaseAllOnCurrentSocket(){const ws=API.ws;if(!ws||ws.readyState!==WebSocket.OPEN||hidSocketStreamId!==videoStreamId)return false;try{ws.send(JSON.stringify({type:"releaseall"}));return true}catch(e){return false}}
function releaseAll(){cancelPendingInputActions();stopJoy();stopTouch(null,false);stopDirectDrag(null,false);releaseKeyboardCaptureKeys();releaseHoldKeys();dragLock.classList.remove("active");API.send({type:"releaseall"})}
function releaseAllForHidden(){releaseAll();sendHidReleaseAllOnCurrentSocket()}
release.onclick=()=>{stopBootKeyLoop("已手动关闭");releaseAll()};
function acceptsKeyboard(){return !keyboardCapture.enabled&&(document.activeElement===screenPad||document.activeElement===joystick||document.activeElement===touchPad)}
document.addEventListener("keydown",e=>{if(!acceptsKeyboard())return;e.preventDefault();API.send({type:"keydown",code:e.code})});
document.addEventListener("keyup",e=>{if(!acceptsKeyboard())return;e.preventDefault();API.send({type:"keyup",code:e.code})});
const combos={"ctrl-alt-del":{modifiers:["ControlLeft","AltLeft"],keys:["Delete"]},"alt-tab":{modifiers:["AltLeft"],keys:["Tab"]},"alt-f4":{modifiers:["AltLeft"],keys:["F4"]}};
document.querySelectorAll("[data-combo]").forEach(b=>b.onclick=()=>API.send({type:"combo",...combos[b.dataset.combo]}));
const heldKeys=new Set();
function setHeldShortcut(button,on){const code=button.dataset.shortcutKey;if(!code)return;const active=!!on;if(active===heldKeys.has(code)||active&&!beginHidGesture())return;if(active){heldKeys.add(code);button.classList.add("active");if(!API.send({type:"keydown",code})){heldKeys.delete(code);button.classList.remove("active")}}else{heldKeys.delete(code);button.classList.remove("active");API.send({type:"keyup",code})}}
function releaseHoldKeys(){document.querySelectorAll("[data-shortcut-key]").forEach(b=>{if(heldKeys.has(b.dataset.shortcutKey))setHeldShortcut(b,false)});for(const code of Array.from(heldKeys)){API.send({type:"keyup",code});heldKeys.delete(code)}}
async function tapShortcutKey(button){const code=button.dataset.shortcutKey;if(!code||heldKeys.has(code))return;button.classList.add("active");await tapCode(code);if(!heldKeys.has(code))button.classList.remove("active")}
const shortcutClickTimers=new WeakMap();
document.querySelectorAll("[data-shortcut-key]").forEach(b=>{b.addEventListener("click",e=>{e.preventDefault();if(e.detail>1)return;clearTimeout(shortcutClickTimers.get(b));shortcutClickTimers.set(b,setTimeout(()=>tapShortcutKey(b),220))});b.addEventListener("dblclick",e=>{e.preventDefault();clearTimeout(shortcutClickTimers.get(b));setHeldShortcut(b,!heldKeys.has(b.dataset.shortcutKey))})});
const layout=[["Escape","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"],["Backquote","Digit1","Digit2","Digit3","Digit4","Digit5","Digit6","Digit7","Digit8","Digit9","Digit0","Backspace","Delete"],["Tab","KeyQ","KeyW","KeyE","KeyR","KeyT","KeyY","KeyU","KeyI","KeyO","KeyP"],["CapsLock","KeyA","KeyS","KeyD","KeyF","KeyG","KeyH","KeyJ","KeyK","KeyL","Enter"],["ShiftLeft","KeyZ","KeyX","KeyC","KeyV","KeyB","KeyN","KeyM","ArrowUp","ShiftRight"],["ControlLeft","MetaLeft","AltLeft","Space","ArrowLeft","ArrowDown","ArrowRight"]];
const labels={Escape:"Esc",Backspace:"Back",Delete:"Del",CapsLock:"Caps",ShiftLeft:"Shift",ShiftRight:"Shift",ControlLeft:"Ctrl",MetaLeft:"Win",AltLeft:"Alt",Space:"Space",ArrowLeft:"Left",ArrowRight:"Right",ArrowUp:"Up",ArrowDown:"Down"};
keyboard.innerHTML=layout.map(row=>`<div class="keyrow">${row.map(code=>`<button class="key ${code==="Space"?"wide":""}" data-code="${code}">${labels[code]||code.replace("Key","").replace("Digit","")}</button>`).join("")}</div>`).join("");
keyboard.querySelectorAll("button").forEach(b=>{b.onmousedown=e=>{e.preventDefault();if(!beginHidGesture())return;b.classList.add("active");if(!API.send({type:"keydown",code:b.dataset.code}))b.classList.remove("active")};b.onmouseup=e=>{e.preventDefault();if(!b.classList.contains("active"))return;b.classList.remove("active");API.send({type:"keyup",code:b.dataset.code})};b.onmouseleave=()=>{if(!b.classList.contains("active"))return;b.classList.remove("active");API.send({type:"keyup",code:b.dataset.code})}});

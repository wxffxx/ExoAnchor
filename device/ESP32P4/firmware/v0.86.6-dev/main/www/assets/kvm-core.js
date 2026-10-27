/* ExoAnchor KVM page module. Loaded as a classic script in a fixed order after
 * ui-core.js and ui-shell.js; the modules share one global lexical scope, so
 * declarations here are visible to the modules loaded after it. Order:
 * kvm-core -> kvm-video -> kvm-lease -> kvm-modes -> kvm-input -> kvm-status.
 * DOM references, shared page state, the HID WebSocket owner and the rail/drawer (mouse-keyboard tool, shortcuts, settings).
 */
const $=id=>document.getElementById(id);
const screenEl=$("screen"),screenPad=$("screenPad"),cursor=$("cursor"),video=$("video"),h264Video=$("h264Video"),h264MseVideo=$("h264MseVideo"),nosignal=$("nosignal"),agentCursor=$("agentCursor"),agentActionToast=$("agentActionToast"),streamStats=$("streamStats"),videoModeChip=$("videoModeChip");
const keyboardPassthrough=$("keyboardPassthrough"),keyboardPassthroughText=$("keyboardPassthroughText"),keyboardCaptureMsg=$("keyboardCaptureMsg"),directClickToggle=$("directClickToggle"),directClickText=$("directClickText"),focusRefreshToggle=$("focusRefreshToggle"),focusRefreshText=$("focusRefreshText"),videoPacingToggle=$("videoPacingToggle"),videoPacingText=$("videoPacingText"),resolutionSelect=$("resolutionSelect"),outputModeField=$("outputModeField"),outputModeSelect=$("outputModeSelect");
const accessModeSelect=$("accessModeSelect"),accessModeDetail=$("accessModeDetail"),qualitySelect=$("qualitySelect"),fallbackMjpeg=$("fallbackMjpeg");
const drawer=$("drawer"),drawerTitle=$("drawerTitle"),controlPopupToggle=$("controlPopupToggle"),shortcutToggle=$("shortcutToggle"),controlPopupClose=$("controlPopupClose"),toolMouseTab=$("toolMouseTab"),toolKeyboardTab=$("toolKeyboardTab"),mouseTool=$("mouseTool"),keyboardTool=$("keyboardTool");
const agentTakeoverBanner=$("agentTakeoverBanner"),agentPresenceTitle=$("agentPresenceTitle"),agentTakeoverDetail=$("agentTakeoverDetail"),reclaimKvm=$("reclaimKvm"),bootKeyLoopKey=$("bootKeyLoopKey"),bootKeyLoopToggle=$("bootKeyLoopToggle"),bootKeyLoopText=$("bootKeyLoopText"),bootKeyLoopState=$("bootKeyLoopState");
let kvmReadOnly=false,automatedControllerLabel="Agent/MCP";
let kvmPageActive=!document.hidden;
let kvmLeaving=false,kvmWindowBlurred=false;
let videoPeerBlocked=false;
let msVideoPowerKnown=false,msVideoPowerOff=false;
const UI=window.ExoAnchorUI,API=UI.api,Session=UI.session,AuthUI=UI.auth;
UI.lifecycle.addCleanup(UI.accessMode.bind(
  accessModeSelect, accessModeDetail,
  error=>{keyboardCaptureMsg.textContent=error?.message||"权限模式保存失败";keyboardCaptureMsg.className="capture-msg bad"}
));
let hidReconnectTimer=0,hidConnectGeneration=0,hidConnectPendingGeneration=-1,hidSocketStreamId=0,hidFocusBlocked=false,hidGestureNoticeTimer=0;
function hidMayConnect(){return kvmPageActive&&!kvmLeaving&&!kvmReadOnly&&!videoPeerBlocked&&!msVideoPowerOff&&videoLeaseHeld&&!hidFocusBlocked&&!document.hidden}
function hidSocketIsCurrent(){return !!API.ws&&API.ws.readyState===WebSocket.OPEN&&hidSocketStreamId===videoStreamId&&hidMayConnect()}
API.send=function(o){if(kvmReadOnly||!Session.active())return false;Session.touch();if(!hidSocketIsCurrent()){if(hidMayConnect())void this.connect();return false}try{this.ws.send(JSON.stringify(o));return true}catch(e){disconnectHidWs();scheduleHidReconnect();return false}};
function clearHidReconnect(){if(hidReconnectTimer)UI.lifecycle.clearTimer(hidReconnectTimer);hidReconnectTimer=0}
function scheduleHidReconnect(delay=API.retry){if(hidReconnectTimer||!hidMayConnect())return;hidReconnectTimer=UI.lifecycle.timeout(()=>{hidReconnectTimer=0;if(hidMayConnect())void API.connect()},delay)}
API.connect=async function(){if(!hidMayConnect())return;clearHidReconnect();const generation=hidConnectGeneration,streamId=videoStreamId;if(hidConnectPendingGeneration===generation)return;hidConnectPendingGeneration=generation;try{if(!(await this.ensureToken())){if(generation===hidConnectGeneration)scheduleHidReconnect();return}if(generation!==hidConnectGeneration||streamId!==videoStreamId||!hidMayConnect()||this.ws?.readyState<=1)return;const proto=location.protocol==="https:"?"wss:":"ws:",ws=new WebSocket(proto+"//"+location.host+"/api/ws/hid?stream_id="+streamId);this.ws=ws;ws.onopen=()=>{if(this.ws!==ws||generation!==hidConnectGeneration||streamId!==videoStreamId||!hidMayConnect()){if(this.ws===ws)this.ws=null;try{ws.close()}catch(e){}return}hidSocketStreamId=streamId;this.retry=1000;clearHidReconnect();syncKeyboardCaptureConnection()};ws.onclose=()=>{if(this.ws===ws){this.ws=null;hidSocketStreamId=0}syncKeyboardCaptureConnection();if(generation===hidConnectGeneration&&streamId===videoStreamId&&hidMayConnect()){scheduleHidReconnect(this.retry);this.retry=Math.min(this.retry*1.5,10000)}};ws.onerror=syncKeyboardCaptureConnection}finally{if(hidConnectPendingGeneration===generation)hidConnectPendingGeneration=-1}};
function disconnectHidWs(){hidConnectGeneration++;clearHidReconnect();const ws=API.ws;API.ws=null;hidSocketStreamId=0;if(ws){ws.onclose=null;try{ws.close()}catch(e){}}syncKeyboardCaptureConnection()}
AuthUI.bindForm(async()=>{await Session.load();await UI.accessMode.load();if(videoLeaseHeld&&!kvmReadOnly){if(!API.ws||API.ws.readyState>1)API.connect()}else void startKvmLease()});
function fmtBytes(n){if(!n)return"--";if(n>1048576)return Math.round(n/1048576)+" MB";return Math.round(n/1024)+" KB"}
function setToolPanel(panel){const mouse=panel!=="keyboard";toolMouseTab.classList.toggle("active",mouse);toolKeyboardTab.classList.toggle("active",!mouse);mouseTool.classList.toggle("hide",!mouse);keyboardTool.classList.toggle("hide",mouse);if(mouse)touchPad.focus({preventScroll:true})}
/* Drawer: one panel at a time (tool = mouse/soft keyboard, shortcut, settings); the rail button of the open panel is highlighted. Closing the tool panel releases any held virtual mouse state. */
const drawerPanels={tool:[$("toolPanel"),"鼠标 / 软键盘"],shortcut:[$("shortcutPanel"),"快捷键"],settings:[$("settingsPanel"),"设置"]},railButtons=Array.from(document.querySelectorAll(".rail [data-drawer]"));
let openDrawer="";
function closeMouseTool(){stopJoy();stopTouch(null,false);if(dragLock.classList.contains("active")){dragLock.classList.remove("active");API.send({type:"mouseup",button:0})}}
function setDrawer(name){name=drawerPanels[name]?name:"";if(openDrawer==="tool"&&name!=="tool")closeMouseTool();openDrawer=name;drawer.classList.toggle("open",!!name);drawer.setAttribute("aria-hidden",name?"false":"true");document.body.classList.toggle("drawer-open",!!name);for(const key in drawerPanels)drawerPanels[key][0].classList.toggle("hide",key!==name);railButtons.forEach(b=>b.classList.toggle("active",b.dataset.drawer===name));if(name)drawerTitle.textContent=drawerPanels[name][1];if(name==="tool")setToolPanel(mouseTool.classList.contains("hide")?"keyboard":"mouse")}
function setControlWindow(open){if(open)setDrawer("tool");else if(openDrawer==="tool")setDrawer("")}
railButtons.forEach(b=>b.onclick=()=>setDrawer(openDrawer===b.dataset.drawer?"":b.dataset.drawer));
controlPopupClose.onclick=()=>setDrawer("");
/* The status-bar VIDEO chip is the shortcut to the quality selector. */videoModeChip.onclick=()=>{setDrawer("settings");qualitySelect.focus({preventScroll:true})};
toolMouseTab.onclick=()=>setToolPanel("mouse");
toolKeyboardTab.onclick=()=>setToolPanel("keyboard");

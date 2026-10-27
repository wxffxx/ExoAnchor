/* ExoAnchor KVM page module. Loaded as a classic script in a fixed order after
 * ui-core.js and ui-shell.js; the modules share one global lexical scope, so
 * declarations here are visible to the modules loaded after it. Order:
 * kvm-core -> kvm-video -> kvm-lease -> kvm-modes -> kvm-input -> kvm-status.
 * Status rendering: Agent presence, the status bar (capture mode / stream statistics), video availability copy and the boot-key loop.
 */
function renderAgentPresence(agentViewing,inputControlActive,controller,browserHealthy){
  const visible=agentViewing||inputControlActive;
  agentTakeoverBanner.classList.toggle("hide",!visible);
  agentTakeoverBanner.classList.toggle("takeover",inputControlActive);
  reclaimKvm.hidden=!inputControlActive;
  if(!visible)return;
  agentPresenceTitle.textContent=inputControlActive?`${controller} 正在控制输入`:"Agent 正在查看画面";
  agentTakeoverDetail.textContent=inputControlActive?`KVM 视频继续共享 · ${browserHealthy?"画面已同步":"画面恢复中"} · 可随时终止自动控制`:`只读观察 · 视频共享 · ${browserHealthy?"画面已同步":"画面恢复中"} · 人工输入不受影响`;
}
function renderVideoModeChip(v,live){const kvm=v.control?.kvm_mode||{},w=Number(kvm.width||v.width||0),h=Number(kvm.height||v.height||0),fps=Number(kvm.fps_x100||0)/100,output=selectedOutputMode==="h264"?"H.264":"MJPEG";videoModeChip.textContent=msVideoPowerOff?"OFF":w&&h?`${w}×${h}${fps?` @ ${fps%1?fps.toFixed(1):fps.toFixed(0)}`:""} · ${output}`:output;videoModeChip.title=`画质设置 · 当前 ${videoModeChip.textContent}`;videoModeChip.classList.toggle("live",!!live&&!msVideoPowerOff)}
function videoUnavailableReason(v,agentTakeover){const reason=v.last_error?(/open UVC stream|waiting for USB UVC/i.test(v.last_error)&&tintRecentColdStart()?"采集卡正在重新枚举（断电重启后最长约 15 秒）":`视频采集异常：${v.last_error}`):Array.isArray(v.modes)&&v.modes.length?"采集器已识别，等待有效视频信号":"未检测到视频采集器或输入信号";return agentTakeover?`Agent 正在控制输入；${reason}`:reason}
function renderKvmStatus(s){
  if(!s)return;
  const v=s.video||{},vc=v.control||{},lease=s.control_lease||{};
  const coldStartActive=renderColdStart(s);
  if(!syncMsVideoPowerState(s)){if(coldStartActive)videoText.textContent=`采集卡冷启动中，${tintColdSeconds} s 后自动上电`;renderVideoModeChip(v,false);renderResolution(v);renderQualitySelect();fallbackMjpeg.hidden=true;return}
  const agentViewing=!!vc.agent_active,controller=lease.agent_owner==="mcp"?"MCP":"Agent";
  const hasInputControlState=typeof lease.input_control_active==="boolean";
  const inputControlActive=hasInputControlState?lease.input_control_active===true:(lease.agent_active===true&&lease.mode!=="observe");
  const frameAge=Number(v.last_frame_ms||0);
  const metrics=v.stream_metrics||{},metricsValid=!!metrics.valid&&Number(metrics.fps)>0&&Number(metrics.stream_id)===videoStreamId;
  const rttMs=Number(UI.status.lastDurationMs),rttText=Number.isFinite(rttMs)&&rttMs>=0?`${Math.round(rttMs)} ms`:"-- ms";
  /* Latency split: "设备" is UVC delivery -> send complete on the firmware (H.264 chain metrics or MJPEG stream metrics), "上屏" is WebSocket arrival -> paint in this browser (H.264 only; a multipart <img> exposes no per-frame hook). */
  const chain=v.h264_runtime?.chain_metrics||{},deviceAvgUs=Number(selectedOutputMode==="h264"?chain.latency_avg_us||0:metrics.latency_avg_us||0),deviceMaxUs=Number(selectedOutputMode==="h264"?chain.latency_max_us||0:metrics.latency_max_us||0),paint=typeof videoPaintLatency==="object"&&videoPaintLatency?videoPaintLatency:{valid:false};
  const catchUps=typeof h264MseCatchUps==="number"&&h264MseCatchUps>0?` · 追赶 ${h264MseCatchUps}`:"";
  const latencyText=`设备 ${deviceAvgUs>0?Math.round(deviceAvgUs/1000)+"/"+Math.round(deviceMaxUs/1000):"--"} ms · 上屏 ${paint.valid?Math.round(paint.avg)+"/"+Math.round(paint.max):"--"} ms${catchUps}`;
  streamStats.textContent=metricsValid?`STREAM ${Number(metrics.fps).toFixed(1)} FPS · ${Number(metrics.bitrate_mbps||0).toFixed(2)} Mb/s · RTT ${rttText} · ${latencyText}`:"STREAM -- FPS · -- Mb/s · RTT -- ms";
  streamStats.classList.toggle("stale",!metricsValid);
  const backendHealthy=!!v.frame_ready&&Number(v.fps||0)>0&&(frameAge===0||frameAge<1500);
  /* A multipart <img> keeps showing its last frame after the device ends the stream, so MJPEG health also needs this stream to be receiving frames per the backend egress metrics. */const browserHealthy=browserVideoReady()&&(selectedOutputMode==="h264"||!videoStreamReady||metricsValid);
  backendVideoHealthy=backendHealthy;
  if(browserHealthy)videoStreamReady=true;
  renderVideoModeChip(v,browserHealthy);
  renderResolution(v);
  renderQualitySelect();
  fallbackMjpeg.hidden=!(selectedOutputMode==="h264"&&!!outputFailureMessage&&!videoModeSwitching&&!kvmReadOnly);
  setKvmReadOnly(inputControlActive,controller);
  renderAgentPresence(agentViewing,inputControlActive,controller,browserHealthy);
  if(videoModeSwitching){
    nosignal.classList.remove("hide");
    return;
  }
  if((backendHealthy&&browserHealthy)||(selectedOutputMode==="h264"&&browserHealthy)){
    videoUnhealthyPolls=0;
    nosignal.classList.add("hide");
    return;
  }
  nosignal.classList.remove("hide");
  if(selectedOutputMode==="h264"&&outputFailureMessage){
    videoText.textContent=outputFailureMessage;
    return;
  }
  if(backendHealthy){
    videoText.textContent="采集后端正常，正在重连浏览器视频流";
  }else if(kvmLeaseLastError){
    videoText.textContent=`${kvmLeaseLastError}，正在自动恢复`;
  }else{
    videoText.textContent=videoUnavailableReason(v,false);
  }
  if(selectedOutputMode==="h264"){
    if(videoClaimed&&!videoLeaseHeld){
      scheduleLeaseRecovery(250);
    }else if(videoClaimed&&videoLeaseHeld&&!browserHealthy&&!outputFailureMessage&&!h264Socket&&!h264Mse){
      startVideo(0);
    }else if(videoClaimed&&videoLeaseHeld&&!browserHealthy&&h264LastFrameAt>0&&performance.now()-h264LastFrameAt>=2500&&++videoUnhealthyPolls>=2){
      videoUnhealthyPolls=0;
      recoverH264(videoStreamId,"H.264 视频流停滞");
    }else if(browserHealthy){
      videoUnhealthyPolls=0;
    }
    return;
  }
  if(videoClaimed&&!videoLeaseHeld){
    scheduleLeaseRecovery(250);
  }else if(videoClaimed&&videoLeaseHeld&&++videoUnhealthyPolls>=2){
    videoUnhealthyPolls=0;
    if(!kvmLeaseTimer)scheduleLeaseRecovery(250);
    else scheduleVideoRecovery(250);
  }
}
function status(){return UI.status.refresh()}
const BOOT_KEY_LOOP_INTERVAL_MS=250;
const bootKeyLoopAllowed=new Set(["Delete","Escape","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"]);
let bootKeyLoopRunning=false,bootKeyLoopTimer=0,bootKeyLoopSending=false,bootKeyLoopCount=0,bootKeyLoopGeneration=0;
function bootKeyLoopLabel(){return bootKeyLoopKey.options[bootKeyLoopKey.selectedIndex]?.textContent||bootKeyLoopKey.value}
function renderBootKeyLoop(message=""){
  bootKeyLoopToggle.checked=bootKeyLoopRunning;
  bootKeyLoopText.textContent=bootKeyLoopRunning?"开启":"关闭";
  bootKeyLoopKey.disabled=bootKeyLoopRunning||kvmReadOnly||videoModeSwitching||videoPeerBlocked||msVideoPowerOff;
  if(message)bootKeyLoopState.textContent=message;
  else bootKeyLoopState.textContent=bootKeyLoopRunning?`运行中 · ${bootKeyLoopLabel()} · 已发送 ${bootKeyLoopCount} 次`:"选择按键后打开；进入 BIOS 后手动关闭。";
  bootKeyLoopState.className=`hint ${bootKeyLoopRunning?"ok":""}`.trim();
  shortcutToggle.classList.toggle("running",bootKeyLoopRunning);
}
function stopBootKeyLoop(reason="已手动关闭"){
  if(!bootKeyLoopRunning){bootKeyLoopToggle.checked=false;return}
  bootKeyLoopRunning=false;bootKeyLoopSending=false;bootKeyLoopGeneration++;
  if(bootKeyLoopTimer)clearInterval(bootKeyLoopTimer);bootKeyLoopTimer=0;
  cancelPendingInputActions();API.send({type:"releaseall"});renderBootKeyLoop(reason);
}
async function sendBootKeyLoopTick(generation){
  if(!bootKeyLoopRunning||bootKeyLoopSending||generation!==bootKeyLoopGeneration)return;
  if(kvmReadOnly||kvmLeaving||document.hidden||videoPeerBlocked||msVideoPowerOff){stopBootKeyLoop("控制不可用，循环已停止");return}
  if(!keyboardWsReady()){renderBootKeyLoop(`等待 HID 连接 · ${bootKeyLoopLabel()}`);if(hidMayConnect())void API.connect();return}
  bootKeyLoopSending=true;
  try{if(await tapCode(bootKeyLoopKey.value,[],inputGeneration)){if(bootKeyLoopRunning&&generation===bootKeyLoopGeneration){bootKeyLoopCount++;renderBootKeyLoop()}}}
  finally{if(generation===bootKeyLoopGeneration)bootKeyLoopSending=false}
}
function startBootKeyLoop(){
  const key=bootKeyLoopKey.value;
  if(!bootKeyLoopAllowed.has(key)||kvmReadOnly||kvmLeaving||document.hidden||videoPeerBlocked||msVideoPowerOff){bootKeyLoopToggle.checked=false;renderBootKeyLoop("当前无法开启循环按键");return}
  if(bootKeyLoopRunning)return;
  bootKeyLoopRunning=true;bootKeyLoopCount=0;bootKeyLoopGeneration++;
  const generation=bootKeyLoopGeneration;renderBootKeyLoop();void sendBootKeyLoopTick(generation);
  bootKeyLoopTimer=window.setInterval(()=>{void sendBootKeyLoopTick(generation)},BOOT_KEY_LOOP_INTERVAL_MS);
}
bootKeyLoopToggle.onchange=()=>{if(bootKeyLoopToggle.checked)startBootKeyLoop();else stopBootKeyLoop("已手动关闭")};
bootKeyLoopKey.onchange=()=>renderBootKeyLoop();
renderBootKeyLoop();

/* Capture-card tint detector. A heat-soaked MS2109 mis-detects the HDMI colour format and
 * delivers whole frames with white rendered magenta and dark areas green (both chroma
 * averages far above 128). The device only forwards those JPEGs, so the check runs on the
 * rendered picture: the H.264 canvas/video element is sampled directly, the MJPEG <img>
 * comes from the stream port and would taint a canvas, so that mode samples the snapshot route instead.
 * Three consecutive hits show the banner, three clean samples hide it. */
const TINT_SAMPLE_MS=1000,TINT_SNAPSHOT_MS=2500,TINT_ON_SAMPLES=3,TINT_OFF_SAMPLES=3,TINT_U_MIN=145,TINT_V_MIN=150;
const tintBanner=document.getElementById("tintBanner"),tintTitle=document.getElementById("tintTitle"),tintDetail=document.getElementById("tintDetail"),tintColdStart=document.getElementById("tintColdStart");
let tintCanvas=null,tintCtx=null,tintHits=0,tintClears=0,tintShown=false,tintSnapshotBusy=false,tintLastSnapshotAt=0,tintColdPending=false,tintColdActive=false,tintLast=null;
function tintMeasure(source){
  if(!tintCanvas){tintCanvas=document.createElement("canvas");tintCanvas.width=32;tintCanvas.height=18;tintCtx=tintCanvas.getContext("2d",{willReadFrequently:true})}
  tintCtx.drawImage(source,0,0,32,18);
  const d=tintCtx.getImageData(0,0,32,18).data;let r=0,g=0,b=0;const n=d.length/4;
  for(let i=0;i<d.length;i+=4){r+=d[i];g+=d[i+1];b+=d[i+2]}
  r/=n;g/=n;b/=n;
  return{y:0.299*r+0.587*g+0.114*b,u:128-0.168736*r-0.331264*g+0.5*b,v:128+0.5*r-0.418688*g-0.081312*b};
}
function tintIsMagenta(m){return m.u>TINT_U_MIN&&m.v>TINT_V_MIN}
function tintRender(){
  const visible=tintShown||tintColdActive;
  tintBanner.classList.toggle("hide",!visible);
  if(!visible)return;
  if(tintColdActive){tintTitle.textContent="采集卡冷启动中";tintDetail.textContent=`断电中，${tintColdSeconds} s 后自动上电并恢复视频`;tintColdStart.hidden=true;return}
  tintTitle.textContent="采集卡色彩异常";
  tintDetail.textContent=`整幅偏品红/绿（U ${tintLast?Math.round(tintLast.u):"--"} · V ${tintLast?Math.round(tintLast.v):"--"}），通常是采集卡过热；冷启动可恢复`;
  tintColdStart.hidden=false;tintColdStart.disabled=tintColdPending;tintColdStart.textContent=tintColdPending?"处理中…":"冷启动采集卡";
}
function tintRecord(m){
  tintLast=m;
  if(tintIsMagenta(m)){tintClears=0;if(++tintHits>=TINT_ON_SAMPLES&&!tintShown){tintShown=true;tintRender()}}
  else{tintHits=0;if(++tintClears>=TINT_OFF_SAMPLES&&tintShown){tintShown=false;tintRender()}}
}
function tintReset(){tintHits=0;tintClears=0;if(tintShown){tintShown=false;tintRender()}}
async function tintSampleSnapshot(){
  const now=performance.now();
  if(tintSnapshotBusy||now-tintLastSnapshotAt<TINT_SNAPSHOT_MS)return;
  tintSnapshotBusy=true;tintLastSnapshotAt=now;
  try{
    const response=await fetch("/api/snapshot",{cache:"no-store",credentials:"same-origin",signal:UI.lifecycle.signal()});
    if(!response.ok)return;
    const bitmap=await createImageBitmap(await response.blob());
    try{tintRecord(tintMeasure(bitmap))}finally{bitmap.close()}
  }catch(e){}
  finally{tintSnapshotBusy=false}
}
function tintTick(){
  if(msVideoPowerOff||kvmLeaving||!kvmPageActive||document.hidden||!nosignal.classList.contains("hide")){tintReset();return}
  try{
    if(selectedOutputMode==="h264"){
      const mse=document.getElementById("h264MseVideo"),canvas=document.getElementById("h264Video");
      if(mse&&!mse.hidden&&mse.readyState>=2&&mse.videoWidth>0)tintRecord(tintMeasure(mse));
      else if(canvas&&!canvas.hidden&&canvas.width>0)tintRecord(tintMeasure(canvas));
      return;
    }
    if(videoStreamReady)void tintSampleSnapshot();
  }catch(e){/* a tainted canvas or a detached element only skips this sample */}
}
let tintColdSeconds=0,tintColdEndedAt=0;
/* true for 20 s after a cold start finished: the UVC reopen may fail transiently while the card re-enumerates. */
function tintRecentColdStart(){return tintColdEndedAt>0&&performance.now()-tintColdEndedAt<20000}
function renderColdStart(s){
  const ms2109=s?.ms2109||{},active=ms2109.cold_start_active===true;
  tintColdSeconds=Math.ceil(Number(ms2109.cold_start_remaining_ms||0)/1000);
  if(active){tintColdActive=true;tintHits=0;tintClears=0;tintShown=false;tintRender();return true}
  if(tintColdActive){tintColdActive=false;tintColdEndedAt=performance.now();tintRender()}
  return false;
}
async function tintRequestColdStart(){
  if(tintColdPending)return;
  tintColdPending=true;tintRender();
  try{await UI.api.post("/api/ms2109/power",{action:"cold_start",off_time_ms:45000})}
  catch(e){tintDetail.textContent=`冷启动失败：${String(e?.message||e).slice(0,80)}`}
  finally{tintColdPending=false;tintRender();void UI.status.refresh().catch(()=>{})}
}
tintColdStart.onclick=()=>{void tintRequestColdStart()};
UI.lifecycle.interval(tintTick,TINT_SAMPLE_MS);

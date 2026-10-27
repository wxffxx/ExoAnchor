/* ExoAnchor KVM page module. Loaded as a classic script in a fixed order after
 * ui-core.js and ui-shell.js; the modules share one global lexical scope, so
 * declarations here are visible to the modules loaded after it. Order:
 * kvm-core -> kvm-video -> kvm-lease -> kvm-modes -> kvm-input -> kvm-status.
 * Capture-mode and output-mode selectors plus the snapshot / reconnect / fullscreen buttons.
 */
function modeFpsX100(m){return Number(m.fps_x100||Math.round(Number(m.fps||0)*100))}
function modeFormat(m){return String(m.pixel_format||"MJPEG").toUpperCase()}
function modeValue(m){return `${modeFormat(m)}:${m.width}x${m.height}@${modeFpsX100(m)}`}
function modeLabel(m){const fps=modeFpsX100(m)/100;return `${modeFormat(m)} ${m.width}x${m.height} @ ${fps%1?fps.toFixed(1):fps.toFixed(0)}fps`}
const COMMON_VIDEO_MODES=[["MJPEG",1920,1080,50],["MJPEG",1920,1080,30],["MJPEG",1920,1080,25],["MJPEG",1920,1080,20],["MJPEG",1920,1080,15],["MJPEG",1280,720,50],["MJPEG",1280,720,30],["MJPEG",640,480,30]];
function commonModes(modes){const list=Array.isArray(modes)?modes:[];return COMMON_VIDEO_MODES.map(([fmt,w,h,fps])=>list.find(m=>modeFormat(m)===fmt&&m.width===w&&m.height===h&&modeFpsX100(m)===fps*100)).filter(Boolean)}
let resolutionSignature="";
function renderResolution(v){const allModes=Array.isArray(v.modes)?v.modes:[],modes=commonModes(allModes),kvm=v.control?.kvm_mode||{};const saved=`${kvm.width||0}x${kvm.height||0}@${kvm.fps_x100||0}`;const signature=saved+"|"+modes.map(m=>`${modeValue(m)}:${m.selected?1:0}`).join("|"),disabled=videoModeSwitching||kvmReadOnly||msVideoPowerOff||modes.length===0;if(videoModeSwitching&&resolutionSelect.options.length){resolutionSelect.disabled=true;return}if(document.activeElement===resolutionSelect&&resolutionSelect.options.length){resolutionSelect.disabled=disabled;return}if(signature===resolutionSignature){resolutionSelect.disabled=disabled;return}resolutionSignature=signature;resolutionSelect.disabled=disabled;resolutionSelect.innerHTML=modes.length?modes.map(m=>`<option value="${modeValue(m)}">${modeLabel(m)}</option>`).join(""):`<option value="">No common mode</option>`;if(modes.length){const selected=modes.find(m=>m.width===kvm.width&&m.height===kvm.height&&modeFpsX100(m)===(kvm.fps_x100||0))||modes.find(m=>m.selected)||modes.find(m=>m.width===v.width&&m.height===v.height)||modes[0];resolutionSelect.value=modeValue(selected)}}
resolutionSelect.onchange=async()=>{
  if(videoModeSwitching)return;
  const m=/^([A-Z0-9]+):(\d+)x(\d+)@(\d+)$/.exec(resolutionSelect.value||"");if(!m)return;
  const pixel_format=m[1],w=Number(m[2]),h=Number(m[3]),fps_x100=Number(m[4]),switchGeneration=beginVideoTransition(`正在切换到 ${w}x${h} @ ${(fps_x100/100).toFixed(fps_x100%100?1:0)}fps`);if(!switchGeneration)return;
  try{
    await API.post("/api/video/resolution",{pixel_format,width:w,height:h,fps_x100});
    if(switchGeneration!==videoModeSwitchGeneration||kvmLeaving)return;
    const claimed=await restartVideoForTransition(switchGeneration,650);
    if(!claimed&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error("视频模式已保存，但视频租约尚未恢复");
    if(claimed&&!(await waitForVideoTransitionReady(switchGeneration))&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error(outputFailureMessage||"新采集模式未输出匹配的有效画面");
    UI.lifecycle.timeout(()=>{if(switchGeneration===videoModeSwitchGeneration)void status()},900);
  }catch(e){
    if(switchGeneration===videoModeSwitchGeneration){videoText.textContent=e.message||"video mode switch failed";nosignal.classList.remove("hide")}
  }finally{
    finishVideoTransition(switchGeneration)
  }
};
outputModeSelect.onchange=async()=>{if(videoModeSwitching){outputModeSelect.value=selectedOutputMode;return}const next=outputModeSelect.value==="h264"&&h264OutputAvailable?"h264":"mjpeg",switchGeneration=beginVideoTransition(`正在切换到 ${next==="h264"?"H.264":"MJPEG"} 输出`);if(!switchGeneration)return;selectedOutputMode=next;outputModeSelect.value=next;try{localStorage.setItem(outputModeStorageKey,next)}catch(e){}try{const claimed=await restartVideoForTransition(switchGeneration,0);if(!claimed&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error("视频输出租约尚未恢复");if(claimed&&!(await waitForVideoTransitionReady(switchGeneration))&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error(outputFailureMessage||`${next==="h264"?"H.264":"MJPEG"} 未输出匹配的有效画面`)}catch(e){if(switchGeneration===videoModeSwitchGeneration){videoText.textContent=e.message||"video output switch failed";nosignal.classList.remove("hide")}}finally{finishVideoTransition(switchGeneration)}};
/* One "quality" selector = capture mode x output format. The two underlying selects stay the engine (and the firmware contract), this only drives them. */
function qualityOutputs(res){const m=/^[A-Z0-9]+:(\d+)x(\d+)@(\d+)$/.exec(res||"");const outputs=["mjpeg"];if(m&&h264OutputAvailable&&h264CaptureSpecSupported({width:Number(m[1]),height:Number(m[2]),fpsX100:Number(m[3])}))outputs.unshift("h264");return outputs}
function qualityLabel(res,output){const m=/^[A-Z0-9]+:(\d+)x(\d+)@(\d+)$/.exec(res||"");if(!m)return res;const fps=Number(m[3])/100;return `${m[1]}x${m[2]} @ ${fps%1?fps.toFixed(1):fps.toFixed(0)} · ${output==="h264"?"H.264":"MJPEG"}`}
let qualitySignature="";
function renderQualitySelect(){const disabled=resolutionSelect.disabled||videoModeSwitching||kvmReadOnly||msVideoPowerOff;qualitySelect.disabled=disabled;const options=Array.from(resolutionSelect.options).filter(o=>o.value);const current=`${resolutionSelect.value}|${selectedOutputMode}`,signature=options.map(o=>o.value).join(",")+"#"+current;if(document.activeElement===qualitySelect&&qualitySelect.options.length)return;if(signature===qualitySignature)return;qualitySignature=signature;qualitySelect.innerHTML=options.length?options.flatMap(o=>qualityOutputs(o.value).map(output=>`<option value="${o.value}|${output}">${qualityLabel(o.value,output)}</option>`)).join(""):`<option value="">无可用采集模式</option>`;if(options.length)qualitySelect.value=current;if(qualitySelect.value!==current&&options.length)qualitySelect.value=`${resolutionSelect.value}|mjpeg`}
qualitySelect.onchange=async()=>{if(videoModeSwitching){qualitySignature="";renderQualitySelect();return}const [res,output]=String(qualitySelect.value||"").split("|");if(!res)return;const nextOutput=output==="h264"&&h264OutputAvailable?"h264":"mjpeg",resChanged=res!==resolutionSelect.value,outChanged=nextOutput!==selectedOutputMode;if(!resChanged&&!outChanged)return;if(resChanged){if(outChanged){selectedOutputMode=nextOutput;outputModeSelect.value=nextOutput;try{localStorage.setItem(outputModeStorageKey,nextOutput)}catch(e){}}resolutionSelect.value=res;await resolutionSelect.onchange()}else{outputModeSelect.value=nextOutput;await outputModeSelect.onchange()}qualitySignature="";renderQualitySelect()};
/* One-click fallback offered on the no-signal overlay once H.264 has given up: same resolution, MJPEG output. */fallbackMjpeg.onclick=()=>{if(videoModeSwitching||!resolutionSelect.value)return;fallbackMjpeg.hidden=true;qualitySelect.value=`${resolutionSelect.value}|mjpeg`;qualitySelect.dispatchEvent(new Event("change"))};
snapshot.onclick=()=>window.open(authUrl("/api/snapshot?t="+Date.now()),"_blank");
reload.onclick=async()=>{const switchGeneration=beginVideoTransition("正在重新连接视频");if(!switchGeneration)return;try{const claimed=await restartVideoForTransition(switchGeneration,0);if(!claimed&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error("视频租约尚未恢复");if(claimed&&!(await waitForVideoTransitionReady(switchGeneration))&&switchGeneration===videoModeSwitchGeneration&&!document.hidden)throw new Error(outputFailureMessage||"重连后未收到匹配的有效画面")}catch(e){if(switchGeneration===videoModeSwitchGeneration){videoText.textContent=e.message||"video reconnect failed";nosignal.classList.remove("hide")}}finally{finishVideoTransition(switchGeneration)}};
fs.onclick=()=>document.fullscreenElement?document.exitFullscreen():screenEl.requestFullscreen();

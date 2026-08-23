(function () {
  "use strict";

  const UI = window.ExoAnchorUI;
  if (!UI) throw new Error("ui-core.js must load before ui-shell.js");

  const embeddedAgent = UI.features.embeddedAgent;
  const uartTerminal = UI.features.uartTerminal;
  const links = [
    ["overview", "/", "Overview"],
    ["kvm", "/kvm", "KVM"],
    ...(embeddedAgent ? [["agent", "/agent", "Agent"]] : []),
    ...(uartTerminal ? [["terminal", "/terminal", "Terminal"]] : []),
    ["settings", "/settings", "Settings"],
  ];
  const RELEASE_NAME = "IndigoShore";
  const CHIP_LABEL = "P4";
  const DISPLAY_VERSION = UI.VERSION.replace(/-dev$/, "dev");
  const DEFAULT_ASSISTANT_NAME = "Agent";
  const BRAND_MARK_HTML = '<img src="/assets/exoanchor-ui-mark.svg" alt="">';
  const PRODUCT_FEATURES_CHANNEL_NAME = "exoanchor-product-features";
  let mounted = false;
  let assistantName = DEFAULT_ASSISTANT_NAME;
  let agentRuntimeEnabled = false;
  let agentAvailabilityGeneration = 0;
  let agentFeatureChannel = null;
  let agentResumeRefreshAt = 0;
  let assistantRuntimeInitialized = false;
  let automationState = { operations: [] };
  let automationSignature = "";
  let lastDeviceStatus = null;
  let msPowerMutationPending = false;

  function byId(id) { return document.getElementById(id); }

  function setBadge(element, label, state, kind) {
    UI.setStatus(element, label + " " + state, kind);
  }

  function normalizeDeviceLabel(value) {
    const label = String(value || "").trim();
    if (/^(?:ESP32[- ]?P4|P4)$/i.test(label)) return "";
    return label;
  }

  function renderDeviceLabel(info) {
    const element = byId("deviceLabelText");
    if (!element) return;
    const live = normalizeDeviceLabel(info?.device_label || info?.label);
    if (live) localStorage.setItem("si_device_label", live);
    const cached = normalizeDeviceLabel(localStorage.getItem("si_device_label"));
    if (!cached && localStorage.getItem("si_device_label")) localStorage.removeItem("si_device_label");
    const label = live || cached;
    element.textContent = label || "读取设备名称…";
    element.title = label || "正在读取设备名称";
  }

  function applyAgentAvailability(config, known = true) {
    if (!embeddedAgent) return;
    const enabled = known && config?.embedded_agent_enabled === true;
    agentRuntimeEnabled = enabled;
    const link = byId("agentNav");
    if (link) {
      if (enabled) {
        link.setAttribute("href", link.dataset.agentHref || "/agent");
        link.removeAttribute("aria-disabled");
        link.removeAttribute("tabindex");
        link.classList.remove("agent-disabled");
        link.title = "";
      } else {
        link.removeAttribute("href");
        link.setAttribute("aria-disabled", "true");
        link.setAttribute("tabindex", "-1");
        link.classList.add("agent-disabled");
        link.title = known ? "内置 Agent 已在 Settings 中关闭" :
          "正在读取内置 Agent 状态";
      }
    }
    const launcher = byId("eaAssistantLauncher");
    if (launcher) {
      launcher.disabled = !enabled;
      launcher.setAttribute("aria-disabled", enabled ? "false" : "true");
      launcher.classList.toggle("runtime-disabled", !enabled);
    }
    if (!enabled) {
      clearTimeout(assistant.pollTimer);
      assistant.pollTimer = 0;
      assistant.open = false;
      byId("eaAssistantWindow")?.classList.remove("show");
      launcher?.classList.remove("active");
      launcher?.setAttribute("aria-expanded", "false");
      return;
    }
    if (!assistantRuntimeInitialized && byId("eaAssistantLauncher")) {
      assistantRuntimeInitialized = true;
      loadAssistantName();
      UI.api.ensure().then(ok => {
        if (ok && agentRuntimeEnabled) assistantSyncRun();
      }).catch(() => {});
    }
  }

  function updateProductFeatures(config) {
    agentAvailabilityGeneration += 1;
    applyAgentAvailability(config, true);
  }

  function handleAgentFeatureBroadcast(event) {
    const config = event?.data;
    if (config?.type !== "embedded-agent-updated" ||
        typeof config.embedded_agent_enabled !== "boolean") return;
    updateProductFeatures(config);
  }

  async function refreshAgentAvailability() {
    if (!embeddedAgent) return;
    const generation = agentAvailabilityGeneration;
    try {
      const config = await UI.api.get("/api/settings/product-features");
      if (generation === agentAvailabilityGeneration) {
        applyAgentAvailability(config, true);
      }
    } catch (error) {
      if (generation === agentAvailabilityGeneration) {
        applyAgentAvailability(null, false);
      }
    }
  }

  function refreshAgentAvailabilityOnResume() {
    if (!embeddedAgent) return;
    const now = Date.now();
    if (now - agentResumeRefreshAt < 500) return;
    agentResumeRefreshAt = now;
    agentAvailabilityGeneration += 1;
    applyAgentAvailability(null, false);
    UI.api.ensure().then(ok => {
      if (ok) refreshAgentAvailability();
    }).catch(() => {});
  }

  function normalizeAssistantName(value) {
    return String(value || "").trim().slice(0, 32) ||
      DEFAULT_ASSISTANT_NAME;
  }

  function applyAssistantName(value) {
    assistantName = normalizeAssistantName(value);
    localStorage.setItem("ea_agent_display_name", assistantName);
    const launcher = byId("eaAssistantLauncher");
    if (launcher) {
      launcher.title = assistantName + " · Ctrl/⌘ + /";
      launcher.setAttribute("aria-label", assistantName);
    }
    const dialog = byId("eaAssistantWindow");
    if (dialog) dialog.setAttribute("aria-label", assistantName + " 对话助手");
    ["eaAssistantLauncherName", "eaAssistantName",
     "eaAssistantEmptyName"].forEach(id => {
      const element = byId(id);
      if (element) element.textContent = assistantName;
    });
  }

  async function loadAssistantName() {
    try {
      const identity = await UI.api.get("/api/settings/device");
      applyAssistantName(identity?.agent_name ||
                         identity?.default_agent_name);
    } catch (error) {
      applyAssistantName(localStorage.getItem("ea_agent_display_name"));
    }
  }

  function modeCount(video) {
    if (Array.isArray(video && video.modes)) return video.modes.length;
    return Number(video && (video.modes_count ?? video.mode_count ?? video.modes) || 0);
  }

  function videoModeState(video) {
    const modes = Array.isArray(video && video.modes) ? video.modes : [];
    const selected = modes.find(mode => mode && mode.selected) || {};
    const height = Number(video.height || selected.height || video.target_height || 0);
    const fpsX100 = Number(video.target_fps_x100 || selected.fps_x100 || 0);
    const fps = fpsX100 > 0 ? fpsX100 / 100 : Number(video.target_fps || selected.fps || 0);
    if (!(height > 0 && fps > 0)) return "--";
    const fpsLabel = Number.isInteger(fps) ? String(fps) : fps.toFixed(2).replace(/0+$/, "").replace(/\.$/, "");
    return height + "P" + fpsLabel;
  }

  const AUTOMATION_RESOURCE_LABELS = Object.freeze({
    power: "主机电源",
    hid: "键盘与鼠标",
    ssh: "SSH 终端",
    uart: "UART 终端",
    "video-observe": "KVM 视频",
    "video-config": "视频配置",
    "video-hardware": "视频硬件",
    maintenance: "系统维护",
    "agent-data": "Agent 数据",
  });

  function automationOperations() {
    return Array.isArray(automationState?.operations) ?
      automationState.operations.filter(operation => operation?.resource) : [];
  }

  function automationBlocksPower() {
    return automationOperations().some(operation =>
      operation.blocks_manual !== false &&
      ["power", "hid", "maintenance", "ssh", "uart"].includes(
        String(operation.resource || "")));
  }

  function automationBlocksVideoPower() {
    return automationOperations().some(operation =>
      operation.blocks_manual !== false &&
      ["video-observe", "video-config", "video-hardware", "maintenance"].includes(
        String(operation.resource || "")));
  }

  function automationActorLabel(actor) {
    if (actor === "agent") return "Agent";
    if (actor === "mcp") return "MCP";
    if (actor === "system") return "SYSTEM";
    return "自动任务";
  }

  function automationPrimaryOperation(operations) {
    return operations.slice().sort((left, right) => {
      const blockDelta = Number(right.blocks_manual !== false) -
        Number(left.blocks_manual !== false);
      if (blockDelta) return blockDelta;
      return Number(left.started_ms || 0) - Number(right.started_ms || 0);
    })[0] || null;
  }

  function renderAutomation(state) {
    automationState = state && typeof state === "object" ? state :
      { operations: [] };
    const operations = automationOperations();
    const banner = byId("eaAutomationBanner");
    const primary = automationPrimaryOperation(operations);
    if (banner) {
      banner.hidden = !primary;
      banner.classList.toggle("observing",
        !!primary && primary.blocks_manual === false);
      banner.classList.toggle("stopping", !!primary?.cancel_requested);
      if (primary) {
        const actor = automationActorLabel(String(primary.actor || ""));
        const resource = AUTOMATION_RESOURCE_LABELS[primary.resource] ||
          String(primary.resource || "设备资源");
        const title = byId("eaAutomationTitle");
        const detail = byId("eaAutomationDetail");
        const stop = byId("eaAutomationStop");
        if (title) title.textContent = primary.cancel_requested ?
          actor + " 正在停止" : actor + " 正在操作 " + resource;
        if (detail) {
          const count = operations.length > 1 ?
            " · 另有 " + (operations.length - 1) + " 项自动操作" : "";
          detail.textContent = String(primary.label || "自动操作进行中") + count +
            (primary.blocks_manual === false ? " · 手动查看仍可用" :
              " · 冲突的手动操作已暂停");
        }
        if (stop) {
          stop.hidden = primary.cancellable === false;
          stop.disabled = primary.cancellable === false ||
            primary.cancel_requested === true;
          stop.dataset.resource = String(primary.resource || "");
          stop.dataset.generation = String(primary.generation || 0);
          stop.textContent = primary.cancel_requested ? "正在终止…" : "立即终止";
        }
      }
    }
    renderStatus(lastDeviceStatus);
    const signature = JSON.stringify(operations.map(operation => ({
      resource: operation.resource,
      actor: operation.actor,
      generation: operation.generation,
      cancel_requested: !!operation.cancel_requested,
      blocks_manual: operation.blocks_manual !== false,
    })));
    if (signature !== automationSignature) {
      automationSignature = signature;
      document.dispatchEvent(new CustomEvent("exoanchor:automation-changed", {
        detail: { operations: operations.slice() },
      }));
    }
  }

  async function refreshAutomation() {
    if (!UI.session.active()) return;
    try {
      renderAutomation(await UI.api.getShared("/api/automation/status", {
        timeoutMs: 3000,
      }));
    } catch (error) {
      if (error?.name === "AbortError") return;
      // A transient status failure must not unlock controls using stale data.
    }
  }

  async function stopPrimaryAutomation() {
    const button = byId("eaAutomationStop");
    const resource = String(button?.dataset.resource || "");
    const generation = Number(button?.dataset.generation || 0);
    if (!resource || !(generation > 0) || button.disabled) return;
    button.disabled = true;
    button.textContent = "正在终止…";
    try {
      renderAutomation(await UI.api.post("/api/automation/stop", {
        resource,
        generation,
      }));
    } catch (error) {
      await refreshAutomation();
    }
  }

  function renderStatus(status, error) {
    if (!error && status) lastDeviceStatus = status;
    if (error || !status) {
      setBadge(byId("badgeVideo"), "VIDEO", "UNKNOWN", "warn");
      setBadge(byId("badgeUsb"), "USB", "UNKNOWN", "warn");
      setBadge(byId("badgePwr"), "POWER", "UNKNOWN", "warn");
      if (byId("badgeVideo")) byId("badgeVideo").disabled = true;
      if (byId("badgePwr")) byId("badgePwr").disabled = true;
      if (byId("videoPowerToggle")) {
        byId("videoPowerToggle").checked = false;
        byId("videoPowerToggle").disabled = true;
      }
      if (byId("videoPowerState")) byId("videoPowerState").textContent = "不可用";
      if (byId("powerActionPower")) byId("powerActionPower").disabled = true;
      if (byId("powerActionReset")) byId("powerActionReset").disabled = true;
      return;
    }
    const video = status.video || {};
    const ms2109 = status.ms2109 || {};
    const hid = status.hid || {};
    const power = status.power || {};
    const detect = power.detect || {};
    const hidMounted = !!(hid.connected || hid.mounted);
    byId("badgeVideo").removeAttribute("title");

    const msVideoOff = ms2109.supported === true &&
      ms2109.initialized === true && ms2109.power_on === false;
    if (msVideoOff) {
      setBadge(byId("badgeVideo"), "VIDEO", "OFF", "neutral");
      byId("badgeVideo").title = "MS2109 电源已关闭";
    }
    else if (video.frame_ready) {
      setBadge(byId("badgeVideo"), "VIDEO", videoModeState(video), "ok");
      const width = Number(video.width || video.target_width || 0);
      const height = Number(video.height || video.target_height || 0);
      byId("badgeVideo").title = width > 0 && height > 0 ? width + "×" + height : "视频在线";
    }
    else if (video.device_connected || modeCount(video) > 0) setBadge(byId("badgeVideo"), "VIDEO", "STANDBY", "warn");
    else setBadge(byId("badgeVideo"), "VIDEO", "OFFLINE", "bad");

    if (hidMounted) setBadge(byId("badgeUsb"), "USB", "ONLINE", "ok");
    else if (hid.initialized || hid.enabled) setBadge(byId("badgeUsb"), "USB", "STANDBY", "warn");
    else setBadge(byId("badgeUsb"), "USB", "OFFLINE", "bad");

    const rail12 = detect.power;
    const railAux = detect.standby;
    if (!rail12?.supported || !railAux?.supported) setBadge(byId("badgePwr"), "POWER", "N/A", "unknown");
    else if (rail12.active) setBadge(byId("badgePwr"), "POWER", "ON", "ok");
    else if (railAux.active) setBadge(byId("badgePwr"), "POWER", "STANDBY", "warn");
    else setBadge(byId("badgePwr"), "POWER", "OFF", "bad");

    const videoControl = video.control || {};
    const videoAutomationBlocked = automationBlocksVideoPower() ||
      videoControl.agent_active === true;
    const msPowerAvailable = ms2109.supported === true &&
      ms2109.initialized === true && !videoAutomationBlocked;
    const msPowerKnown = ms2109.power_on === true ||
      ms2109.power_on === false;
    byId("badgeVideo").disabled = !(ms2109.supported === true &&
      ms2109.initialized === true);
    if (videoAutomationBlocked) {
      byId("badgeVideo").title =
        "Agent/MCP 正在使用视频；请先在顶部终止自动操作";
    }
    byId("videoPowerToggle").checked = ms2109.power_on === true;
    byId("videoPowerToggle").disabled = msPowerMutationPending ||
      !(msPowerAvailable && msPowerKnown);
    byId("videoPowerState").textContent = msPowerKnown ?
      (ms2109.power_on === true ? "开启" : "关闭") : "未知";

    const automationBlocked = automationBlocksPower();
    const available = !!power.available && !power.busy && !automationBlocked;
    byId("badgePwr").disabled = !available;
    byId("badgePwr").title = automationBlocked ?
      "Agent/MCP 自动操作进行中，请先在顶部终止" : "";
    byId("powerActionPower").disabled = !(available && power.buttons?.power?.available);
    byId("powerActionReset").disabled = !(available && power.buttons?.reset?.available);
  }

  function closeVideoMenu() {
    byId("videoMenu")?.classList.add("hide");
    byId("badgeVideo")?.setAttribute("aria-expanded", "false");
  }

  function closePowerMenu() {
    byId("powerMenu")?.classList.add("hide");
    byId("badgePwr")?.setAttribute("aria-expanded", "false");
  }

  function closeStatusMenus() {
    closeVideoMenu();
    closePowerMenu();
  }

  function toggleStatusMenu(menuId, buttonId) {
    const menu = byId(menuId);
    const button = byId(buttonId);
    if (!menu || !button || button.disabled) return;
    const willOpen = menu.classList.contains("hide");
    closeStatusMenus();
    if (willOpen) {
      menu.classList.remove("hide");
      button.setAttribute("aria-expanded", "true");
    }
  }

  async function powerAction(action) {
    closePowerMenu();
    byId("badgePwr").disabled = true;
    try { await UI.api.post("/api/power/action", { action }); }
    finally { await UI.status.refresh().catch(() => {}); }
  }

  async function ms2109PowerAction(enabled) {
    const toggle = byId("videoPowerToggle");
    const state = byId("videoPowerState");
    const previous = lastDeviceStatus?.ms2109?.power_on === true;
    const desired = enabled === true;
    if (!toggle || msPowerMutationPending || toggle.disabled) {
      if (toggle) toggle.checked = previous;
      return;
    }
    msPowerMutationPending = true;
    toggle.checked = desired;
    toggle.disabled = true;
    state.textContent = "处理中…";
    let succeeded = false;
    try {
      await UI.api.post("/api/ms2109/power", {
        action: desired ? "on" : "off",
      });
      succeeded = true;
    } catch (error) {
      toggle.checked = previous;
      state.textContent = previous ? "开启" : "关闭";
      throw error;
    } finally {
      msPowerMutationPending = false;
      await UI.status.refresh().catch(() => {});
      if (state.textContent === "处理中…") {
        toggle.checked = succeeded ? desired : previous;
        state.textContent = toggle.checked ? "开启" : "关闭";
      }
      const ms2109 = lastDeviceStatus?.ms2109 || {};
      toggle.disabled = automationBlocksVideoPower() ||
        ms2109.supported !== true || ms2109.initialized !== true;
    }
  }

  function showLogout() { byId("shellLogoutModal").classList.add("show"); }
  function hideLogout() { byId("shellLogoutModal").classList.remove("show"); }
  async function performLogout() {
    hideLogout();
    UI.lifecycle.logout();
    try { await UI.api.post("/api/auth/logout", {}); } catch (error) {}
    UI.lifecycle.destroy("logout");
    UI.api.setSession("", UI.api.username);
    localStorage.removeItem("ea_auth_confirmed");
    window.location.assign("/");
  }

  function bindShell() {
    byId("badgeVideo").onclick = function () {
      toggleStatusMenu("videoMenu", "badgeVideo");
    };
    byId("badgePwr").onclick = function () {
      toggleStatusMenu("powerMenu", "badgePwr");
    };
    byId("videoPowerToggle").onchange = function () {
      ms2109PowerAction(this.checked).catch(() => {});
    };
    byId("powerActionPower").onclick = function () { powerAction("power").catch(() => {}); };
    byId("powerActionReset").onclick = function () { powerAction("reset").catch(() => {}); };
    byId("logoutNav").onclick = showLogout;
    byId("eaAutomationStop").onclick = () => {
      stopPrimaryAutomation().catch(() => {});
    };
    byId("shellLogoutCancel").onclick = hideLogout;
    byId("shellLogoutConfirm").onclick = () => { performLogout().catch(() => {}); };
    byId("shellLogoutModal").onclick = event => { if (event.target === byId("shellLogoutModal")) hideLogout(); };
    document.addEventListener("keydown", event => {
      if (event.key === "Escape") {
        hideLogout();
        closeStatusMenus();
      }
    });
    document.addEventListener("click", event => {
      if (!event.target.closest(".ea-shell .status-menu")) closeStatusMenus();
    });
    document.addEventListener("click", event => {
      const link = event.target.closest(".ea-shell .nav a");
      if (!link || event.defaultPrevented || event.button !== 0 ||
          event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      if (link.getAttribute("aria-disabled") === "true") {
        event.preventDefault();
        return;
      }
      if (!link.hasAttribute("href")) return;
      const destination = link.href;
      if (!destination || destination === location.href) return;
      link.setAttribute("aria-busy", "true");
      UI.lifecycle.navigate(destination);
    }, true);
  }

  const assistant = {
    page: "overview",
    open: false,
    sessionId: "",
    jobId: "",
    polling: false,
    pollTimer: 0,
    context: null,
    historyLoaded: false,
    lastEventSeq: 0,
    streamJobId: "",
    streamNode: null,
  };

  function assistantSessionId(value) {
    return String(value || "").replace(/[^a-zA-Z0-9_-]/g, "").slice(0, 24);
  }

  function assistantNewSessionId() {
    const random = globalThis.crypto?.randomUUID?.().replaceAll("-", "").slice(0, 16) ||
      (Date.now().toString(16) + Math.random().toString(16).slice(2)).slice(0, 16);
    return assistantSessionId("s" + random);
  }

  function assistantSetSession(value) {
    assistant.sessionId = assistantSessionId(value);
    if (assistant.sessionId) localStorage.setItem("ea_agent_session", assistant.sessionId);
    else localStorage.removeItem("ea_agent_session");
    const label = byId("eaAssistantThread");
    if (label) label.textContent = assistant.sessionId || "新对话";
  }

  function assistantResetForDataClear() {
    clearTimeout(assistant.pollTimer);
    assistant.pollTimer = 0;
    assistant.polling = false;
    assistant.jobId = "";
    assistant.lastEventSeq = 0;
    assistant.streamJobId = "";
    assistant.streamNode = null;
    assistant.context = null;
    assistant.historyLoaded = true;
    assistantSetSession("");
    assistantRenderHistory([]);
    assistantRenderContext();
    assistantStatus("IDLE", "");
    assistantTaskControls(false);
  }

  function assistantAddMessage(kind, text) {
    const log = byId("eaAssistantLog");
    if (!log || text === undefined || text === null) return null;
    log.querySelector(".ea-assistant-empty")?.remove();
    const item = document.createElement("div");
    item.className = "ea-assistant-message " + kind;
    item.textContent = String(text);
    log.appendChild(item);
    while (log.children.length > 80) log.removeChild(log.firstElementChild);
    log.scrollTop = log.scrollHeight;
    return item;
  }

  function assistantScrollToTail() {
    const log = byId("eaAssistantLog");
    if (log) log.scrollTop = log.scrollHeight;
  }

  function assistantSetRunIdentity(jobId) {
    const next = String(jobId || "");
    if (assistant.streamJobId === next) return;
    assistant.streamJobId = next;
    assistant.lastEventSeq = 0;
    assistant.streamNode = null;
  }

  function assistantEnsureStream(jobId) {
    assistantSetRunIdentity(jobId);
    if (assistant.streamNode?.isConnected) return assistant.streamNode;
    const item = assistantAddMessage("agent", "");
    if (!item) return null;
    item.classList.add("ea-assistant-stream");
    item.dataset.jobId = String(jobId || "");
    item.innerHTML =
      '<div class="ea-assistant-stream-head">' +
        '<span class="ea-assistant-stream-dot" aria-hidden="true"></span>' +
        '<span class="ea-assistant-stream-label">正在处理</span>' +
        '<span class="ea-assistant-stream-elapsed"></span>' +
      '</div>' +
      '<div class="ea-assistant-stream-updates" aria-live="polite"></div>' +
      '<div class="ea-assistant-stream-answer" aria-live="polite"></div>';
    assistant.streamNode = item;
    return item;
  }

  function assistantVisibleEvent(event) {
    const text = String(event?.text || "").trim();
    if (!text || /tool\[\d+\] output ·/.test(text)) return "";
    return text.length > 180 ? text.slice(0, 179) + "…" : text;
  }

  function assistantAppendEvent(event) {
    const stream = assistantEnsureStream(assistant.jobId);
    const seq = Number(event?.seq || 0);
    if (seq && seq <= assistant.lastEventSeq) return;
    const text = assistantVisibleEvent(event);
    if (seq > assistant.lastEventSeq) assistant.lastEventSeq = seq;
    if (!text) return;
    const updates = stream?.querySelector(".ea-assistant-stream-updates");
    if (!updates || updates.lastElementChild?.textContent === text) return;
    const row = document.createElement("div");
    row.className = "ea-assistant-stream-update " +
      (["bad", "warn", "ok"].includes(event?.kind) ? event.kind : "info");
    row.textContent = text;
    updates.appendChild(row);
    while (updates.children.length > 6) updates.removeChild(updates.firstElementChild);
    assistantScrollToTail();
  }

  async function assistantPullEvents() {
    if (!assistant.jobId) return;
    const replay = await UI.api.getShared(
      "/api/agent/run/events?job_id=" +
      encodeURIComponent(assistant.jobId) + "&after_seq=" +
      assistant.lastEventSeq,
      { timeoutMs: 5000 });
    (Array.isArray(replay.events) ? replay.events : []).forEach(assistantAppendEvent);
    const next = Number(replay.next_after_seq || replay.latest_seq || 0);
    if (next > assistant.lastEventSeq) assistant.lastEventSeq = next;
  }

  function assistantUpdateStream(status) {
    const stream = assistantEnsureStream(status?.job_id || assistant.jobId);
    if (!stream) return;
    const label = stream.querySelector(".ea-assistant-stream-label");
    const elapsed = stream.querySelector(".ea-assistant-stream-elapsed");
    const state = String(status?.state || "");
    const failed = ["failed", "aborted"].includes(state);
    const paused = state === "paused";
    stream.classList.toggle("failed", failed);
    stream.classList.toggle("paused", paused);
    if (label) label.textContent = status?.cancel_requested ? "正在停止" :
      failed ? "任务结束，需要检查" :
      paused ? "已暂停" : (status?.stage || "正在处理");
    if (elapsed) elapsed.textContent = Math.max(0, Math.round(Number(status?.elapsed_ms || 0) / 1000)) + "s";
  }

  function assistantStreamText(node, text) {
    const value = String(text || "");
    if (!node) return Promise.resolve();
    if (document.hidden || matchMedia("(prefers-reduced-motion: reduce)").matches ||
        value.length < 2) {
      node.textContent = value;
      return Promise.resolve();
    }
    node.textContent = "";
    node.classList.add("streaming");
    let offset = 0;
    return new Promise(resolve => {
      const step = () => {
        const remaining = value.length - offset;
        const size = remaining > 180 ? 5 : remaining > 60 ? 3 : 1;
        node.textContent += value.slice(offset, offset + size);
        offset += size;
        assistantScrollToTail();
        if (offset < value.length) requestAnimationFrame(step);
        else {
          node.classList.remove("streaming");
          resolve();
        }
      };
      requestAnimationFrame(step);
    });
  }

  async function assistantFinishStream(status) {
    const stream = assistantEnsureStream(status?.job_id || assistant.jobId);
    if (!stream) return;
    const ok = status?.state === "done" && status?.result?.ok !== false;
    stream.classList.toggle("done", ok);
    stream.classList.toggle("failed", !ok);
    stream.classList.remove("paused");
    const label = stream.querySelector(".ea-assistant-stream-label");
    const elapsed = stream.querySelector(".ea-assistant-stream-elapsed");
    if (label) label.textContent = ok ? "已完成" : "任务结束，需要检查";
    if (elapsed) elapsed.textContent = "";
    await assistantStreamText(
      stream.querySelector(".ea-assistant-stream-answer"),
      assistantResultText(status?.result, status));
  }

  function assistantStatus(text, kind) {
    const node = byId("eaAssistantStatus");
    if (!node) return;
    node.textContent = text || "IDLE";
    node.className = "ea-assistant-status " + (kind || "");
  }

  function assistantTaskControls(active) {
    const stop = byId("eaAssistantStop");
    const input = byId("eaAssistantInput");
    const launcher = byId("eaAssistantLauncher");
    if (stop) stop.hidden = !active;
    if (launcher) {
      launcher.classList.toggle("running", !!active);
      launcher.setAttribute("data-task-active", active ? "true" : "false");
    }
    if (input) input.placeholder = active ?
      "补充要求会合并到当前任务…" :
      "问设备、规划任务，或从当前页面继续…";
  }

  function assistantResultText(run, status) {
    const result = run && typeof run === "object" ? run : {};
    const report = result.final_report && typeof result.final_report === "object" ?
      result.final_report : null;
    const parts = [];
    const message = String(report?.message || report?.summary ||
      result.message || result.summary || "").trim();
    if (message) parts.push(message);
    if (Array.isArray(report?.next_steps) && report.next_steps.length) {
      parts.push("下一步：" + report.next_steps.filter(Boolean).slice(0, 3).join("；"));
    }
    const error = String(result.error || status?.error || "").trim();
    if (error && !parts.includes(error)) parts.push(error);
    return parts.join("\n") || "任务已结束，没有收到可显示的回复。";
  }

  function assistantRenderHistory(records) {
    const log = byId("eaAssistantLog");
    if (!log) return;
    assistant.streamNode = null;
    log.innerHTML = "";
    (Array.isArray(records) ? records : []).forEach(record => {
      const role = String(record?.role || record?.kind || "system").toLowerCase();
      const kind = role === "user" ? "user" :
        (role === "agent" || role === "assistant") ? "agent" : "system";
      let text = record?.content || record?.text || record?.message || "";
      if (!text) {
        try { text = JSON.stringify(record); } catch (error) { text = ""; }
      }
      if (text) assistantAddMessage(kind, text);
    });
    if (!log.children.length) {
      log.innerHTML = '<div class="ea-assistant-empty"><b id="eaAssistantEmptyName"></b><span>从当前页面开始对话。</span></div>';
      applyAssistantName(assistantName);
    }
  }

  async function assistantLoadHistory() {
    assistantSetSession(localStorage.getItem("ea_agent_session"));
    if (!assistant.sessionId) {
      assistantRenderHistory([]);
      assistant.historyLoaded = true;
      return;
    }
    try {
      const history = await UI.api.get("/api/agent/history?session_id=" +
        encodeURIComponent(assistant.sessionId));
      assistantRenderHistory(history.records || []);
      assistant.historyLoaded = true;
    } catch (error) {
      assistantAddMessage("system", "对话记录暂不可用：" + (error.message || error));
    }
  }

  function assistantContextCapture() {
    assistant.context = UI.pageContext.capture(assistant.page);
    assistantRenderContext();
    return assistant.context;
  }

  function assistantRenderContext() {
    const box = byId("eaAssistantContextPreview");
    if (!box) return;
    const context = assistant.context;
    box.innerHTML = "";
    if (!context) {
      box.classList.remove("show");
      return;
    }
    box.classList.add("show");
    const meta = document.createElement("div");
    meta.className = "ea-assistant-context-meta";
    const captured = Number(context.captured_ms || 0);
    const capturedLabel = captured > 0 ?
      new Date(captured).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) :
      "刚刚";
    meta.textContent = context.page + " · " + capturedLabel + " · " +
      context.items.length + " 项 · 已脱敏";
    box.appendChild(meta);
    context.items.forEach((entry, index) => {
      const row = document.createElement("div");
      row.className = "ea-assistant-context-row";
      const text = document.createElement("span");
      text.textContent = entry.label + " · " + entry.value;
      const remove = document.createElement("button");
      remove.type = "button";
      remove.title = "不发送此项";
      remove.textContent = "×";
      remove.onclick = () => {
        context.items.splice(index, 1);
        assistantRenderContext();
      };
      row.append(text, remove);
      box.appendChild(row);
    });
    if (!context.items.length) {
      const empty = document.createElement("div");
      empty.className = "ea-assistant-context-meta";
      empty.textContent = "当前快照没有可发送项目";
      box.appendChild(empty);
    }
  }

  function assistantMaterializeSession() {
    if (assistant.sessionId) return assistant.sessionId;
    assistantSetSession(assistantNewSessionId());
    if (!assistant.sessionId) throw new Error("设备没有返回有效的 Thread ID");
    return assistant.sessionId;
  }

  function assistantSchedulePoll(delay) {
    clearTimeout(assistant.pollTimer);
    if (!assistant.open || document.hidden || !assistant.jobId) return;
    assistant.pollTimer = setTimeout(() => assistantPollRun(), delay || 1000);
  }

  async function assistantReadRunStatus() {
    const status = await UI.api.getShared(
      "/api/agent/run/status", { timeoutMs: 5000 });
    if (status?.state === "unavailable") {
      throw new Error("agent run snapshot temporarily unavailable");
    }
    return status;
  }

  async function assistantPollRun() {
    if (assistant.polling || !assistant.open || document.hidden) return;
    assistant.polling = true;
    try {
      const [status] = await Promise.all([
        assistantReadRunStatus(),
        assistantSyncRequests(),
      ]);
      const active = !!status.busy || ["running", "waiting_request", "paused"].includes(status.state);
      if (active && status.job_id) {
        assistant.jobId = String(status.job_id);
        assistantUpdateStream(status);
        if (status.session_id) assistantSetSession(status.session_id);
        await assistantPullEvents().catch(() => {});
        const elapsed = Math.max(0, Number(status.elapsed_ms || 0));
        assistantStatus((status.paused ? "PAUSED" : status.waiting_request ? "WAITING" : "RUNNING") + " · " +
          (status.stage || "Agent") + " · " + Math.round(elapsed / 1000) + "s",
          status.paused ? "warn" : "live");
        assistantTaskControls(true);
        assistantSchedulePoll(1000);
        return;
      }
      if (assistant.jobId && status.job_id === assistant.jobId &&
          ["done", "failed", "aborted"].includes(status.state)) {
        const ok = status.state === "done" && status.result?.ok !== false;
        await assistantPullEvents().catch(() => {});
        await assistantFinishStream(status);
        assistantStatus(ok ? "DONE" : String(status.state || "CHECK").toUpperCase(),
          ok ? "ok" : "bad");
        localStorage.setItem("ea_assistant_last_job", assistant.jobId);
        assistant.jobId = "";
        assistantTaskControls(false);
        assistant.historyLoaded = false;
        return;
      }
      assistant.jobId = "";
      assistantTaskControls(false);
      assistantStatus("IDLE", "");
    } catch (error) {
      assistantStatus("OFFLINE", "bad");
      assistantAddMessage("system", "后台任务状态暂不可用：" + (error.message || error));
    } finally {
      assistant.polling = false;
    }
  }

  async function assistantSyncRun() {
    try {
      const [status] = await Promise.all([
        assistantReadRunStatus(),
        assistantSyncRequests(),
      ]);
      if ((status.busy || ["running", "waiting_request", "paused"].includes(status.state)) && status.job_id) {
        assistant.jobId = String(status.job_id);
        assistantUpdateStream(status);
        if (status.session_id) assistantSetSession(status.session_id);
        await assistantPullEvents().catch(() => {});
        assistantStatus("FOLLOWING · " + (status.stage || status.state), "live");
        assistantTaskControls(true);
        assistantSchedulePoll(100);
      } else {
        assistantStatus("IDLE", "");
        assistantTaskControls(false);
      }
    } catch (error) {
      assistantStatus("OFFLINE", "bad");
    }
  }

  function assistantRequestPending(status) {
    const state = String(status?.state || "").toLowerCase();
    return state === "planned";
  }

  async function assistantResolveRequest(kind, status, approved, response = "") {
    if (kind === "agent") {
      if (!status?.request_id) return;
      const asksAnswer = status.kind === "context" && status.resource === "ask_user";
      const answer = String(response || "").trim();
      if (approved && asksAnswer && !answer) return;
      if (approved && !asksAnswer && !(await UI.confirmAction({
        title: "允许这一次 Agent 请求？",
        message: `${status.resource || "--"}\n风险：${status.risk || "--"}\n原因：${status.reason || "--"}\n参数 hash：${status.argument_hash || "--"}\n\n批准只绑定当前 request ID；参数变化或过期后自动失效。`,
        confirmLabel: "允许一次",
      }))) return;
      await UI.api.durablePost(
        approved ? "/api/agent/requests/decision" : "/api/agent/requests/cancel",
        approved ? {
          request_id: status.request_id,
          decision: "allow_once",
          ...(answer ? { response: answer } : {}),
        } :
          { request_id: status.request_id },
      );
      await assistantSyncRequests();
      document.dispatchEvent(new CustomEvent("exoanchor:agent-request-changed", {
        detail: { source: "assistant", request_id: status.request_id },
      }));
      return;
    }
    const endpoint = kind === "display" ? "/api/host/display" : "/api/host/boot-key";
    const action = approved ? "approve" : "cancel";
    if (!status?.plan_id) return;
    if (approved) {
      const title = kind === "display" ? "批准显示配置请求？" : "批准固件按键请求？";
      const detail = kind === "display" ?
        `${status.target?.connector || "--"} · ${status.target?.width || 0}×${status.target?.height || 0}@${status.target?.refresh_hz || 0}` :
        `${status.profile?.target || "--"} · ${status.profile?.key_code || "--"} · ${status.profile?.trigger || "--"}`;
      if (!(await UI.confirmAction({
        title,
        message: `${detail}\n\n批准只绑定当前设备请求 ${status.plan_id}；参数变化后必须重新批准。`,
        confirmLabel: "批准精确请求",
      }))) return;
    }
    await UI.api.durablePost(endpoint, {
      action,
      plan_id: status.plan_id,
      approved: !!approved,
    });
    await assistantSyncRequests();
  }

  function assistantRenderRequests(requests) {
    const host = byId("eaAssistantRequests");
    if (!host) return;
    host.innerHTML = "";
    for (const request of requests) {
      const card = document.createElement("section");
      card.className = "ea-assistant-request";
      const asksAnswer = request.kind === "agent" &&
        request.status.kind === "context" &&
        request.status.resource === "ask_user";
      if (asksAnswer) card.classList.add("asks-answer");
      const copy = document.createElement("div");
      const title = document.createElement("b");
      const detail = document.createElement("span");
      title.textContent = request.kind === "agent" ? "Agent 请求" :
        request.kind === "display" ? "显示配置请求" : "固件按键请求";
      detail.textContent = request.kind === "agent" ?
        `${request.status.resource || "--"} · ${request.status.risk || "--"} · ${request.status.reason || "--"}` :
        request.kind === "display" ?
        `${request.status.target?.connector || "--"} · ${request.status.target?.width || 0}×${request.status.target?.height || 0}@${request.status.target?.refresh_hz || 0}` :
        `${request.status.profile?.target || "--"} · ${request.status.profile?.key_code || "--"} · ${request.status.profile?.trigger || "--"}`;
      copy.append(title, detail);
      const actions = document.createElement("div");
      const reject = document.createElement("button");
      const approve = document.createElement("button");
      reject.type = approve.type = "button";
      reject.textContent = asksAnswer ? "跳过" : "拒绝";
      approve.textContent = asksAnswer ? "回答" : "批准";
      approve.className = "primary";
      reject.onclick = () => assistantResolveRequest(request.kind, request.status, false)
        .catch(error => assistantAddMessage("system", error.message || "请求取消失败"));
      let answerInput = null;
      if (asksAnswer) {
        answerInput = document.createElement("textarea");
        answerInput.maxLength = 512;
        answerInput.rows = 2;
        answerInput.placeholder = "输入给 Agent 的回答";
        card.append(copy, answerInput);
      }
      approve.onclick = () => {
        if (asksAnswer && !String(answerInput?.value || "").trim()) {
          answerInput?.focus();
          return;
        }
        assistantResolveRequest(
          request.kind, request.status, true, answerInput?.value || "")
        .catch(error => assistantAddMessage("system", error.message || "请求批准失败"));
      };
      actions.append(reject, approve);
      if (!asksAnswer) card.append(copy);
      card.append(actions);
      host.appendChild(card);
    }
    host.classList.toggle("show", requests.length > 0);
  }

  async function assistantSyncRequests() {
    const results = await Promise.allSettled([
      UI.api.getShared("/api/agent/requests", { timeoutMs: 5000 }),
      UI.api.getShared("/api/host/display", { timeoutMs: 5000 }),
      UI.api.getShared("/api/host/boot-key", { timeoutMs: 5000 }),
    ]);
    const requests = [];
    if (results[0].status === "fulfilled" &&
        Array.isArray(results[0].value?.items)) {
      results[0].value.items
        .filter(item => item?.status === "waiting_user")
        .forEach(item => requests.push({ kind: "agent", status: item }));
    }
    if (results[1].status === "fulfilled" &&
        assistantRequestPending(results[1].value)) {
      requests.push({ kind: "display", status: results[1].value });
    }
    if (results[2].status === "fulfilled" &&
        assistantRequestPending(results[2].value)) {
      requests.push({ kind: "boot", status: results[2].value });
    }
    assistantRenderRequests(requests);
    return requests;
  }

  async function assistantSend() {
    const input = byId("eaAssistantInput");
    const send = byId("eaAssistantSend");
    const text = String(input?.value || "").trim();
    if (!text) return;
    if (!(await UI.api.ensure())) return;
    send.disabled = true;
    input.disabled = true;
    try {
      if (assistant.jobId) {
        const steered = await UI.api.post("/api/agent/run/steer", {
          run_id: assistant.jobId,
          message: text,
        });
        if (!steered.accepted) throw new Error("当前任务没有接受补充要求");
        assistantAddMessage("user", text);
        input.value = "";
        assistantStatus("UPDATED · r" + (steered.intent_revision || "--"), "live");
        assistantSchedulePoll(100);
        return;
      }
      assistantSetSession(localStorage.getItem("ea_agent_session"));
      const sessionId = assistantMaterializeSession();
      assistantAddMessage("user", text);
      input.value = "";
      const attach = !!byId("eaAssistantAttachContext")?.checked;
      const pageContext = attach ? (assistant.context || assistantContextCapture()) : null;
      assistantStatus("STARTING", "warn");
      const profile = localStorage.getItem("ea_agent_profile") || "";
      const model = localStorage.getItem("ea_agent_model") || "";
      const started = await UI.api.durablePost("/api/agent/run", {
        session_id: sessionId,
        message: text,
        profile,
        model,
        authority_mode: "policy",
        page_context: pageContext,
      });
      if (!started.accepted && started.busy) {
        assistantAddMessage("system", "设备已有后台 Agent 任务；已切换为跟随该任务，本条消息未发送。");
      }
      assistant.jobId = String(started.job_id || "");
      if (assistant.jobId) assistantEnsureStream(assistant.jobId);
      if (started.session_id) assistantSetSession(started.session_id);
      assistant.context = null;
      assistantRenderContext();
      assistantStatus(started.busy ? "RUNNING · " + (started.stage || "Agent") :
        (started.accepted ? "QUEUED" : "IDLE"), started.busy ? "live" :
        (started.accepted ? "warn" : ""));
      if (assistant.jobId) assistantSchedulePoll(300);
    } catch (error) {
      assistantStatus("CHECK", "bad");
      assistantAddMessage("system", error.message || "消息发送失败");
    } finally {
      send.disabled = false;
      input.disabled = false;
      input.focus();
    }
  }

  function assistantClamp(left, top) {
    const panel = byId("eaAssistantWindow");
    const width = panel?.offsetWidth || 420;
    const height = panel?.offsetHeight || 620;
    return {
      left: Math.max(8, Math.min(window.innerWidth - width - 8, left)),
      top: Math.max(72, Math.min(window.innerHeight - height - 8, top)),
    };
  }

  function assistantApplyPosition() {
    const panel = byId("eaAssistantWindow");
    if (!panel) return;
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem("ea_assistant_position") || "null"); }
    catch (error) {}
    if (!saved || !Number.isFinite(saved.left) || !Number.isFinite(saved.top)) {
      panel.style.removeProperty("left");
      panel.style.removeProperty("top");
      return;
    }
    const next = assistantClamp(saved.left, saved.top);
    panel.style.left = next.left + "px";
    panel.style.top = next.top + "px";
    panel.style.right = "auto";
    panel.style.bottom = "auto";
  }

  function assistantOpen(nextOpen) {
    if (!agentRuntimeEnabled) return;
    assistant.open = nextOpen === undefined ? !assistant.open : !!nextOpen;
    byId("eaAssistantWindow")?.classList.toggle("show", assistant.open);
    byId("eaAssistantLauncher")?.classList.toggle("active", assistant.open);
    byId("eaAssistantLauncher")?.setAttribute("aria-expanded", assistant.open ? "true" : "false");
    if (!assistant.open) {
      clearTimeout(assistant.pollTimer);
      return;
    }
    assistant.page = UI.pageContext.detect();
    assistantApplyPosition();
    if (!assistant.historyLoaded ||
        assistant.sessionId !== assistantSessionId(localStorage.getItem("ea_agent_session"))) {
      assistantLoadHistory().catch(() => {});
    }
    UI.api.ensure().then(ok => {
      if (ok) assistantSyncRun();
    });
    setTimeout(() => byId("eaAssistantInput")?.focus(), 0);
  }

  function assistantStartDrag(event) {
    if (event.button !== 0 || event.target.closest("button,a")) return;
    const panel = byId("eaAssistantWindow");
    const rect = panel.getBoundingClientRect();
    const origin = { x: event.clientX, y: event.clientY, left: rect.left, top: rect.top };
    panel.classList.add("dragging");
    event.currentTarget.setPointerCapture(event.pointerId);
    const move = moveEvent => {
      const next = assistantClamp(origin.left + moveEvent.clientX - origin.x,
        origin.top + moveEvent.clientY - origin.y);
      panel.style.left = next.left + "px";
      panel.style.top = next.top + "px";
      panel.style.right = "auto";
      panel.style.bottom = "auto";
    };
    const finish = () => {
      panel.classList.remove("dragging");
      const position = panel.getBoundingClientRect();
      localStorage.setItem("ea_assistant_position",
        JSON.stringify({ left: position.left, top: position.top }));
      document.removeEventListener("pointermove", move);
      document.removeEventListener("pointerup", finish);
    };
    document.addEventListener("pointermove", move);
    document.addEventListener("pointerup", finish, { once: true });
  }

  function mountAssistant(activePage) {
    assistant.page = activePage;
    document.body.insertAdjacentHTML("beforeend",
      '<button id="eaAssistantLauncher" class="ea-assistant-launcher" type="button" aria-expanded="false" aria-controls="eaAssistantWindow">' +
        '<span aria-hidden="true">' + BRAND_MARK_HTML + '</span><b id="eaAssistantLauncherName"></b>' +
      '</button>' +
      '<section id="eaAssistantWindow" class="ea-assistant-window" role="dialog">' +
        '<header id="eaAssistantDrag" class="ea-assistant-head">' +
          '<div><span class="ea-assistant-mark" aria-hidden="true">' + BRAND_MARK_HTML + '</span><div><b id="eaAssistantName"></b><small id="eaAssistantThread">新对话</small></div></div>' +
          '<nav><a href="/agent" title="前往完整 Agent Workspace">Workspace</a><button id="eaAssistantNew" type="button" title="新对话">＋</button><button id="eaAssistantClose" type="button" title="关闭；后台任务会继续">×</button></nav>' +
        '</header>' +
        '<div class="ea-assistant-runbar"><span id="eaAssistantStatus" class="ea-assistant-status">IDLE</span><span>任务在设备后台持续</span><button id="eaAssistantStop" type="button" hidden>停止</button></div>' +
        '<div id="eaAssistantRequests" class="ea-assistant-requests" aria-live="polite"></div>' +
        '<div id="eaAssistantLog" class="ea-assistant-log"><div class="ea-assistant-empty"><b id="eaAssistantEmptyName"></b><span>从当前页面开始对话。</span></div></div>' +
        '<div id="eaAssistantContextPreview" class="ea-assistant-context"></div>' +
        '<form id="eaAssistantForm" class="ea-assistant-compose">' +
          '<textarea id="eaAssistantInput" rows="3" maxlength="1800" placeholder="问设备、规划任务，或从当前页面继续…"></textarea>' +
          '<div><label><input id="eaAssistantAttachContext" type="checkbox"> 附带当前页面</label><button id="eaAssistantPreviewContext" type="button">预览</button><span></span><button id="eaAssistantSend" class="primary" type="submit">发送</button></div>' +
        '</form>' +
      '</section>');
    applyAssistantName(localStorage.getItem("ea_agent_display_name"));
    byId("eaAssistantLauncher").onclick = () => assistantOpen();
    byId("eaAssistantClose").onclick = () => assistantOpen(false);
    byId("eaAssistantStop").onclick = async () => {
      if (!assistant.jobId) return;
      if (!(await UI.confirmAction({
        title: "停止当前 Agent 任务？",
        message: "后续动作将被取消，已产生的事件和结果仍会保留。",
        confirmLabel: "停止任务",
      }))) return;
      await UI.api.post("/api/agent/run/abort", {run_id: assistant.jobId});
      assistantStatus("STOPPING", "warn");
      assistantSchedulePoll(100);
    };
    byId("eaAssistantNew").onclick = () => {
      assistantSetSession("");
      assistant.jobId = "";
      assistant.historyLoaded = true;
      assistant.context = null;
      assistantRenderHistory([]);
      assistantRenderContext();
      assistantStatus("IDLE", "");
      assistantTaskControls(false);
      byId("eaAssistantInput").focus();
    };
    byId("eaAssistantPreviewContext").onclick = () => {
      if (assistant.context && byId("eaAssistantContextPreview").classList.contains("show")) {
        assistant.context = null;
        assistantRenderContext();
      } else {
        byId("eaAssistantAttachContext").checked = true;
        assistantContextCapture();
      }
    };
    document.addEventListener("exoanchor:agent-data-cleared",
      assistantResetForDataClear);
    document.addEventListener("exoanchor:agent-request-changed", event => {
      if (event.detail?.source !== "assistant") {
        assistantSyncRequests().catch(() => {});
      }
    });
    window.addEventListener("storage", event => {
      if (event.key === "ea_agent_data_clear_generation") {
        assistantResetForDataClear();
      }
    });
    byId("eaAssistantAttachContext").onchange = event => {
      if (event.target.checked) assistantContextCapture();
      else {
        assistant.context = null;
        assistantRenderContext();
      }
    };
    byId("eaAssistantForm").onsubmit = event => {
      event.preventDefault();
      assistantSend();
    };
    byId("eaAssistantInput").onkeydown = event => {
      if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        assistantSend();
      }
    };
    byId("eaAssistantDrag").addEventListener("pointerdown", assistantStartDrag);
    byId("eaAssistantDrag").ondblclick = () => {
      localStorage.removeItem("ea_assistant_position");
      const panel = byId("eaAssistantWindow");
      panel.style.removeProperty("left");
      panel.style.removeProperty("top");
      panel.style.removeProperty("right");
      panel.style.removeProperty("bottom");
    };
    document.addEventListener("keydown", event => {
      if (agentRuntimeEnabled && (event.ctrlKey || event.metaKey) &&
          event.key === "/") {
        event.preventDefault();
        assistantOpen();
      } else if (event.key === "Escape" && assistant.open &&
                 !byId("authModal")?.classList.contains("show")) {
        assistantOpen(false);
      }
    });
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden && assistant.open) assistantSyncRun();
    });
    window.addEventListener("resize", () => {
      if (assistant.open) assistantApplyPosition();
    });
  }

  function mount(activePage) {
    if (mounted) return;
    UI.lifecycle.mount(activePage);
    const host = byId("appShell");
    if (!host) throw new Error("appShell mount point not found");
    const nav = links.map(item => {
      const current = item[0] === activePage;
      if (item[0] === "agent") {
        return '<a id="agentNav" data-agent-href="' + item[1] + '"' +
          ' class="' + (current ? "active " : "") +
          'agent-disabled" aria-disabled="true" tabindex="-1" ' +
          (current ? 'aria-current="page" ' : "") +
          'title="正在读取内置 Agent 状态">' + item[2] + "</a>";
      }
      return '<a' + (current ? ' class="active" aria-current="page"' : "") +
        ' href="' + item[1] + '">' + item[2] + "</a>";
    }).join("");
    host.outerHTML = '<nav class="top ea-shell">' +
      '<div class="brand"><div class="logo" aria-hidden="true">' + BRAND_MARK_HTML + '</div><div class="brand-copy"><div class="title">ExoAnchor</div><div class="sub"><span id="deviceLabelText" class="device-label">读取设备名称…</span></div></div></div>' +
      '<div class="nav">' + nav + '<span id="releaseMeta" class="release-meta">' + CHIP_LABEL + ' ' + RELEASE_NAME + ' v' + DISPLAY_VERSION + '</span></div>' +
      '<div class="status-strip" aria-label="设备概要">' +
        '<div class="status-menu"><button id="badgeVideo" class="data-state neutral" type="button" aria-haspopup="menu" aria-expanded="false" disabled>VIDEO --</button>' +
          '<div id="videoMenu" class="status-menu-list status-toggle-menu hide" role="menu"><div class="status-toggle-row" role="none"><span class="status-toggle-copy"><b>MS 视频电源</b><small>关闭后停止采集，开启后自动恢复</small></span><label class="ui-switch"><input id="videoPowerToggle" type="checkbox" role="switch" aria-label="MS 视频电源" aria-describedby="videoPowerState" disabled><span class="ui-switch-track"><span class="ui-switch-thumb"></span></span><span id="videoPowerState" class="ui-switch-state">不可用</span></label></div></div></div>' +
        '<span id="badgeUsb" class="data-state neutral">USB --</span>' +
        '<div class="status-menu"><button id="badgePwr" class="data-state unknown" type="button" aria-haspopup="menu" aria-expanded="false" disabled>POWER N/A</button>' +
          '<div id="powerMenu" class="status-menu-list hide" role="menu"><button id="powerActionPower" type="button" role="menuitem" disabled>Power</button><button id="powerActionReset" type="button" role="menuitem" disabled>Reset</button></div></div>' +
      '</div><button id="logoutNav" class="shell-logout" type="button">Logout</button></nav>' +
      '<div id="eaAutomationBanner" class="ea-automation-banner" role="status" aria-live="polite" hidden>' +
        '<span class="ea-automation-copy"><b id="eaAutomationTitle">自动操作进行中</b><small id="eaAutomationDetail"></small></span>' +
        '<button id="eaAutomationStop" type="button">立即终止</button>' +
      '</div>' +
      '<div id="shellLogoutModal" class="ui-modal" role="dialog" aria-modal="true"><div class="ui-dialog"><h2>退出本地会话？</h2><p>退出后需要重新登录才能继续操作。</p><div class="actions"><button id="shellLogoutCancel" type="button">取消</button><button id="shellLogoutConfirm" class="danger" type="button">退出</button></div></div></div>';

    mounted = true;
    bindShell();
    UI.auth.mount();
    UI.session.start();
    UI.status.subscribe(renderStatus);
    UI.info.subscribe(renderDeviceLabel);
    renderDeviceLabel(UI.info.value);
    UI.api.ensure().then(ok => {
      if (!ok) return;
      refreshAutomation();
      UI.lifecycle.interval(refreshAutomation, 800);
    }).catch(() => {});
    if (embeddedAgent) {
      mountAssistant(activePage);
      applyAgentAvailability(null, false);
      window.addEventListener("exoanchor:agent-name-changed", event =>
        applyAssistantName(event.detail?.name));
      window.addEventListener("exoanchor:product-features-changed", event =>
        updateProductFeatures(event.detail || {}));
      if (typeof BroadcastChannel === "function") {
        agentFeatureChannel = new BroadcastChannel(
          PRODUCT_FEATURES_CHANNEL_NAME);
        agentFeatureChannel.addEventListener(
          "message", handleAgentFeatureBroadcast);
        window.addEventListener("pagehide", () => {
          agentFeatureChannel?.close();
          agentFeatureChannel = null;
        }, { once: true });
      }
      window.addEventListener("focus", refreshAgentAvailabilityOnResume);
      document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "visible") {
          refreshAgentAvailabilityOnResume();
        }
      });
      UI.api.ensure().then(ok => {
        if (ok) refreshAgentAvailability();
      }).catch(() => {});
    }
  }

  window.ExoAnchorShell = {
    mount,
    updateStatus: renderStatus,
    setAgentName: applyAssistantName,
    updateProductFeatures,
  };
})();

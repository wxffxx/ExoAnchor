(function () {
  "use strict";

  const UI = window.ExoAnchorUI;
  if (!UI) throw new Error("ui-core.js must load before ui-shell.js");

  const links = [
    ["overview", "/", "Overview"],
    ["kvm", "/kvm", "KVM"],
    ["settings", "/settings", "Settings"],
  ];
  let mounted = false;

  function byId(id) { return document.getElementById(id); }

  function setBadge(element, label, state, kind) {
    UI.setStatus(element, label + " " + state, kind);
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

  function renderStatus(status, error) {
    if (error || !status) {
      setBadge(byId("badgeVideo"), "VIDEO", "UNKNOWN", "warn");
      setBadge(byId("badgeUsb"), "USB", "UNKNOWN", "warn");
      setBadge(byId("badgePwr"), "POWER", "UNKNOWN", "warn");
      if (byId("powerMenuButton")) byId("powerMenuButton").disabled = true;
      return;
    }
    const video = status.video || {};
    const hid = status.hid || {};
    const power = status.power || {};
    const detect = power.detect || {};
    const hidMounted = !!(hid.connected || hid.mounted);
    byId("badgeVideo").removeAttribute("title");

    if (video.connected) {
      setBadge(byId("badgeVideo"), "VIDEO", videoModeState(video), "ok");
      const width = Number(video.width || video.target_width || 0);
      const height = Number(video.height || video.target_height || 0);
      byId("badgeVideo").title = width > 0 && height > 0 ? width + "×" + height : "Video online";
    }
    else if (modeCount(video) > 0) setBadge(byId("badgeVideo"), "VIDEO", "STANDBY", "warn");
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

    const available = !!power.available && !power.busy;
    byId("powerMenuButton").disabled = !available;
    byId("powerActionPower").disabled = !(available && power.buttons?.power?.available);
    byId("powerActionReset").disabled = !(available && power.buttons?.reset?.available);
  }

  function renderDeviceLabel(info) {
    const fallback = localStorage.getItem("si_device_label") || "ESP32-P4";
    const label = String(info?.device_label || info?.label || fallback).trim() || fallback;
    localStorage.setItem("si_device_label", label);
    if (byId("deviceLabelText")) byId("deviceLabelText").textContent = label;
  }

  function closePowerMenu() {
    byId("powerMenu")?.classList.add("hide");
    byId("powerMenuButton")?.setAttribute("aria-expanded", "false");
  }

  async function powerAction(action) {
    closePowerMenu();
    byId("powerMenuButton").disabled = true;
    try { await UI.api.post("/api/power/action", { action }); }
    finally { await UI.status.refresh().catch(() => {}); }
  }

  function showLogout() { byId("shellLogoutModal").classList.add("show"); }
  function hideLogout() { byId("shellLogoutModal").classList.remove("show"); }
  function performLogout() {
    hideLogout();
    document.dispatchEvent(new CustomEvent("exoanchor:logout"));
    UI.api.setSession("", UI.api.username);
    window.location.assign("/");
  }

  function bindShell() {
    byId("powerMenuButton").onclick = function () {
      const hidden = byId("powerMenu").classList.toggle("hide");
      byId("powerMenuButton").setAttribute("aria-expanded", hidden ? "false" : "true");
    };
    byId("powerActionPower").onclick = function () { powerAction("power").catch(() => {}); };
    byId("powerActionReset").onclick = function () { powerAction("reset").catch(() => {}); };
    byId("logoutNav").onclick = showLogout;
    byId("shellLogoutCancel").onclick = hideLogout;
    byId("shellLogoutConfirm").onclick = performLogout;
    byId("shellLogoutModal").onclick = event => { if (event.target === byId("shellLogoutModal")) hideLogout(); };
    document.addEventListener("keydown", event => { if (event.key === "Escape") hideLogout(); });
    document.addEventListener("click", event => {
      if (!event.target.closest(".ea-shell .power-menu")) closePowerMenu();
    });
  }

  function mount(activePage) {
    if (mounted) return;
    const host = byId("appShell");
    if (!host) throw new Error("appShell mount point not found");
    const nav = links.map(item => '<a' + (item[0] === activePage ? ' class="active" aria-current="page"' : "") +
      ' href="' + item[1] + '">' + item[2] + "</a>").join("");
    host.outerHTML = '<nav class="top ea-shell">' +
      '<div class="brand"><div class="logo">EA</div><div><div class="title">ESP32P4</div><div class="sub"><span id="deviceLabelText" class="device-label">ESP32-P4</span><span id="deviceVersionText" class="device-version">v' + UI.VERSION + '</span></div></div></div>' +
      '<div class="nav">' + nav + '<button id="logoutNav" type="button">Logout</button><span class="version-pill">v' + UI.VERSION + '</span></div>' +
      '<div class="status-strip" aria-label="设备概要">' +
        '<span id="badgeVideo" class="badge neutral">VIDEO --</span><span id="badgeUsb" class="badge neutral">USB --</span><span id="badgePwr" class="badge unknown">POWER N/A</span>' +
        '<div class="power-menu"><button id="powerMenuButton" type="button" aria-haspopup="menu" aria-expanded="false" disabled>电源 ▾</button>' +
          '<div id="powerMenu" class="power-menu-list hide" role="menu"><button id="powerActionPower" type="button" role="menuitem" disabled>Power</button><button id="powerActionReset" type="button" role="menuitem" disabled>Reset</button></div></div>' +
      '</div></nav>' +
      '<div id="shellLogoutModal" class="ui-modal" role="dialog" aria-modal="true"><div class="ui-dialog"><h2>退出本地会话？</h2><p>退出后需要重新登录才能继续操作。</p><div class="actions"><button id="shellLogoutCancel" type="button">取消</button><button id="shellLogoutConfirm" class="danger" type="button">退出</button></div></div></div>';

    mounted = true;
    bindShell();
    UI.auth.mount();
    UI.session.start();
    UI.status.subscribe(renderStatus);
    UI.info.subscribe(renderDeviceLabel);
    renderDeviceLabel(UI.info.value);
    UI.status.start();
    UI.info.start();
  }

  window.ExoAnchorShell = { mount, updateStatus: renderStatus };
})();

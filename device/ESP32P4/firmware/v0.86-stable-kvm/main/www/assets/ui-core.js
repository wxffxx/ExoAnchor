(function () {
  "use strict";

  const VERSION = "0.86.0-stable-kvm";

  function byId(id) {
    return document.getElementById(id);
  }

  class ApiClient {
    constructor() {
      this.token = localStorage.getItem("si_token") || "";
      this.username = localStorage.getItem("si_username") || "admin";
      this.ws = null;
      this.retry = 1000;
    }

    headers(json = true) {
      const headers = json ? { "Content-Type": "application/json" } : {};
      if (this.token) headers.Authorization = "Bearer " + this.token;
      return headers;
    }

    authHeader() {
      return this.token ? { Authorization: "Bearer " + this.token } : {};
    }

    setSession(token, username) {
      this.token = token || "";
      this.username = username || this.username || "admin";
      if (this.token) {
        localStorage.setItem("si_token", this.token);
        localStorage.setItem("si_auth_last_active", String(Date.now()));
      } else {
        localStorage.removeItem("si_token");
        localStorage.removeItem("si_auth_last_active");
      }
      if (this.username) localStorage.setItem("si_username", this.username);
    }

    async login(username, password) {
      const response = await fetch("/api/auth/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password }),
      });
      if (!response.ok) throw new Error(await response.text());
      const result = await response.json();
      this.setSession(result.token, result.username || username);
      return result;
    }

    async request(method, path, body, promptAuth = true) {
      const response = await fetch(path, {
        method,
        headers: this.headers(body !== undefined),
        body: body === undefined ? undefined : JSON.stringify(body),
        cache: method === "GET" ? "no-store" : undefined,
      });
      if (response.status === 401 && promptAuth && auth.mounted) {
        if (await auth.requireLogin()) return this.request(method, path, body, false);
        throw new Error("login required");
      }
      if (!response.ok) throw new Error(await response.text());
      const type = response.headers.get("Content-Type") || "";
      return type.includes("application/json") ? response.json() : response.text();
    }

    req(method, path, body) {
      if (!session.active()) return Promise.reject(new Error("login required"));
      return this.request(method, path, body, true);
    }

    get(path) {
      return this.req("GET", path);
    }

    post(path, body) {
      return this.req("POST", path, body || {});
    }

    async getSilent(path) {
      return this.request("GET", path, undefined, false);
    }

    async ensure() {
      try {
        const state = await this.getSilent("/api/auth/status");
        if (!state.enabled) return true;
        if (state.must_change_credentials) return auth.requireChange(state);
        if (state.token_valid) return true;
      } catch (error) {}
      return auth.requireLogin();
    }

    ensureToken() {
      return this.ensure();
    }

    upload(path, file, onProgress) {
      if (!session.active()) return Promise.reject(new Error("login required"));
      return new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        request.open("POST", path);
        if (this.token) request.setRequestHeader("Authorization", "Bearer " + this.token);
        request.setRequestHeader("Content-Type", "application/octet-stream");
        request.upload.onprogress = event => {
          if (event.lengthComputable && onProgress) onProgress(event.loaded / event.total);
        };
        request.onload = () => {
          if (request.status >= 200 && request.status < 300) {
            try { resolve(JSON.parse(request.responseText || "{}")); }
            catch (error) { resolve({ ok: true }); }
          } else reject(new Error(request.responseText || request.statusText || "upload failed"));
        };
        request.onerror = () => reject(new Error("upload failed"));
        request.send(file);
      });
    }
  }

  const api = new ApiClient();

  const auth = {
    mounted: false,
    mode: "login",
    pending: null,
    waiting: null,
    state: null,
    onAuthenticated: null,

    mount() {
      if (this.mounted) return;
      document.body.insertAdjacentHTML("beforeend", '<div id="authModal" class="ui-modal modal" role="dialog" aria-modal="true">' +
        '<form id="authForm" class="ui-dialog dialog">' +
          '<h2 id="authTitle">登录本地账户</h2><p id="authHint">需要登录后才能继续。</p>' +
          '<div class="field"><label for="authCurrentUsername">当前用户名</label><input id="authCurrentUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authCurrentPassword">当前密码</label><input id="authCurrentPassword" type="password" autocomplete="current-password"></div>' +
          '<div id="authNewFields"><div class="field"><label for="authNewUsername">新用户名</label><input id="authNewUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authNewPassword">新密码</label><input id="authNewPassword" type="password" autocomplete="new-password" minlength="6" maxlength="64"></div>' +
          '<div class="field"><label for="authConfirmPassword">确认新密码</label><input id="authConfirmPassword" type="password" autocomplete="new-password" minlength="6" maxlength="64"></div></div>' +
          '<div class="actions"><button id="authSubmit" class="primary" type="submit">登录</button></div><div id="authMsg" class="msg"></div>' +
        '</form></div>');
      this.mounted = true;
    },

    show(mode, state) {
      this.mount();
      this.mode = mode;
      this.state = state || {};
      byId("authModal").classList.add("show");
      byId("authNewFields").style.display = mode === "login" ? "none" : "block";
      byId("authTitle").textContent = mode === "login" ? "登录本地账户" : "请修改默认账户";
      byId("authHint").textContent = mode === "login" ? "需要登录后才能继续。" : "当前账户仍使用默认凭据，请先更新。";
      byId("authSubmit").textContent = mode === "login" ? "登录" : "保存并继续";
      byId("authMsg").textContent = "";
      byId("authMsg").className = "msg";
      byId("authCurrentUsername").value = this.state.username || this.state.default_username || api.username || "admin";
      byId("authNewUsername").value = this.state.username || api.username || "admin";
      byId("authCurrentPassword").value = "";
      byId("authNewPassword").value = "";
      byId("authConfirmPassword").value = "";
      setTimeout(() => byId("authCurrentPassword").focus(), 0);
    },

    hide(ok) {
      byId("authModal")?.classList.remove("show");
      if (this.pending) {
        this.pending(ok);
        this.pending = null;
      }
      this.waiting = null;
    },

    requireLogin() {
      if (byId("authModal")?.classList.contains("show") && this.mode === "login" && this.waiting) return this.waiting;
      this.show("login", {});
      this.waiting = new Promise(resolve => { this.pending = resolve; });
      return this.waiting;
    },

    requireChange(state) {
      if (byId("authModal")?.classList.contains("show") && this.mode === "change" && this.waiting) return this.waiting;
      this.show("change", state);
      this.waiting = new Promise(resolve => { this.pending = resolve; });
      return this.waiting;
    },

    async checkRequired() {
      try {
        const state = await api.getSilent("/api/auth/status");
        if (state.enabled && !state.token_valid) {
          return await this.requireLogin();
        }
        if (state.must_change_credentials) {
          return await this.requireChange(state);
        }
        return true;
      } catch (error) {
        return await this.requireLogin();
      }
    },

    bindForm(onAuthenticated) {
      this.mount();
      this.onAuthenticated = onAuthenticated || null;
      byId("authForm").onsubmit = async event => {
        event.preventDefault();
        const message = byId("authMsg");
        message.textContent = "";
        message.className = "msg";
        const currentUsername = byId("authCurrentUsername").value.trim();
        const currentPassword = byId("authCurrentPassword").value;
        try {
          if (this.mode === "login") {
            const result = await api.login(currentUsername, currentPassword);
            if (result.must_change_credentials) {
              this.show("change", result);
              return;
            }
            this.hide(true);
            if (this.onAuthenticated) await this.onAuthenticated(result);
            return;
          }
          const username = byId("authNewUsername").value.trim();
          const nextPassword = byId("authNewPassword").value;
          const confirmPassword = byId("authConfirmPassword").value;
          if (!username) throw new Error("用户名不能为空");
          if (nextPassword !== confirmPassword) throw new Error("两次密码不一致");
          if (!api.token) await api.login(currentUsername, currentPassword);
          const result = await api.post("/api/settings/account", {
            current_username: currentUsername,
            current_password: currentPassword,
            username,
            new_password: nextPassword,
          });
          api.setSession(result.token, result.username || username);
          this.hide(true);
          if (this.onAuthenticated) await this.onAuthenticated(result);
        } catch (error) {
          message.textContent = error.message || "认证失败";
          message.className = "msg bad";
        }
      };
    },
  };

  const session = {
    cfg: { enabled: false, minutes: 15, loaded: false },
    started: false,

    apply(config) {
      this.cfg = {
        enabled: !!(config?.auto_logout_enabled ?? config?.auto_logout ?? config?.enabled),
        minutes: Math.max(1, Math.min(1440, Number(config?.auto_logout_minutes ?? config?.minutes ?? 15))),
        loaded: true,
      };
    },

    enabled() { return !!this.cfg.enabled; },
    minutes() { return Math.max(1, Math.min(1440, Number(this.cfg.minutes || 15))); },

    async load() {
      try { this.apply(await api.getSilent("/api/settings/session")); }
      catch (error) {}
      return this.cfg;
    },

    touch() {
      if (api.token) localStorage.setItem("si_auth_last_active", String(Date.now()));
    },

    active() {
      if (!this.enabled() || !api.token) return true;
      const last = Number(localStorage.getItem("si_auth_last_active") || Date.now());
      if (Date.now() - last <= this.minutes() * 60000) return true;
      api.setSession("", api.username);
      document.dispatchEvent(new CustomEvent("exoanchor:session-expired"));
      if (auth.mounted) auth.requireLogin();
      return false;
    },

    start() {
      if (this.started) return;
      this.started = true;
      ["click", "keydown", "pointerdown", "touchstart"].forEach(name => {
        document.addEventListener(name, () => this.touch(), { passive: true, capture: true });
      });
      this.load();
      setInterval(() => this.active(), 10000);
    },
  };

  class PollStore {
    constructor(path, interval) {
      this.path = path;
      this.interval = interval;
      this.value = null;
      this.error = null;
      this.listeners = new Set();
      this.timer = 0;
      this.inFlight = null;
    }

    subscribe(listener, immediate = true) {
      this.listeners.add(listener);
      if (immediate && this.value) listener(this.value, null);
      return () => this.listeners.delete(listener);
    }

    notify() {
      this.listeners.forEach(listener => listener(this.value, this.error));
    }

    async refresh() {
      if (this.inFlight) return this.inFlight;
      this.inFlight = api.getSilent(this.path).then(value => {
        this.value = value;
        this.error = null;
        this.notify();
        return value;
      }).catch(error => {
        this.error = error;
        this.notify();
        throw error;
      }).finally(() => { this.inFlight = null; });
      return this.inFlight;
    }

    start() {
      if (this.timer) return;
      this.refresh().catch(() => {});
      this.timer = setInterval(() => this.refresh().catch(() => {}), this.interval);
    }
  }

  const status = new PollStore("/api/status", 2000);
  const info = new PollStore("/api/system/info", 10000);

  function setStatus(element, text, kind) {
    if (!element) return;
    element.textContent = text;
    element.className = "badge " + (kind || "neutral");
  }

  function row(name, value) {
    return '<div class="row"><b>' + name + '</b><span>' + value + '</span></div>';
  }

  function message(element, text, kind) {
    if (!element) return;
    element.textContent = text || "";
    element.className = "msg " + (text ? (kind || "ok") : "");
  }

  let confirmResolve = null;

  function closeConfirm(result) {
    byId("uiConfirmModal")?.classList.remove("show");
    if (confirmResolve) {
      confirmResolve(!!result);
      confirmResolve = null;
    }
  }

  function ensureConfirm() {
    if (byId("uiConfirmModal")) return;
    document.body.insertAdjacentHTML("beforeend", '<div id="uiConfirmModal" class="ui-modal" role="dialog" aria-modal="true">' +
      '<div class="ui-dialog"><h2 id="uiConfirmTitle">确认操作</h2><p id="uiConfirmMessage"></p>' +
      '<div class="actions"><button id="uiConfirmCancel" type="button">取消</button><button id="uiConfirmSubmit" class="danger" type="button">确认</button></div></div></div>');
    byId("uiConfirmCancel").onclick = () => closeConfirm(false);
    byId("uiConfirmSubmit").onclick = () => closeConfirm(true);
    byId("uiConfirmModal").onclick = event => { if (event.target === byId("uiConfirmModal")) closeConfirm(false); };
    document.addEventListener("keydown", event => {
      if (event.key === "Escape" && byId("uiConfirmModal")?.classList.contains("show")) closeConfirm(false);
    });
  }

  function confirmAction(options) {
    ensureConfirm();
    if (confirmResolve) closeConfirm(false);
    const config = typeof options === "string" ? { message: options } : (options || {});
    byId("uiConfirmTitle").textContent = config.title || "确认操作";
    byId("uiConfirmMessage").textContent = config.message || "此操作会立即生效。";
    byId("uiConfirmSubmit").textContent = config.confirmLabel || "确认";
    byId("uiConfirmSubmit").classList.toggle("danger", config.danger !== false);
    byId("uiConfirmModal").classList.add("show");
    setTimeout(() => byId("uiConfirmCancel").focus(), 0);
    return new Promise(resolve => { confirmResolve = resolve; });
  }

  window.ExoAnchorUI = {
    VERSION,
    api,
    auth,
    session,
    status,
    info,
    byId,
    setStatus,
    row,
    message,
    confirmAction,
  };
})();

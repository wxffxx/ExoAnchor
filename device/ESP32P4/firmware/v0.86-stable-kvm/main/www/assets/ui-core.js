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

    async setup(username, password) {
      const ticket = await this.getSilent("/api/auth/setup");
      if (!ticket.nonce) throw new Error("无法开始首次配置");
      return this.login(username, password, ticket.nonce);
    }

    async login(username, password, setupNonce = "", otp = "") {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 75000);
      try {
        let response = await fetch(setupNonce ? "/api/auth/setup" : "/api/auth/login", {
          method: "POST", headers: { "Content-Type": "application/json", ...(setupNonce ? { "X-ExoAnchor-Setup": setupNonce } : {}) },
          body: JSON.stringify({ username, password, ...(otp ? { otp } : {}), request_id: crypto.randomUUID() }),
          credentials: "same-origin", signal: controller.signal,
        });
        while (response.status === 202) {
          const pending = await response.json();
          if (!pending.job_id) throw new Error("invalid login job");
          await new Promise(resolve => setTimeout(resolve, 100));
          response = await fetch("/api/auth/login/status?job_id=" + encodeURIComponent(pending.job_id), {
            credentials: "same-origin", cache: "no-store", signal: controller.signal,
          });
        }
        if (!response.ok) throw new Error(await response.text());
        const result = await response.json();
        this.setSession(result.token, result.username || username);
        return result;
      } finally { clearTimeout(timer); }
    }

    async logout() {
      const headers = this.authHeader();
      this.setSession("", this.username);
      document.dispatchEvent(new CustomEvent("exoanchor:logout"));
      await fetch("/api/auth/logout", { method: "POST", headers, credentials: "same-origin" });
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

  const passkeys = {
    controller: null,
    problem() {
      if (!window.isSecureContext || location.protocol !== "https:") return "请使用浏览器信任的 HTTPS 网址。";
      const host = location.hostname;
      if (host.includes(":") || /^[\d.]+$/.test(host)) return "请通过设备主机名访问，通行密钥不能绑定 IP 地址。";
      if (!window.PublicKeyCredential || !navigator.credentials?.create || !navigator.credentials?.get) return "当前浏览器不支持通行密钥，请使用密码登录。";
      return "";
    },
    cancel() { this.controller?.abort(); },
    decode(text) {
      return Uint8Array.from(atob(text.replace(/-/g, "+").replace(/_/g, "/")), c => c.charCodeAt(0));
    },
    encode(bytes) {
      return btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    },
    async post(action, body, signal) {
      const response = await fetch("/api/auth/passkeys", {
        method: "POST", headers: api.headers(), credentials: "same-origin", cache: "no-store",
        body: JSON.stringify({action, ...body}), signal,
      });
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    },
    async perform(action, body = {}) {
      const problem = this.problem(); if (problem) throw new Error(problem);
      if (this.controller) throw new Error("请先完成或取消当前通行密钥操作。");
      if (action === "login" && session.loggedOut?.()) await session.logout();
      const logoutGeneration = localStorage.getItem("ea_auth_logout_generation");
      const controller = new AbortController(); this.controller = controller;
      const cancel = () => controller.abort();
      document.addEventListener("exoanchor:logout", cancel);
      document.addEventListener("exoanchor:session-expired", cancel);
      window.addEventListener("pagehide", cancel);
      const timer = setTimeout(() => controller.abort(), 210000);
      try {
        let result;
        if (action === "register" || action === "login") {
          const create = action === "register";
          const options = await this.post(create ? "register_options" : "login_options", body, controller.signal);
          options.challenge = this.decode(options.challenge);
          if (create) {
            options.user.id = this.decode(options.user.id);
            options.excludeCredentials = (options.excludeCredentials || []).map(k => ({...k, id: this.decode(k.id)}));
          }
          const credential = await navigator.credentials[create ? "create" : "get"]({publicKey: options, signal: controller.signal});
          if (!credential) throw new Error("未返回通行密钥。");
          const data = {type: credential.type, rawId: this.encode(credential.rawId), clientDataJSON: this.encode(credential.response.clientDataJSON)};
          if (create) data.attestationObject = this.encode(credential.response.attestationObject);
          else {
            data.authenticatorData = this.encode(credential.response.authenticatorData);
            data.signature = this.encode(credential.response.signature);
            data.userHandle = credential.response.userHandle ? this.encode(credential.response.userHandle) : "";
          }
          if (controller.signal.aborted) throw new DOMException("已取消", "AbortError");
          result = await this.post(create ? "register_finish" : "login_finish", data, controller.signal);
        } else result = await this.post(action, body, controller.signal);
        if (controller.signal.aborted || logoutGeneration !== localStorage.getItem("ea_auth_logout_generation"))
          throw new DOMException("登录已取消", "AbortError");
        if (action === "login") api.setSession(result.token, result.username);
        return result;
      } catch (error) {
        if (error.name === "NotAllowedError" || error.name === "AbortError") throw new Error("操作已取消或超时；可以重新尝试或使用密码登录。");
        if (error.name === "InvalidStateError") throw new Error("此验证器已添加过通行密钥。");
        throw error;
      } finally {
        clearTimeout(timer); this.controller = null;
        document.removeEventListener("exoanchor:logout", cancel);
        document.removeEventListener("exoanchor:session-expired", cancel);
        window.removeEventListener("pagehide", cancel);
        body.current_password = body.otp = "";
      }
    },
  };

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
          '<div id="authCurrentFields"><div class="field"><label for="authCurrentUsername">当前用户名</label><input id="authCurrentUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authCurrentPassword">当前密码</label><input id="authCurrentPassword" type="password" autocomplete="current-password"></div>' +
          '<div class="field"><label for="authOtp">验证码 / 恢复码（已启用两步验证时填写）</label><input id="authOtp" type="text" autocomplete="one-time-code" maxlength="32" spellcheck="false"></div></div>' +
          '<div id="authNewFields"><div class="field"><label for="authNewUsername">新用户名</label><input id="authNewUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authNewPassword">新密码</label><input id="authNewPassword" type="password" autocomplete="new-password" minlength="6" maxlength="64"></div>' +
          '<div class="field"><label for="authConfirmPassword">确认新密码</label><input id="authConfirmPassword" type="password" autocomplete="new-password" minlength="6" maxlength="64"></div></div>' +
          '<div class="actions"><button id="authSubmit" class="primary" type="submit">登录</button></div><button id="authPasskey" type="button">使用通行密钥登录</button><p id="authPasskeyHint" role="status"></p><div id="authMsg" class="msg"></div>' +
        '</form></div>');
      this.mounted = true;
    },

    show(mode, state) {
      this.mount();
      this.mode = mode;
      this.state = state || {};
      byId("authModal").classList.add("show");
      byId("authNewFields").style.display = mode === "login" ? "none" : "block";
      byId("authCurrentFields").style.display = mode === "setup" ? "none" : "block";
      byId("authTitle").textContent = mode === "setup" ? "创建管理员账号" : mode === "login" ? "登录本地账户" : "请修改默认账户";
      byId("authHint").textContent = mode === "setup" ? "在可信的本地网络完成首次配置。" : mode === "login" ? "需要登录后才能继续。" : "当前账户仍使用默认凭据，请先更新。";
      byId("authPasskey").hidden = mode !== "login" || this.state.passkey?.enabled === false;
      const passkeyProblem = passkeys.problem();
      byId("authPasskey").disabled = !!passkeyProblem;
      byId("authPasskeyHint").textContent = mode === "login" && !byId("authPasskey").hidden ? passkeyProblem : "";
      byId("authSubmit").disabled = false;
      byId("authSubmit").textContent = mode === "setup" ? "创建并继续" : mode === "login" ? "登录" : "保存并继续";
      if (mode === "login" && this.state.two_step_storage_ok === false) {
        byId("authHint").textContent = "两步验证存储异常，认证已锁定。请检查设备存储或使用本地恢复流程。";
      } else if (mode === "login" && this.state.two_step_enabled && this.state.two_step_time_ready === false) {
        byId("authHint").textContent = "设备正在等待网络校时。请稍后使用动态验证码，或使用一次性恢复码登录。";
      }
      byId("authMsg").textContent = "";
      byId("authMsg").className = "msg";
      byId("authCurrentUsername").value = this.state.username || this.state.default_username || api.username || "admin";
      byId("authNewUsername").value = this.state.username || api.username || "admin";
      byId("authCurrentPassword").value = "";
      byId("authOtp").value = "";
      byId("authNewPassword").value = "";
      byId("authConfirmPassword").value = "";
      setTimeout(() => byId(mode === "setup" ? "authNewUsername" : "authCurrentPassword").focus(), 0);
    },

    hide(ok) {
      passkeys.cancel();
      if (byId("authOtp")) byId("authOtp").value = "";
      byId("authModal")?.classList.remove("show");
      if (this.pending) {
        this.pending(ok);
        this.pending = null;
      }
      this.waiting = null;
    },

    requireLogin(state = {}) {
      if (byId("authModal")?.classList.contains("show") && this.mode === "login" && this.waiting) return this.waiting;
      this.show("login", state);
      this.waiting = new Promise(resolve => { this.pending = resolve; });
      return this.waiting;
    },

    requireSetup(state) {
      if (byId("authModal")?.classList.contains("show") && this.mode === "setup" && this.waiting) return this.waiting;
      this.show("setup", state);
      if (!this.waiting) this.waiting = new Promise(resolve => { this.pending = resolve; });
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
        if (state.setup_required) return await this.requireSetup(state);
        if (state.enabled && !state.token_valid) {
          return await this.requireLogin(state);
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
      byId("authPasskey").onclick = async () => {
        const button = byId("authPasskey"), message = byId("authMsg");
        button.disabled = byId("authSubmit").disabled = true;
        byId("authCurrentPassword").value = byId("authOtp").value = "";
        message.textContent = "请在系统提示中确认通行密钥…"; message.className = "msg";
        try {
          const result = await passkeys.perform("login");
          this.hide(true);
          if (this.onAuthenticated) await this.onAuthenticated(result);
        } catch (error) { message.textContent = error.message; message.className = "msg bad"; }
        finally { button.disabled = !!passkeys.problem(); byId("authSubmit").disabled = false; }
      };

      byId("authForm").onsubmit = async event => {
        event.preventDefault();
        passkeys.cancel();
        const message = byId("authMsg");
        message.textContent = "";
        message.className = "msg";
        const currentUsername = byId("authCurrentUsername").value.trim();
        const currentPassword = byId("authCurrentPassword").value;
        const otp = byId("authOtp").value.trim();
        byId("authOtp").value = "";
        try {
          if (this.mode === "login") {
            const result = await api.login(currentUsername, currentPassword, "", otp);
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
          if (this.mode === "setup") {
            const result = await api.setup(username, nextPassword);
            this.hide(true);
            if (this.onAuthenticated) await this.onAuthenticated(result);
            return;
          }
          await api.login(currentUsername, currentPassword, "", otp);
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
      api.logout().catch(() => {});
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
    passkeys,
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

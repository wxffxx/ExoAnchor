(function () {
  "use strict";

  const VERSION = String(window.ExoAnchorBuild?.version || "0.87.6-dev");
  const FEATURES = Object.freeze({
    embeddedAgent: window.ExoAnchorBuild?.embeddedAgent !== false,
    uartTerminal: window.ExoAnchorBuild?.uartTerminal !== false,
    h264Video: window.ExoAnchorBuild?.h264Video === true,
    externalMcp: window.ExoAnchorBuild?.externalMcp !== false,
  });

  function byId(id) {
    return document.getElementById(id);
  }

  class ApiClient {
    constructor() {
      const legacySessionToken = sessionStorage.getItem("ea_auth_token") || "";
      this.token = localStorage.getItem("ea_auth_token") || legacySessionToken;
      if (this.token) localStorage.setItem("ea_auth_token", this.token);
      sessionStorage.removeItem("ea_auth_token");
      if (sessionStorage.getItem("ea_auth_confirmed") === "1") {
        localStorage.setItem("ea_auth_confirmed", "1");
      }
      sessionStorage.removeItem("ea_auth_confirmed");
      this.username = localStorage.getItem("si_username") || "admin";
      this.ws = null;
      this.retry = 1000;
      this.sharedGets = new Map();
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
        localStorage.setItem("ea_auth_token", this.token);
        localStorage.setItem("si_auth_last_active", String(Date.now()));
        localStorage.setItem("ea_auth_confirmed", "1");
      } else {
        localStorage.removeItem("ea_auth_token");
        localStorage.removeItem("si_auth_last_active");
        localStorage.removeItem("ea_auth_confirmed");
      }
      if (this.username) localStorage.setItem("si_username", this.username);
    }

    async login(username, password) {
      const requestId = globalThis.crypto?.randomUUID?.().replaceAll("-", "") ||
        (Date.now().toString(16) + Math.random().toString(16).slice(2));
      const request = () => fetch("/api/auth/login", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ username, password, request_id: requestId }),
          cache: "no-store",
          credentials: "same-origin",
        });
      let response;
      try {
        response = await request();
      } catch (error) {
        await new Promise(resolve => setTimeout(resolve, 180));
        response = await request();
      }
      if (response.status === 202) {
        let pending = await response.json();
        const jobId = String(pending.job_id || "");
        if (!jobId) throw new Error("login job was not created");
        const deadline = Date.now() + 30000;
        while (Date.now() < deadline) {
          await new Promise(resolve =>
            setTimeout(resolve, Math.max(50, Number(pending.poll_after_ms) || 100)));
          response = await fetch("/api/auth/login/status?job_id=" +
            encodeURIComponent(jobId), {
              cache: "no-store",
              credentials: "same-origin",
            });
          if (response.status !== 202) break;
          pending = await response.json();
        }
        if (response.status === 202) throw new Error("login timed out");
      }
      if (!response.ok) throw new Error(await response.text());
      const result = await response.json();
      this.setSession(result.token, result.username || username);
      return result;
    }

    async request(method, path, body, promptAuth = true, options = {}) {
      const baseSignal = options.signal === undefined ? lifecycle.signal() : options.signal;
      const configuredTimeout = options.timeoutMs;
      const timeoutMs = Math.max(
        0,
        Number(configuredTimeout === undefined && method === "GET" ?
          8000 : configuredTimeout) || 0
      );
      const requestOptions = {
          method,
          headers: this.headers(body !== undefined),
          body: body === undefined ? undefined : JSON.stringify(body),
          cache: method === "GET" ? "no-store" : undefined,
          credentials: "same-origin",
        };
      if (options.keepalive) requestOptions.keepalive = true;
      const request = async () => {
        const attemptOptions = { ...requestOptions };
        let controller = null;
        let timer = 0;
        let timedOut = false;
        let forwardAbort = null;
        if (timeoutMs > 0) {
          controller = new AbortController();
          if (baseSignal?.aborted) {
            controller.abort();
          } else if (baseSignal) {
            forwardAbort = () => controller.abort();
            baseSignal.addEventListener("abort", forwardAbort, { once: true });
          }
          timer = setTimeout(() => {
            timedOut = true;
            controller.abort();
          }, timeoutMs);
          attemptOptions.signal = controller.signal;
        } else if (baseSignal) {
          attemptOptions.signal = baseSignal;
        }
        try {
          return await fetch(path, attemptOptions);
        } catch (error) {
          if (timedOut) {
            const timeoutError = new Error("request timed out");
            timeoutError.name = "TimeoutError";
            throw timeoutError;
          }
          throw error;
        } finally {
          if (timer) clearTimeout(timer);
          if (baseSignal && forwardAbort) {
            baseSignal.removeEventListener("abort", forwardAbort);
          }
        }
      };
      let response;
      try {
        response = await request();
      } catch (error) {
        if (method !== "GET" ||
            error?.name === "AbortError" ||
            error?.name === "TimeoutError") throw error;
        await new Promise(resolve => setTimeout(resolve, 120));
        response = await request();
      }
      if (response.status === 401 && promptAuth && auth.mounted) {
        if (await auth.requireLogin()) {
          return this.request(method, path, body, false, options);
        }
        throw new Error("login required");
      }
      if (!response.ok) throw new Error(await response.text());
      const type = response.headers.get("Content-Type") || "";
      return type.includes("application/json") ? response.json() : response.text();
    }

    req(method, path, body, options) {
      if (!session.active()) return Promise.reject(new Error("login required"));
      return this.request(method, path, body, true, options);
    }

    get(path, options) {
      return this.req("GET", path, undefined, options);
    }

    getShared(path, options = {}) {
      const key = String(path);
      const current = this.sharedGets.get(key);
      if (current) return current;
      const request = this.get(path, options).finally(() => {
        if (this.sharedGets.get(key) === request) {
          this.sharedGets.delete(key);
        }
      });
      this.sharedGets.set(key, request);
      return request;
    }

    post(path, body, options) {
      return this.req("POST", path, body || {}, options);
    }

    durablePost(path, body) {
      return this.req("POST", path, body || {}, {
        signal: null,
        keepalive: true,
        timeoutMs: 8000,
      });
    }

    async getSilent(path, options) {
      return this.request("GET", path, undefined, false, options);
    }

    async ensure() {
      return auth.checkRequired();
    }

    ensureToken() {
      return this.ensure();
    }

    upload(path, file, onProgress) {
      if (!session.active()) return Promise.reject(new Error("login required"));
      return new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        const release = lifecycle.addCleanup(() => request.abort());
        const finish = callback => value => {
          release();
          callback(value);
        };
        request.open("POST", path);
        request.withCredentials = true;
        if (this.token) request.setRequestHeader("Authorization", "Bearer " + this.token);
        request.setRequestHeader("Content-Type", "application/octet-stream");
        request.upload.onprogress = event => {
          if (event.lengthComputable && onProgress) onProgress(event.loaded / event.total);
        };
        request.onload = () => {
          if (request.status >= 200 && request.status < 300) {
            try { finish(resolve)(JSON.parse(request.responseText || "{}")); }
            catch (error) { finish(resolve)({ ok: true }); }
          } else {
            finish(reject)(
              new Error(request.responseText || request.statusText || "upload failed")
            );
          }
        };
        request.onerror = () => finish(reject)(new Error("upload failed"));
        request.onabort = () => finish(reject)(
          new DOMException("upload cancelled by page lifecycle", "AbortError")
        );
        request.send(file);
      });
    }
  }

  const api = new ApiClient();

  const lifecycle = {
    page: "",
    mounted: false,
    destroyed: false,
    reason: "",
    controller: new AbortController(),
    cleanups: new Set(),
    timers: new Set(),

    signal() {
      return this.controller.signal;
    },

    emit(name, detail = {}) {
      document.dispatchEvent(new CustomEvent("exoanchor:" + name, {
        detail: Object.assign({ page: this.page }, detail),
      }));
    },

    mount(page) {
      if (page) this.page = String(page);
      if (this.mounted) return this;
      this.mounted = true;
      document.addEventListener("visibilitychange", () => {
        this.emit("visible", { visible: !document.hidden });
      });
      window.addEventListener("pagehide", event => {
        this.destroy("pagehide", { persisted: !!event.persisted });
      });
      window.addEventListener("beforeunload", () => this.destroy("beforeunload"));
      window.addEventListener("pageshow", event => {
        if (event.persisted) {
          // Resource owners are one-shot by design. A BFCache restore gets a
          // fresh document so every page re-establishes the same contracts.
          location.reload();
        }
      });
      this.emit("mount");
      return this;
    },

    addCleanup(callback) {
      if (typeof callback !== "function") return () => {};
      if (this.destroyed) {
        try { callback(this.reason || "destroyed"); } catch (error) {}
        return () => {};
      }
      this.cleanups.add(callback);
      return () => this.cleanups.delete(callback);
    },

    interval(callback, delay) {
      const timer = window.setInterval(callback, delay);
      this.timers.add(["interval", timer]);
      return timer;
    },

    timeout(callback, delay) {
      let entry;
      const timer = window.setTimeout(() => {
        this.timers.delete(entry);
        callback();
      }, delay);
      entry = ["timeout", timer];
      this.timers.add(entry);
      return timer;
    },

    clearTimer(timer) {
      for (const entry of this.timers) {
        if (entry[1] !== timer) continue;
        if (entry[0] === "interval") window.clearInterval(timer);
        else window.clearTimeout(timer);
        this.timers.delete(entry);
        return;
      }
      window.clearTimeout(timer);
      window.clearInterval(timer);
    },

    navigate(href) {
      this.emit("navigate", { href: String(href || "") });
      this.destroy("navigate");
    },

    logout() {
      this.emit("logout");
    },

    reconnect(detail = {}) {
      if (this.destroyed) return;
      this.emit("reconnect", detail);
    },

    destroy(reason = "destroy", detail = {}) {
      if (this.destroyed) return;
      this.destroyed = true;
      this.reason = reason;
      this.emit("destroy", Object.assign({ reason }, detail));
      this.controller.abort(reason);
      for (const entry of this.timers) {
        if (entry[0] === "interval") window.clearInterval(entry[1]);
        else window.clearTimeout(entry[1]);
      }
      this.timers.clear();
      const callbacks = Array.from(this.cleanups).reverse();
      this.cleanups.clear();
      callbacks.forEach(callback => {
        try { callback(reason); } catch (error) {
          console.error("page lifecycle cleanup failed", error);
        }
      });
    },
  };

  const auth = {
    mounted: false,
    mode: "login",
    pending: null,
    waiting: null,
    state: null,
    onAuthenticated: null,
    authProbe: null,

    mount() {
      if (this.mounted) return;
      document.body.insertAdjacentHTML("beforeend", '<div id="authModal" class="ui-modal" role="dialog" aria-modal="true">' +
        '<form id="authForm" class="ui-dialog">' +
          '<div class="auth-brand"><img src="/assets/exoanchor-ui-mark.svg" alt=""><span>ExoAnchor</span></div>' +
          '<h2 id="authTitle">登录本地账户</h2><p id="authHint">需要登录后才能继续。</p>' +
          '<div class="field"><label for="authCurrentUsername">当前用户名</label><input id="authCurrentUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authCurrentPassword">当前密码</label><input id="authCurrentPassword" type="password" autocomplete="current-password"></div>' +
          '<div id="authNewFields"><div class="field"><label for="authNewUsername">新用户名</label><input id="authNewUsername" type="text" autocomplete="username" maxlength="32"></div>' +
          '<div class="field"><label for="authNewPassword">新密码（至少 6 位）</label><input id="authNewPassword" type="password" autocomplete="new-password" minlength="6" maxlength="64"></div>' +
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
      byId("authHint").textContent = mode === "login" ?
        (this.state.must_change_credentials ?
          "首次使用请以默认账户 admin / admin 登录，随后设置新密码。" :
          "需要登录后才能继续。") :
        "当前账户仍使用默认凭据，请设置至少六位的新密码。";
      byId("authSubmit").textContent = mode === "login" ? "登录" : "保存并继续";
      byId("authMsg").textContent = "";
      byId("authMsg").className = "msg";
      byId("authCurrentUsername").value = this.state.username || this.state.default_username || api.username || "admin";
      byId("authNewUsername").value = this.state.username || api.username || "admin";
      byId("authCurrentPassword").value = "";
      byId("authNewPassword").value = "";
      byId("authConfirmPassword").value = "";
      status.stop();
      info.stop();
      setTimeout(() => byId("authCurrentPassword").focus(), 0);
    },

    hide(ok) {
      byId("authModal")?.classList.remove("show");
      if (ok) {
        status.start();
        info.start();
      }
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

    requireChange(state) {
      if (byId("authModal")?.classList.contains("show") && this.mode === "change" && this.waiting) return this.waiting;
      this.show("change", state);
      this.waiting = new Promise(resolve => { this.pending = resolve; });
      return this.waiting;
    },

    finishAuthentication(result) {
      const hadWaitingCaller = !!this.pending;
      this.hide(true);
      session.load().catch(() => {});
      if (!hadWaitingCaller && this.onAuthenticated) {
        Promise.resolve(this.onAuthenticated(result)).catch(error => {
          console.error("post-auth page initialization failed", error);
        });
      }
    },

    async readAuthState() {
      if (this.authProbe) return this.authProbe;
      this.authProbe = (async () => {
        let delay = 100;
        while (true) {
          try {
            return await api.getSilent("/api/auth/status");
          } catch (error) {
            // A navigation abort, a temporarily full socket queue, or a network
            // transition is not proof that the session is invalid. Keep the
            // current UI/session and retry until the visible page can ask the
            // device authoritatively.
            if (document.hidden) return null;
            await new Promise(resolve => setTimeout(resolve, delay));
            delay = Math.min(Math.round(delay * 1.7), 1000);
          }
        }
      })();
      try {
        return await this.authProbe;
      } finally {
        this.authProbe = null;
      }
    },

    async checkRequired() {
      let state = await this.readAuthState();
      if (!state) return false;
      const bootId = String(state.boot_id || "");
      const previousBootId = sessionStorage.getItem("ea_server_boot_id") || "";
      if (bootId) sessionStorage.setItem("ea_server_boot_id", bootId);
      if (bootId && previousBootId && previousBootId !== bootId) {
        location.reload();
        return false;
      }
      if (!state.enabled) {
        session.apply(state);
        status.start();
        info.start();
        return true;
      }
      if (state.must_change_credentials && state.token_valid) {
        return await this.requireChange(state);
      }
      if (!state.token_valid &&
          localStorage.getItem("ea_auth_confirmed") === "1") {
        // A full-page navigation cancels old requests and closes streams while
        // the new document starts. Do not turn one transient anonymous probe
        // into a login modal; require three authoritative anonymous responses.
        for (const delay of [80, 180]) {
          await new Promise(resolve => setTimeout(resolve, delay));
          state = await this.readAuthState();
          if (!state || state.token_valid) break;
        }
      }
      if (!state.token_valid) {
        api.setSession("", api.username);
        return await this.requireLogin(state);
      }
      localStorage.setItem("ea_auth_confirmed", "1");
      session.apply(state);
      status.start();
      info.start();
      return true;
    },

    bindForm(onAuthenticated) {
      this.mount();
      this.onAuthenticated = onAuthenticated || null;
      byId("authForm").onsubmit = async event => {
        event.preventDefault();
        const message = byId("authMsg");
        const submit = byId("authSubmit");
        message.textContent = "";
        message.className = "msg";
        submit.disabled = true;
        submit.textContent = this.mode === "login" ? "正在登录…" : "正在保存…";
        const currentUsername = byId("authCurrentUsername").value.trim();
        const currentPassword = byId("authCurrentPassword").value;
        try {
          if (this.mode === "login") {
            const result = await api.login(currentUsername, currentPassword);
            if (result.must_change_credentials) {
              this.show("change", result);
              byId("authCurrentPassword").value = currentPassword;
              return;
            }
            localStorage.setItem("ea_auth_confirmed", "1");
            this.finishAuthentication(result);
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
          this.finishAuthentication(result);
        } catch (error) {
          if (this.mode === "change" &&
              (error instanceof TypeError || /fetch|network/i.test(String(error?.message || "")))) {
            const username = byId("authNewUsername").value.trim();
            const nextPassword = byId("authNewPassword").value;
            try {
              const state = await api.getSilent("/api/auth/status");
              if (state.enabled && !state.must_change_credentials &&
                  !state.using_default && state.username === username) {
                const result = await api.login(username, nextPassword);
                this.finishAuthentication(result);
                return;
              }
            } catch (reconcileError) {}
            message.textContent = "设备可能已经保存新凭据。请刷新页面并使用新密码登录。";
          } else {
            message.textContent = error.message || "认证失败";
          }
          message.className = "msg bad";
        } finally {
          if (byId("authModal")?.classList.contains("show")) {
            submit.disabled = false;
            submit.textContent = this.mode === "login" ? "登录" : "保存并继续";
          }
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
      lifecycle.interval(() => this.active(), 10000);
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
      this.lastDurationMs = null;
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
      const startedAt = performance.now();
      this.inFlight = api.getSilent(this.path).then(value => {
        this.lastDurationMs = Math.max(0, performance.now() - startedAt);
        this.value = value;
        this.error = null;
        this.notify();
        return value;
      }).catch(error => {
        this.lastDurationMs = null;
        this.error = error;
        this.notify();
        throw error;
      }).finally(() => { this.inFlight = null; });
      return this.inFlight;
    }

    start() {
      if (this.timer) return;
      const tick = () => {
        if (document.hidden || byId("authModal")?.classList.contains("show")) return;
        this.refresh().catch(() => {});
      };
      tick();
      this.timer = lifecycle.interval(tick, this.interval);
    }

    stop() {
      if (this.timer) lifecycle.clearTimer(this.timer);
      this.timer = 0;
    }
  }

  const status = new PollStore("/api/status", 2000);
  const info = new PollStore("/api/system/info", 10000);

  function setStatus(element, text, kind) {
    if (!element) return;
    element.textContent = text;
    element.className = "data-state " + (kind || "neutral");
  }

  function row(name, value) {
    return '<div class="row"><b>' + name + '</b><span>' + value + '</span></div>';
  }

  function message(element, text, kind) {
    if (!element) return;
    element.textContent = text || "";
    element.className = "msg " + (text ? (kind || "ok") : "");
  }

  const pageContext = {
    schemaVersion: 1,
    pages: new Set(["overview", "kvm", "agent", "terminal", "settings"]),
    providers: new Map(),

    detect() {
      const path = String(location.pathname || "/").toLowerCase();
      if (path.includes("kvm")) return "kvm";
      if (path.includes("agent")) return "agent";
      if (path.includes("terminal")) return "terminal";
      if (path.includes("settings")) return "settings";
      return "overview";
    },

    route(page) {
      return ({
        overview: "/",
        kvm: "/kvm",
        agent: "/agent",
        terminal: "/terminal",
        settings: "/settings",
      })[page] || "/";
    },

    newId() {
      if (window.crypto?.randomUUID) return window.crypto.randomUUID();
      return "ctx-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10);
    },

    register(page, provider) {
      if (!this.pages.has(page) || typeof provider !== "function") return () => {};
      this.providers.set(page, provider);
      return () => {
        if (this.providers.get(page) === provider) this.providers.delete(page);
      };
    },

    build({ page, summary = "", items = [], context_id = "" } = {}) {
      const selectedPage = this.pages.has(page) ? page : this.detect();
      const cleanItems = (Array.isArray(items) ? items : []).slice(0, 8).map(item => ({
        kind: String(item?.kind || "selection").slice(0, 32),
        label: String(item?.label || "Context").slice(0, 64),
        value: String(item?.value ?? "").slice(0, 384),
      }));
      return {
        schema_version: this.schemaVersion,
        page: selectedPage,
        page_id: selectedPage,
        route: this.route(selectedPage),
        context_id: String(context_id || this.newId()).slice(0, 64),
        captured_ms: Date.now(),
        summary: String(summary || "").slice(0, 256),
        items: cleanItems,
      };
    },

    capture(page) {
      const selectedPage = this.pages.has(page) ? page : this.detect();
      let value = null;
      try { value = this.providers.get(selectedPage)?.(); }
      catch (error) { console.error("page context provider failed", error); }
      if (!value || typeof value !== "object") {
        value = {
          page: selectedPage,
          summary: "当前位于 " + selectedPage + " 页面",
          items: [{ kind: "status", label: "Page", value: selectedPage }],
        };
      }
      return this.build(Object.assign({}, value, { page: selectedPage }));
    },
  };

  const actionMirror = {
    available: FEATURES.embeddedAgent,
    subscribers: new Set(),
    timer: 0,
    inFlight: false,
    jobId: "",
    latestSeq: 0,

    subscribe(listener) {
      if (!this.available || typeof listener !== "function") return () => {};
      this.subscribers.add(listener);
      this.start();
      return () => {
        this.subscribers.delete(listener);
        if (!this.subscribers.size) this.stop();
      };
    },

    async poll() {
      if (!this.available) {
        this.stop();
        return;
      }
      if (this.inFlight || !this.subscribers.size || document.hidden ||
          byId("authModal")?.classList.contains("show")) return;
      this.inFlight = true;
      try {
        const eventsPath = afterSeq => "/api/agent/run/events?after_seq=" +
          encodeURIComponent(String(afterSeq || 0));
        let statusValue = await api.getSilent(
          eventsPath(this.latestSeq), { timeoutMs: 5000 });
        const jobId = String(statusValue?.job_id || "");
        if (jobId !== this.jobId) {
          this.jobId = jobId;
          this.latestSeq = 0;
          if (Number(statusValue?.after_seq || 0) !== 0) {
            statusValue = await api.getSilent(
              eventsPath(0), { timeoutMs: 5000 });
          }
        }
        const events = Array.isArray(statusValue?.events) ? statusValue.events : [];
        for (const event of events) {
          const seq = Number(event?.seq || 0);
          if (!seq || seq <= this.latestSeq) continue;
          this.latestSeq = seq;
          this.subscribers.forEach(listener => {
            try { listener(event, statusValue); } catch (error) {}
          });
        }
        this.latestSeq = Math.max(
          this.latestSeq,
          Number(statusValue?.next_after_seq || statusValue?.latest_seq || 0)
        );
      } catch (error) {
      } finally {
        this.inFlight = false;
      }
    },

    start() {
      if (!this.available || this.timer) return;
      this.poll();
      this.timer = lifecycle.interval(() => this.poll(), 800);
    },

    stop() {
      if (this.timer) lifecycle.clearTimer(this.timer);
      this.timer = 0;
      this.inFlight = false;
    },
  };

  const accessMode = {
    mode: "manual",
    definitions: Object.freeze({
      manual: {
        label: "手动授权",
        description: "有副作用的动作逐项等待浏览器确认",
      },
      assisted: {
        label: "替我审批",
        description: "自动批准低、中风险动作；高风险仍需确认",
      },
      full: {
        label: "完全访问",
        description: "策略和控制租约内自动批准全部风险等级",
      },
    }),
    subscribers: new Set(),
    channel: typeof BroadcastChannel === "function" ?
      new BroadcastChannel("exoanchor-access-mode") : null,

    valid(mode) {
      return Object.prototype.hasOwnProperty.call(this.definitions, mode);
    },

    emit(mode) {
      if (this.valid(mode)) this.mode = mode;
      this.subscribers.forEach(listener => {
        try { listener(this.mode); } catch (error) {}
      });
      document.dispatchEvent(new CustomEvent("exoanchor:access-mode", {
        detail: { mode: this.mode },
      }));
    },

    async load() {
      const value = await api.getShared("/api/settings/access-mode", {
        timeoutMs: 5000,
      });
      this.emit(this.valid(value?.mode) ? value.mode : "manual");
      return value;
    },

    async set(mode) {
      if (!this.valid(mode)) throw new Error("invalid access mode");
      const value = await api.post("/api/settings/access-mode", { mode }, {
        timeoutMs: 8000,
      });
      const confirmed = this.valid(value?.mode) ? value.mode : mode;
      this.emit(confirmed);
      this.channel?.postMessage({ mode: confirmed });
      return value;
    },

    bind(select, detail, onError) {
      if (!select) return () => {};
      const render = mode => {
        const value = this.valid(mode) ? mode : "manual";
        if (document.activeElement !== select) select.value = value;
        select.title = this.definitions[value].description;
        if (detail) detail.textContent = this.definitions[value].description;
      };
      this.subscribers.add(render);
      render(this.mode);
      const change = async () => {
        const previous = this.mode;
        const next = select.value;
        if (next === "full") {
          const approved = await confirmAction({
            title: "启用完全访问",
            message: "Agent 与 MCP 将可在既定策略和控制租约内自动执行高风险动作。正在运行的自动操作会先终止。",
            confirmLabel: "启用完全访问",
          });
          if (!approved) {
            select.value = previous;
            return;
          }
        }
        select.disabled = true;
        try {
          await this.set(next);
        } catch (error) {
          select.value = previous;
          if (typeof onError === "function") onError(error);
        } finally {
          select.disabled = false;
        }
      };
      select.addEventListener("change", change);
      if (session.active()) {
        void this.load().catch(error => {
          if (typeof onError === "function") onError(error);
        });
      }
      return () => {
        select.removeEventListener("change", change);
        this.subscribers.delete(render);
      };
    },
  };
  if (accessMode.channel) {
    accessMode.channel.unref?.();
    accessMode.channel.onmessage = event => {
      if (accessMode.valid(event?.data?.mode))
        accessMode.emit(event.data.mode);
    };
    window.addEventListener?.("pagehide", () => {
      accessMode.channel?.close();
      accessMode.channel = null;
    }, { once: true });
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
    features: FEATURES,
    api,
    lifecycle,
    auth,
    session,
    status,
    info,
    byId,
    setStatus,
    row,
    message,
    confirmAction,
    pageContext,
    actionMirror,
    accessMode,
  };
})();

#ifdef CONFIG_HTTPD_WS_SUPPORT
#define TERMINAL_WS_CLOSE_RETRY_MS 50

static esp_timer_handle_t s_terminal_ws_close_timer;

/* With a revoked session given, marks the channel revoked when it belongs to
 * that session; without one, picks up a channel already marked.  A revoked
 * channel gets no further output and is asked to close.  Returns false when
 * the close could not be queued (httpd's control queue was full or the lock
 * was busy), so the caller retries. */
static bool terminal_ws_close_revoked(SemaphoreHandle_t lock,
                                      httpd_handle_t *handle, int *fd,
                                      bool *revoked,
                                      const uint32_t *generation,
                                      const char *session,
                                      const char *revoked_session,
                                      uint32_t revoked_generation)
{
    httpd_handle_t close_handle = NULL;
    int close_fd = -1;
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(
                    revoked_session ? 1000 : 20)) != pdTRUE) {
        if (revoked_session) {
            ESP_LOGW(TAG, "Terminal WebSocket revocation skipped: lock busy");
        }
        return revoked_session != NULL;
    }
    if (*handle && *fd >= 0 &&
        (revoked_session ? *generation == revoked_generation &&
                               strcmp(session, revoked_session) == 0
                         : *revoked)) {
        *revoked = true;
        close_handle = *handle;
        close_fd = *fd;
    }
    xSemaphoreGive(lock);
    return !close_handle ||
           httpd_sess_trigger_close(close_handle, close_fd) == ESP_OK;
}

static bool terminal_ws_close_all(const char *revoked_session,
                                  uint32_t revoked_generation)
{
    bool done = terminal_ws_close_revoked(
        s_ssh_ws_lock, &s_ssh_ws_handle, &s_ssh_ws_fd, &s_ssh_ws_revoked,
        &s_ssh_ws_generation, s_ssh_ws_session_id, revoked_session,
        revoked_generation);
#if SI_CFG_UART_TERMINAL_ENABLED
    done = terminal_ws_close_revoked(
               s_uart_ws_lock, &s_uart_ws_handle, &s_uart_ws_fd,
               &s_uart_ws_revoked, &s_uart_ws_generation,
               s_uart_ws_session_id, revoked_session, revoked_generation) &&
           done;
#endif
    return done;
}

static void terminal_ws_close_retry(void *arg)
{
    (void)arg;
    if (!terminal_ws_close_all(NULL, 0)) {
        (void)esp_timer_start_once(s_terminal_ws_close_timer,
                                   TERMINAL_WS_CLOSE_RETRY_MS * 1000ULL);
    }
}
#endif

static void terminal_ws_auth_session_revoked(const char *session_id,
                                             uint32_t generation,
                                             void *context)
{
    (void)context;
    if (!session_id) {
        return;
    }
#ifdef CONFIG_HTTPD_WS_SUPPORT
    if (!terminal_ws_close_all(session_id, generation) &&
        s_terminal_ws_close_timer) {
        (void)esp_timer_start_once(s_terminal_ws_close_timer,
                                   TERMINAL_WS_CLOSE_RETRY_MS * 1000ULL);
    }
#else
    (void)generation;
#endif
}

static esp_err_t terminal_ws_session_start(void)
{
#ifdef CONFIG_HTTPD_WS_SUPPORT
    if (!s_terminal_ws_close_timer) {
        const esp_timer_create_args_t args = {
            .callback = terminal_ws_close_retry,
            .name = "term_ws_close",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_terminal_ws_close_timer),
                            TAG, "create terminal close timer");
    }
#endif
    return si_auth_add_session_revoked_listener(
        terminal_ws_auth_session_revoked, NULL);
}

// SSH transport service.
#include "ssh_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "ssh_hostkey_store.h"

#include "libssh2_config.h"
#include "libssh2.h"

static const char *TAG = "si-ssh";

#define SI_SSH_TASK_STACK_SIZE 65536
#define SI_SSH_DEFAULT_TIMEOUT_MS 30000
#define SI_SSH_MAX_TIMEOUT_MS 600000
#define SI_SSH_SHELL_INPUT_BUFFER_SIZE 4096
#define SI_SSH_SHELL_IO_CHUNK 512
#define SI_SSH_IO_WAIT_SLICE_MS 250U

static void ssh_secure_clear(void *data, size_t len);

static void bytes_to_hex(const uint8_t *bytes, size_t len,
                         char *out, size_t out_size)
{
    if (!bytes || !out || out_size < len * 2U + 1U) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        snprintf(out + i * 2U, 3, "%02x", bytes[i]);
    }
    out[len * 2U] = '\0';
}

static esp_err_t ssh_verify_host_key(LIBSSH2_SESSION *session,
                                     const char *host, uint16_t port)
{
    const uint8_t *fingerprint = (const uint8_t *)libssh2_hostkey_hash(
        session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!fingerprint || !host || !host[0]) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    char fingerprint_hex[65] = {0};
    bytes_to_hex(fingerprint, 32, fingerprint_hex, sizeof(fingerprint_hex));

    char identity[SI_SSH_HOST_MAX_LEN + 8];
    snprintf(identity, sizeof(identity), "%s:%u", host, (unsigned)port);
    uint8_t identity_hash[32] = {0};
    if (mbedtls_sha256((const uint8_t *)identity, strlen(identity),
                       identity_hash, 0) != 0) {
        return ESP_FAIL;
    }
    char key[15] = "h";
    bytes_to_hex(identity_hash, 6, key + 1, sizeof(key) - 1);
    ssh_secure_clear(identity_hash, sizeof(identity_hash));

    bool trusted_new = false;
    esp_err_t ret = si_ssh_hostkey_verify_or_trust(
        key, fingerprint_hex, &trusted_new);
    ssh_secure_clear(fingerprint_hex, sizeof(fingerprint_hex));
    ssh_secure_clear(key, sizeof(key));
    ssh_secure_clear(identity, sizeof(identity));
    if (ret == ESP_OK && trusted_new) {
        ESP_LOGW(TAG, "Trusted first SSH host key for %s:%u",
                 host, (unsigned)port);
    }
    return ret;
}

static void ssh_secure_clear(void *data, size_t len)
{
    static void *(*const volatile memset_fn)(void *, int, size_t) = memset;
    if (data && len > 0) {
        (void)memset_fn(data, 0, len);
    }
}

typedef struct {
    si_ssh_exec_config_t config;
    si_ssh_exec_result_t *result;
    SemaphoreHandle_t done;
    esp_err_t ret;
} ssh_task_ctx_t;

struct si_ssh_shell {
    si_ssh_shell_config_t config;
    si_ssh_shell_output_cb_t output_cb;
    void *user_ctx;
    StreamBufferHandle_t input;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool running;
    esp_err_t ret;
};

static SemaphoreHandle_t s_ssh_lock;

static esp_err_t ensure_ssh_lock(void)
{
    if (!s_ssh_lock) {
        s_ssh_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ssh_lock != NULL, ESP_ERR_NO_MEM, TAG, "create ssh lock");
    }
    return ESP_OK;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_error(si_ssh_exec_result_t *result, const char *message)
{
    if (result) {
        strlcpy(result->error, message ? message : "ssh error", sizeof(result->error));
    }
}

static void set_libssh2_error(si_ssh_exec_result_t *result, LIBSSH2_SESSION *session,
                              int rc, const char *fallback)
{
    result->ssh_rc = rc;
    char *err = NULL;
    int err_len = 0;
    if (session) {
        (void)libssh2_session_last_error(session, &err, &err_len, 0);
    }
    if (err && err_len > 0) {
        size_t len = (size_t)MIN(err_len, SI_SSH_ERROR_MAX_LEN - 1);
        memcpy(result->error, err, len);
        result->error[len] = '\0';
    } else {
        snprintf(result->error, sizeof(result->error), "%s (%d)",
                 fallback ? fallback : "libssh2 error", rc);
    }
}

static void append_output(si_ssh_exec_result_t *result, const char *data, size_t len)
{
    if (!result || !result->output || result->output_size == 0 || !data || len == 0) {
        return;
    }
    if (result->output_len >= result->output_size - 1) {
        result->truncated = true;
        return;
    }
    size_t room = result->output_size - 1 - result->output_len;
    size_t copy_len = MIN(room, len);
    memcpy(result->output + result->output_len, data, copy_len);
    result->output_len += copy_len;
    result->output[result->output_len] = '\0';
    if (copy_len < len) {
        result->truncated = true;
    }
}

static bool deadline_valid(uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms()) > 0;
}

static bool ssh_exec_cancel_requested(const si_ssh_exec_config_t *config)
{
    return config && config->cancel_cb &&
           config->cancel_cb(config->cancel_user_ctx);
}

static int wait_fd(int sock, LIBSSH2_SESSION *session, uint32_t deadline_ms)
{
    uint32_t current = now_ms();
    if ((int32_t)(deadline_ms - current) <= 0) {
        return 0;
    }

    uint32_t wait_ms = MIN(deadline_ms - current, SI_SSH_IO_WAIT_SLICE_MS);
    struct timeval timeout = {
        .tv_sec = (time_t)(wait_ms / 1000U),
        .tv_usec = (suseconds_t)((wait_ms % 1000U) * 1000U),
    };
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
        timeout.tv_usec = 1000;
    }

    fd_set fd;
    fd_set *readfd = NULL;
    fd_set *writefd = NULL;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);

    int dir = session ? libssh2_session_block_directions(session) :
                        (LIBSSH2_SESSION_BLOCK_INBOUND | LIBSSH2_SESSION_BLOCK_OUTBOUND);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) {
        readfd = &fd;
    }
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
        writefd = &fd;
    }
    if (!readfd && !writefd) {
        readfd = &fd;
        writefd = &fd;
    }
    return select(sock + 1, readfd, writefd, NULL, &timeout);
}

static int connect_with_timeout(int sock, const struct sockaddr *addr,
                                socklen_t addr_len, uint32_t timeout_ms,
                                si_ssh_exec_cancel_cb_t cancel_cb,
                                void *cancel_user_ctx)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    int rc = connect(sock, addr, addr_len);
    if (rc == 0) {
        return 0;
    }
    if (errno != EINPROGRESS) {
        return -1;
    }

    uint32_t deadline_ms = now_ms() + timeout_ms;
    while (deadline_valid(deadline_ms)) {
        if (cancel_cb && cancel_cb(cancel_user_ctx)) {
            errno = ECANCELED;
            return -1;
        }
        uint32_t remaining_ms = deadline_ms - now_ms();
        uint32_t wait_ms = MIN(remaining_ms, SI_SSH_IO_WAIT_SLICE_MS);
        struct timeval tv = {
            .tv_sec = (time_t)(wait_ms / 1000U),
            .tv_usec = (suseconds_t)((wait_ms % 1000U) * 1000U),
        };
        if (tv.tv_sec == 0 && tv.tv_usec == 0) {
            tv.tv_usec = 1000;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0) {
            break;
        }
        if (rc < 0) {
            return -1;
        }
    }
    if (rc <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    int err = 0;
    socklen_t err_len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
        errno = err ? err : errno;
        return -1;
    }
    /* libssh2 is driven in non-blocking mode below.  Restoring the socket to
     * blocking here lets handshake/auth/read calls ignore our deadline and
     * makes WebSocket disconnect wait forever after a failed connection. */
    return 0;
}

static int ssh_authenticate(LIBSSH2_SESSION *session, int sock, const char *username,
                            const char *password, bool use_private_key,
                            const char *public_key, const char *private_key,
                            const char *key_passphrase, uint32_t deadline_ms,
                            si_ssh_exec_cancel_cb_t cancel_cb,
                            void *cancel_user_ctx)
{
    if (!session || !username || !username[0]) {
        return LIBSSH2_ERROR_INVAL;
    }

    int rc = 0;
    if (use_private_key && private_key && private_key[0]) {
        const char *pub = public_key && public_key[0] ? public_key : NULL;
        size_t pub_len = pub ? strlen(pub) : 0;
        const char *passphrase = key_passphrase && key_passphrase[0] ? key_passphrase : NULL;
        while ((rc = libssh2_userauth_publickey_frommemory(
                    session,
                    username,
                    strlen(username),
                    pub,
                    pub_len,
                    private_key,
                    strlen(private_key),
                    passphrase)) == LIBSSH2_ERROR_EAGAIN &&
               deadline_valid(deadline_ms)) {
            if (cancel_cb && cancel_cb(cancel_user_ctx)) {
                return LIBSSH2_ERROR_SOCKET_DISCONNECT;
            }
            wait_fd(sock, session, deadline_ms);
        }
        return rc;
    }

    while ((rc = libssh2_userauth_password(session, username, password ? password : "")) ==
               LIBSSH2_ERROR_EAGAIN &&
           deadline_valid(deadline_ms)) {
        if (cancel_cb && cancel_cb(cancel_user_ctx)) {
            return LIBSSH2_ERROR_SOCKET_DISCONNECT;
        }
        wait_fd(sock, session, deadline_ms);
    }
    return rc;
}

static bool ssh_exec_write_channel(LIBSSH2_SESSION *session, int sock,
                                   LIBSSH2_CHANNEL *channel,
                                   const char *data, size_t len,
                                   uint32_t deadline_ms,
                                   si_ssh_exec_result_t *result,
                                   const si_ssh_exec_config_t *config)
{
    size_t written = 0;
    while (written < len && deadline_valid(deadline_ms)) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            return false;
        }
        ssize_t rc = libssh2_channel_write(channel, data + written, len - written);
        if (rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            wait_fd(sock, session, deadline_ms);
            continue;
        }
        set_libssh2_error(result, session, (int)rc, "stdin write failed");
        return false;
    }
    if (written < len) {
        set_error(result, "stdin write timeout");
        return false;
    }
    return true;
}

static esp_err_t ssh_exec_worker(const si_ssh_exec_config_t *config,
                                 si_ssh_exec_result_t *result)
{
    if (!config || !result || !result->output || result->output_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t timeout_ms = config->timeout_ms ? config->timeout_ms : SI_SSH_DEFAULT_TIMEOUT_MS;
    timeout_ms = MIN(timeout_ms, SI_SSH_MAX_TIMEOUT_MS);
    uint32_t deadline = now_ms() + timeout_ms;
    if (ssh_exec_cancel_requested(config)) {
        set_error(result, "ssh command cancelled");
        return ESP_ERR_INVALID_STATE;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, config->host, &addr) != 1) {
        set_error(result, "host must be an IPv4 address");
        return ESP_ERR_INVALID_ARG;
    }

    int rc = libssh2_init(0);
    if (rc != 0) {
        snprintf(result->error, sizeof(result->error), "libssh2_init failed (%d)", rc);
        result->ssh_rc = rc;
        return ESP_FAIL;
    }

    int sock = -1;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *channel = NULL;
    esp_err_t ret = ESP_FAIL;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        snprintf(result->error, sizeof(result->error), "socket failed errno=%d", errno);
        goto done;
    }

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(config->port ? config->port : 22),
        .sin_addr = addr,
    };
    if (connect_with_timeout(sock, (struct sockaddr *)&sin, sizeof(sin),
                             MIN(timeout_ms, 10000U), config->cancel_cb,
                             config->cancel_user_ctx) != 0) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
        } else {
            snprintf(result->error, sizeof(result->error),
                     "connect failed errno=%d", errno);
        }
        goto done;
    }

    session = libssh2_session_init();
    if (!session) {
        set_error(result, "libssh2_session_init failed");
        goto done;
    }
    libssh2_session_set_blocking(session, 0);

    while ((rc = libssh2_session_handshake(session, sock)) == LIBSSH2_ERROR_EAGAIN &&
           deadline_valid(deadline)) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        wait_fd(sock, session, deadline);
    }
    if (rc != 0) {
        set_libssh2_error(result, session, rc,
                          deadline_valid(deadline) ? "handshake failed" : "handshake timeout");
        goto done;
    }
    esp_err_t hostkey_ret =
        ssh_verify_host_key(session, config->host, config->port);
    if (hostkey_ret != ESP_OK) {
        if (hostkey_ret == ESP_ERR_INVALID_CRC) {
            set_error(result, "SSH host key changed");
        } else {
            snprintf(result->error, sizeof(result->error),
                     "SSH host key verification failed: %s",
                     esp_err_to_name(hostkey_ret));
        }
        goto done;
    }

    rc = ssh_authenticate(session, sock, config->username, config->password,
                          config->use_private_key, config->public_key,
                          config->private_key, config->key_passphrase, deadline,
                          config->cancel_cb, config->cancel_user_ctx);
    if (rc != 0) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
        } else {
            set_libssh2_error(
                result, session, rc,
                deadline_valid(deadline) ?
                (config->use_private_key ? "public key auth failed" :
                                           "password auth failed") :
                "auth timeout");
        }
        goto done;
    }

    do {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        channel = libssh2_channel_open_session(session);
        if (channel) {
            break;
        }
        rc = libssh2_session_last_error(session, NULL, NULL, 0);
        if (rc != LIBSSH2_ERROR_EAGAIN || !deadline_valid(deadline)) {
            set_libssh2_error(result, session, rc, "open channel failed");
            goto done;
        }
        wait_fd(sock, session, deadline);
    } while (true);

    while ((rc = libssh2_channel_exec(channel, config->command)) == LIBSSH2_ERROR_EAGAIN &&
           deadline_valid(deadline)) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        wait_fd(sock, session, deadline);
    }
    if (rc != 0) {
        set_libssh2_error(result, session, rc,
                          deadline_valid(deadline) ? "exec failed" : "exec timeout");
        goto done;
    }

    if (config->stdin_data[0]) {
        if (!ssh_exec_write_channel(session, sock, channel, config->stdin_data,
                                    strlen(config->stdin_data), deadline, result,
                                    config)) {
            if (ssh_exec_cancel_requested(config)) {
                ret = ESP_ERR_INVALID_STATE;
            }
            goto done;
        }
        if (config->close_stdin_after_write) {
            while ((rc = libssh2_channel_send_eof(channel)) == LIBSSH2_ERROR_EAGAIN &&
                   deadline_valid(deadline)) {
                if (ssh_exec_cancel_requested(config)) {
                    set_error(result, "ssh command cancelled");
                    ret = ESP_ERR_INVALID_STATE;
                    goto done;
                }
                wait_fd(sock, session, deadline);
            }
            if (rc != 0) {
                set_libssh2_error(result, session, rc, "stdin eof failed");
                goto done;
            }
        }
    }

    while (deadline_valid(deadline)) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        char buffer[512];
        ssize_t nread = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (nread > 0) {
            append_output(result, buffer, (size_t)nread);
            if (config->output_cb) {
                config->output_cb(buffer, (size_t)nread, config->output_user_ctx);
            }
            continue;
        }
        ssize_t nerr = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));
        if (nerr > 0) {
            append_output(result, buffer, (size_t)nerr);
            if (config->output_cb) {
                config->output_cb(buffer, (size_t)nerr, config->output_user_ctx);
            }
            continue;
        }
        if (nread == LIBSSH2_ERROR_EAGAIN || nerr == LIBSSH2_ERROR_EAGAIN) {
            wait_fd(sock, session, deadline);
            continue;
        }
        break;
    }

    if (!deadline_valid(deadline)) {
        set_error(result, "command timeout");
        ret = ESP_ERR_TIMEOUT;
        goto done;
    }

    while ((rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN &&
           deadline_valid(deadline)) {
        if (ssh_exec_cancel_requested(config)) {
            set_error(result, "ssh command cancelled");
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        wait_fd(sock, session, deadline);
    }
    result->exit_status = libssh2_channel_get_exit_status(channel);
    result->ok = (rc == 0 && result->exit_status == 0);
    result->ssh_rc = rc;
    ret = ESP_OK;

done:
    if (channel) {
        libssh2_channel_free(channel);
    }
    if (session) {
        (void)libssh2_session_disconnect(session, "ExoAnchor shutdown");
        libssh2_session_free(session);
    }
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
    libssh2_exit();
    return ret;
}

static void ssh_task(void *arg)
{
    ssh_task_ctx_t *ctx = (ssh_task_ctx_t *)arg;
    ctx->ret = ssh_exec_worker(&ctx->config, ctx->result);
    xSemaphoreGive(ctx->done);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

esp_err_t si_ssh_exec(const si_ssh_exec_config_t *config, si_ssh_exec_result_t *result)
{
    if (!config || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_ssh_lock(), TAG, "ensure ssh lock");
    if (xSemaphoreTake(s_ssh_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        set_error(result, "ssh busy");
        return ESP_ERR_INVALID_STATE;
    }

    memset(result->error, 0, sizeof(result->error));
    result->ok = false;
    result->exit_status = -1;
    result->ssh_rc = 0;
    result->output_len = 0;
    result->truncated = false;
    if (result->output && result->output_size) {
        result->output[0] = '\0';
    }

    ssh_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        xSemaphoreGive(s_ssh_lock);
        return ESP_ERR_NO_MEM;
    }
    ctx->config = *config;
    ctx->result = result;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        xSemaphoreGive(s_ssh_lock);
        return ESP_ERR_NO_MEM;
    }

    int64_t started_us = esp_timer_get_time();
    TaskHandle_t task = NULL;
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(
        ssh_task, "si_ssh_exec", SI_SSH_TASK_STACK_SIZE,
        ctx, tskIDLE_PRIORITY + 2, &task, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        xSemaphoreGive(s_ssh_lock);
        set_error(result, "create ssh task failed");
        return ESP_ERR_NO_MEM;
    }

    uint32_t timeout_ms = config->timeout_ms ? config->timeout_ms : SI_SSH_DEFAULT_TIMEOUT_MS;
    timeout_ms = MIN(timeout_ms, SI_SSH_MAX_TIMEOUT_MS);
    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(timeout_ms + 15000U)) != pdTRUE) {
        ESP_LOGW(TAG, "SSH task exceeded wait window, waiting for cleanup");
        (void)xSemaphoreTake(ctx->done, portMAX_DELAY);
        if (result->error[0] == '\0') {
            set_error(result, "ssh task timeout");
        }
        ctx->ret = ESP_ERR_TIMEOUT;
    }

    /* Delete from the owner task. Non-self WithCaps deletion needs no
     * temporary internal cleanup task and therefore cannot abort on OOM. */
    vTaskDeleteWithCaps(task);
    esp_err_t ret = ctx->ret;
    result->elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (result->error[0] == '\0' && ret != ESP_OK) {
        set_error(result, esp_err_to_name(ret));
    }
    vSemaphoreDelete(ctx->done);
    memset(&ctx->config, 0, sizeof(ctx->config));
    free(ctx);
    xSemaphoreGive(s_ssh_lock);
    return ret;
}

static void shell_emit(si_ssh_shell_t *shell, const void *data, size_t len)
{
    if (!shell || !shell->output_cb || !data || len == 0) {
        return;
    }
    (void)shell->output_cb((const uint8_t *)data, len, shell->user_ctx);
}

static void shell_emit_str(si_ssh_shell_t *shell, const char *text)
{
    if (text) {
        shell_emit(shell, text, strlen(text));
    }
}

static void shell_emit_error(si_ssh_shell_t *shell, LIBSSH2_SESSION *session,
                             int rc, const char *fallback)
{
    char message[SI_SSH_ERROR_MAX_LEN + 32] = {0};
    char *err = NULL;
    int err_len = 0;
    if (session) {
        (void)libssh2_session_last_error(session, &err, &err_len, 0);
    }
    if (err && err_len > 0) {
        size_t copy_len = (size_t)MIN(err_len, SI_SSH_ERROR_MAX_LEN - 1);
        char detail[SI_SSH_ERROR_MAX_LEN] = {0};
        memcpy(detail, err, copy_len);
        snprintf(message, sizeof(message), "\r\n[ssh: %s]\r\n", detail);
    } else {
        snprintf(message, sizeof(message), "\r\n[ssh: %s (%d)]\r\n",
                 fallback ? fallback : "libssh2 error", rc);
    }
    shell_emit_str(shell, message);
}

static bool shell_wait_retry(int sock, LIBSSH2_SESSION *session, uint32_t deadline)
{
    if (!deadline_valid(deadline)) {
        return false;
    }
    (void)wait_fd(sock, session, deadline);
    return deadline_valid(deadline);
}

static bool shell_write_channel(si_ssh_shell_t *shell, int sock, LIBSSH2_SESSION *session,
                                LIBSSH2_CHANNEL *channel, const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len && !shell->stop_requested) {
        ssize_t rc = libssh2_channel_write(channel, (const char *)data + written, len - written);
        if (rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            (void)wait_fd(sock, session, now_ms() + 100U);
            continue;
        }
        shell_emit_error(shell, session, (int)rc, "channel write failed");
        return false;
    }
    return written == len;
}

static bool ssh_shell_cancel_requested(void *user_ctx)
{
    const si_ssh_shell_t *shell = (const si_ssh_shell_t *)user_ctx;
    return shell && shell->stop_requested;
}

static esp_err_t ssh_shell_worker(si_ssh_shell_t *shell)
{
    uint32_t timeout_ms = shell->config.timeout_ms ? shell->config.timeout_ms : SI_SSH_DEFAULT_TIMEOUT_MS;
    timeout_ms = MIN(timeout_ms, SI_SSH_MAX_TIMEOUT_MS);
    uint32_t deadline = now_ms() + timeout_ms;

    struct in_addr addr;
    if (inet_pton(AF_INET, shell->config.host, &addr) != 1) {
        shell_emit_str(shell, "\r\n[ssh: host must be an IPv4 address]\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    int rc = libssh2_init(0);
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "\r\n[ssh: libssh2_init failed (%d)]\r\n", rc);
        shell_emit_str(shell, msg);
        return ESP_FAIL;
    }

    int sock = -1;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *channel = NULL;
    esp_err_t ret = ESP_FAIL;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "\r\n[ssh: socket failed errno=%d]\r\n", errno);
        shell_emit_str(shell, msg);
        goto done;
    }

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(shell->config.port ? shell->config.port : 22),
        .sin_addr = addr,
    };
    if (connect_with_timeout(sock, (struct sockaddr *)&sin, sizeof(sin),
                             MIN(timeout_ms, 5000U),
                             ssh_shell_cancel_requested, shell) != 0) {
        char msg[72];
        snprintf(msg, sizeof(msg), "\r\n[ssh: connect failed errno=%d]\r\n", errno);
        shell_emit_str(shell, msg);
        goto done;
    }

    session = libssh2_session_init();
    if (!session) {
        shell_emit_str(shell, "\r\n[ssh: libssh2_session_init failed]\r\n");
        goto done;
    }
    libssh2_session_set_blocking(session, 0);

    while (!shell->stop_requested &&
           (rc = libssh2_session_handshake(session, sock)) == LIBSSH2_ERROR_EAGAIN &&
           shell_wait_retry(sock, session, deadline)) {
    }
    if (shell->stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (rc != 0) {
        shell_emit_error(shell, session, rc,
                         deadline_valid(deadline) ? "handshake failed" : "handshake timeout");
        goto done;
    }
    esp_err_t hostkey_ret = ssh_verify_host_key(
        session, shell->config.host, shell->config.port);
    if (hostkey_ret != ESP_OK) {
        if (hostkey_ret == ESP_ERR_INVALID_CRC) {
            shell_emit_str(shell, "\r\n[ssh: host key changed]\r\n");
        } else {
            char msg[112];
            snprintf(msg, sizeof(msg),
                     "\r\n[ssh: host key verification failed: %s]\r\n",
                     esp_err_to_name(hostkey_ret));
            shell_emit_str(shell, msg);
        }
        ret = hostkey_ret;
        goto done;
    }

    bool used_private_key = shell->config.use_private_key;
    rc = ssh_authenticate(session, sock, shell->config.username, shell->config.password,
                          used_private_key, shell->config.public_key,
                          shell->config.private_key, shell->config.key_passphrase,
                          deadline, ssh_shell_cancel_requested, shell);
    ssh_secure_clear(shell->config.password, sizeof(shell->config.password));
    ssh_secure_clear(shell->config.public_key, sizeof(shell->config.public_key));
    ssh_secure_clear(shell->config.private_key, sizeof(shell->config.private_key));
    ssh_secure_clear(shell->config.key_passphrase, sizeof(shell->config.key_passphrase));
    if (shell->stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (rc != 0) {
        shell_emit_error(shell, session, rc,
                         deadline_valid(deadline) ?
                         (used_private_key ? "public key auth failed" : "password auth failed") :
                         "auth timeout");
        goto done;
    }

    do {
        channel = libssh2_channel_open_session(session);
        if (channel) {
            break;
        }
        rc = libssh2_session_last_error(session, NULL, NULL, 0);
        if (rc != LIBSSH2_ERROR_EAGAIN || !shell_wait_retry(sock, session, deadline)) {
            shell_emit_error(shell, session, rc, "open channel failed");
            goto done;
        }
    } while (!shell->stop_requested);

    if (shell->stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    (void)libssh2_channel_handle_extended_data2(channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);

    const char *term = "xterm-256color";
    uint16_t cols = shell->config.cols ? shell->config.cols : 120;
    uint16_t rows = shell->config.rows ? shell->config.rows : 32;
    while (!shell->stop_requested &&
           (rc = libssh2_channel_request_pty_ex(channel, term, (unsigned int)strlen(term),
                                                NULL, 0, cols, rows, 0, 0)) ==
               LIBSSH2_ERROR_EAGAIN &&
           shell_wait_retry(sock, session, deadline)) {
    }
    if (shell->stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (rc != 0) {
        shell_emit_error(shell, session, rc, "pty request failed");
        goto done;
    }

    while (!shell->stop_requested &&
           (rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN &&
           shell_wait_retry(sock, session, deadline)) {
    }
    if (shell->stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (rc != 0) {
        shell_emit_error(shell, session, rc, "shell request failed");
        goto done;
    }

    shell->running = true;
    ret = ESP_OK;
    shell_emit_str(shell, "\r\n[ssh: connected]\r\n");

    uint8_t io[SI_SSH_SHELL_IO_CHUNK];
    while (!shell->stop_requested && !libssh2_channel_eof(channel)) {
        bool did_work = false;
        while (!shell->stop_requested) {
            ssize_t nread = libssh2_channel_read(channel, (char *)io, sizeof(io));
            if (nread > 0) {
                shell_emit(shell, io, (size_t)nread);
                did_work = true;
                continue;
            }
            if (nread == LIBSSH2_ERROR_EAGAIN || nread == 0) {
                break;
            }
            shell_emit_error(shell, session, (int)nread, "channel read failed");
            goto done;
        }

        size_t input_len = xStreamBufferReceive(shell->input, io, sizeof(io), 0);
        if (input_len > 0) {
            did_work = true;
            if (!shell_write_channel(shell, sock, session, channel, io, input_len)) {
                goto done;
            }
        }

        if (!did_work) {
            (void)wait_fd(sock, session, now_ms() + 50U);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

done:
    shell->running = false;
    if (channel) {
        (void)libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
    if (session) {
        (void)libssh2_session_disconnect(session, "ExoAnchor shell shutdown");
        libssh2_session_free(session);
    }
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
    libssh2_exit();
    if (!shell->stop_requested) {
        shell_emit_str(shell, "\r\n[ssh: session closed]\r\n");
    }
    return ret;
}

static void ssh_shell_task(void *arg)
{
    si_ssh_shell_t *shell = (si_ssh_shell_t *)arg;
    shell->ret = ssh_shell_worker(shell);
    xSemaphoreGive(s_ssh_lock);
    xSemaphoreGive(shell->done);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

esp_err_t si_ssh_shell_start(const si_ssh_shell_config_t *config,
                             si_ssh_shell_output_cb_t output_cb,
                             void *user_ctx,
                             si_ssh_shell_t **out_shell)
{
    if (!config || !out_shell || !output_cb || !config->host[0] || !config->username[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_shell = NULL;
    ESP_RETURN_ON_ERROR(ensure_ssh_lock(), TAG, "ensure ssh lock");
    if (xSemaphoreTake(s_ssh_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    si_ssh_shell_t *shell = calloc(1, sizeof(*shell));
    if (!shell) {
        xSemaphoreGive(s_ssh_lock);
        return ESP_ERR_NO_MEM;
    }
    shell->config = *config;
    shell->output_cb = output_cb;
    shell->user_ctx = user_ctx;
    shell->input = xStreamBufferCreate(SI_SSH_SHELL_INPUT_BUFFER_SIZE, 1);
    shell->done = xSemaphoreCreateBinary();
    if (!shell->input || !shell->done) {
        if (shell->input) {
            vStreamBufferDelete(shell->input);
        }
        if (shell->done) {
            vSemaphoreDelete(shell->done);
        }
        ssh_secure_clear(&shell->config, sizeof(shell->config));
        free(shell);
        xSemaphoreGive(s_ssh_lock);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(
        ssh_shell_task, "si_ssh_shell", SI_SSH_TASK_STACK_SIZE, shell,
        tskIDLE_PRIORITY + 2, &shell->task, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        vStreamBufferDelete(shell->input);
        vSemaphoreDelete(shell->done);
        ssh_secure_clear(&shell->config, sizeof(shell->config));
        free(shell);
        xSemaphoreGive(s_ssh_lock);
        return ESP_ERR_NO_MEM;
    }

    *out_shell = shell;
    return ESP_OK;
}

esp_err_t si_ssh_shell_write(si_ssh_shell_t *shell, const uint8_t *data, size_t len,
                             uint32_t timeout_ms)
{
    if (!shell || !data || len == 0 || shell->stop_requested) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t sent = xStreamBufferSend(shell->input, data, len, pdMS_TO_TICKS(timeout_ms));
    return sent == len ? ESP_OK : ESP_ERR_TIMEOUT;
}

void si_ssh_shell_stop(si_ssh_shell_t *shell)
{
    if (!shell) {
        return;
    }
    shell->stop_requested = true;
    if (shell->input) {
        const uint8_t wake = 0x04;
        (void)xStreamBufferSend(shell->input, &wake, 1, 0);
    }
    if (shell->done) {
        (void)xSemaphoreTake(shell->done, portMAX_DELAY);
    }
    if (shell->task) {
        vTaskDeleteWithCaps(shell->task);
        shell->task = NULL;
    }
    if (shell->input) {
        vStreamBufferDelete(shell->input);
    }
    if (shell->done) {
        vSemaphoreDelete(shell->done);
    }
    ssh_secure_clear(&shell->config, sizeof(shell->config));
    free(shell);
}

bool si_ssh_shell_is_running(si_ssh_shell_t *shell)
{
    return shell && shell->running;
}

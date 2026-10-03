#!/usr/bin/env python3
"""Run the production LAN discovery loop against deterministic socket faults."""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "main/services/network_discovery.c").read_text()


def function(signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


defines = "\n".join(re.findall(
    r"^#define DISCOVERY_(?:REQUEST_MAX|RESPONSE_MAX|NONCE_MAX|DISABLED_POLL_MS) .*",
    source, re.MULTILINE,
))
production = "\n".join(function(signature) for signature in (
    "static int open_discovery_socket(",
    "static void discovery_task(",
))
harness = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define SI_NETWORK_DISCOVERY_PORT 12345U
#define SI_PRODUCT_HOSTNAME_MAX_LEN 63U
#define ESP_OK 0
#define pdMS_TO_TICKS(ms) ((ms) / 10U)
typedef int esp_err_t;
typedef struct { char address_source[16]; } si_net_status_t;
static int s_rate_limiter;
static jmp_buf stop_loop;
static int open_calls, close_calls, bind_calls, timeout_calls, reuse_calls;
static int delay_calls, published_calls, unpublished_calls, identity_calls;
static int parse_calls, recv_calls, live_fd;
static bool fail_socket, fail_timeout, fail_reuse, fail_bind;
static bool stop_when_disabled;
static int disable_after;
static int receive_errors[16];
static int receive_fds[16];
static size_t receive_count, receive_index;

static int fake_socket(int family, int type, int protocol)
{
    assert(family == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_IP);
    open_calls++;
    if (fail_socket) { errno = ENFILE; return -1; }
    assert(live_fd == -1);
    live_fd = 6 + open_calls;
    return live_fd;
}
static int fake_setsockopt(int fd, int level, int option,
                           const void *value, socklen_t length)
{
    assert(fd == live_fd && level == SOL_SOCKET);
    if (option == SO_RCVTIMEO) {
        const struct timeval *timeout = value;
        assert(length == sizeof(*timeout));
        assert(timeout->tv_sec == 1 && timeout->tv_usec == 0);
        timeout_calls++;
        if (fail_timeout) { errno = ENOMEM; return -1; }
    } else {
        assert(option == SO_REUSEADDR && length == sizeof(int));
        assert(*(const int *)value == 1);
        reuse_calls++;
        if (fail_reuse) { errno = ENOPROTOOPT; return -1; }
    }
    return 0;
}
static int fake_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    assert(fd == live_fd && length == sizeof(struct sockaddr_in));
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    assert(ipv4->sin_family == AF_INET);
    assert(ipv4->sin_port == htons(SI_NETWORK_DISCOVERY_PORT));
    assert(ipv4->sin_addr.s_addr == htonl(INADDR_ANY));
    bind_calls++;
    if (fail_bind) { errno = EADDRINUSE; return -1; }
    return 0;
}
static int fake_close(int fd)
{
    assert(fd >= 0 && fd == live_fd);
    live_fd = -1;
    close_calls++;
    /* Closing can change errno; recovery must already have classified it. */
    errno = 0;
    return 0;
}
static void vTaskDelay(unsigned ticks)
{
    assert(ticks > 0);
    delay_calls++;
    if (stop_when_disabled && ticks == pdMS_TO_TICKS(250))
        longjmp(stop_loop, 1);
    assert(ticks == pdMS_TO_TICKS(1000));
}
static bool si_lan_discovery_is_enabled_cached(void)
{
    return disable_after < 0 || recv_calls < disable_after;
}
static esp_err_t publish_mdns_service(void)
{
    published_calls++;
    return ESP_OK;
}
static void unpublish_mdns_service(void) { unpublished_calls++; }
static void sync_mdns_identity(char *hostname, size_t hostname_size,
                               char *source, size_t source_size)
{
    (void)hostname; (void)hostname_size; (void)source; (void)source_size;
    identity_calls++;
}
static ssize_t fake_recvfrom(int fd, void *buffer, size_t length, int flags,
                            struct sockaddr *address, socklen_t *address_len)
{
    (void)buffer;
    assert(fd == live_fd && fd >= 0);
    assert(length == 257 && flags == 0);
    assert(*address_len == sizeof(struct sockaddr_storage));
    if (receive_index == receive_count) longjmp(stop_loop, 1);
    receive_fds[receive_index] = fd;
    int error = receive_errors[receive_index++];
    recv_calls++;
    if (error) { errno = error; return -1; }
    address->sa_family = AF_INET;
    return 0; /* An empty UDP datagram must leave the socket open. */
}
static bool si_discovery_rate_limit_allow(int *limiter,
                                         uint32_t address, uint32_t now_ms)
{
    (void)limiter; (void)address; (void)now_ms;
    return true;
}
static int64_t esp_timer_get_time(void) { return 0; }
static bool parse_discovery_request(char *data, char *nonce, size_t capacity)
{
    (void)data; (void)nonce; (void)capacity;
    parse_calls++;
    return false;
}
static char *build_discovery_response(const char *nonce)
{
    (void)nonce;
    return NULL;
}
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define close fake_close
#define recvfrom fake_recvfrom
'''
tests = r'''
static void reset(void)
{
    open_calls = close_calls = bind_calls = timeout_calls = reuse_calls = 0;
    delay_calls = published_calls = unpublished_calls = identity_calls = 0;
    parse_calls = recv_calls = 0;
    live_fd = -1;
    fail_socket = fail_timeout = fail_reuse = fail_bind = false;
    stop_when_disabled = false;
    disable_after = -1;
    receive_count = receive_index = 0;
    memset(receive_errors, 0, sizeof(receive_errors));
    memset(receive_fds, 0, sizeof(receive_fds));
}
static void run_loop(void)
{
    if (setjmp(stop_loop) == 0) discovery_task(NULL);
}
int main(void)
{
    reset();
    fail_timeout = true;
    assert(open_discovery_socket() == -1);
    assert(timeout_calls == 1 && close_calls == 1 && live_fd == -1);
    assert(bind_calls == 0 && reuse_calls == 0);

    reset();
    fail_socket = true;
    assert(open_discovery_socket() == -1);
    assert(open_calls == 1 && timeout_calls == 0 && close_calls == 0);

    reset();
    fail_bind = true;
    assert(open_discovery_socket() == -1);
    assert(bind_calls == 1 && close_calls == 1 && live_fd == -1);

    reset();
    fail_reuse = true; /* Reuse is optional; the receive deadline is mandatory. */
    assert(open_discovery_socket() == 7);
    assert(timeout_calls == 1 && reuse_calls == 1 && bind_calls == 1);
    assert(close_calls == 0);

    reset();
    receive_errors[0] = EAGAIN;
    receive_errors[1] = EWOULDBLOCK;
    receive_errors[2] = EINTR;
    receive_count = 4; /* Last item is an empty datagram. */
    run_loop();
    assert(recv_calls == 4 && open_calls == 1 && close_calls == 0);
    assert(delay_calls == 0 && published_calls == 1 && parse_calls == 0);
    for (size_t i = 0; i < receive_count; i++) assert(receive_fds[i] == 7);

    reset();
    receive_errors[0] = EBADF;
    receive_errors[1] = ENOTSOCK;
    receive_errors[2] = ENETDOWN;
    receive_count = 3;
    run_loop();
    assert(open_calls == 4 && close_calls == 3 && delay_calls == 3);
    assert(timeout_calls == 4 && bind_calls == 4);
    assert(published_calls == 1 && unpublished_calls == 0);
    for (size_t i = 0; i < receive_count; i++)
        assert(receive_fds[i] == 7 + (int)i);

    reset();
    receive_errors[0] = EAGAIN;
    receive_errors[1] = EINTR;
    receive_errors[2] = EBADF;
    receive_errors[3] = 0;
    receive_errors[4] = ECONNRESET;
    receive_errors[5] = EWOULDBLOCK;
    receive_count = 6;
    run_loop();
    assert(open_calls == 3 && close_calls == 2 && delay_calls == 2);
    assert(receive_fds[0] == 7 && receive_fds[1] == 7 && receive_fds[2] == 7);
    assert(receive_fds[3] == 8 && receive_fds[4] == 8 && receive_fds[5] == 9);
    assert(parse_calls == 0 && published_calls == 1);

    reset();
    receive_errors[0] = EAGAIN;
    receive_count = 1;
    disable_after = 1;
    stop_when_disabled = true;
    run_loop();
    assert(open_calls == 1 && close_calls == 1 && live_fd == -1);
    assert(published_calls == 1 && unpublished_calls == 1 && delay_calls == 1);

    puts("LAN discovery socket recovery runtime: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="exoanchor-network-discovery-") as tmp:
    test_c = Path(tmp) / "test.c"
    test_bin = Path(tmp) / "test"
    test_c.write_text(harness + "\n" + defines + "\n" + production + tests)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", str(test_c), "-o", str(test_bin),
    ], check=True)
    subprocess.run([str(test_bin)], check=True)

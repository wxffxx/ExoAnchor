#!/usr/bin/env python3
"""Compile the production HTTPD send override against deterministic socket I/O."""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "main/services/video_h264_stream.c").read_text()
start = source.index("static int h264_socket_send_all(")
function = source[start:source.index("\n}\n", start) + 3]
defines = "\n".join(re.findall(
    r"^#define SI_H264_(?:SEND_TIMEOUT_MS|SEND_EAGAIN_RETRY_MS|EGRESS_STOP_TIMEOUT_MS) .*",
    source, re.MULTILINE,
))
harness = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

typedef void *httpd_handle_t;
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(ms) ((ms) / 10U)

static int64_t now_us;
static int64_t advance_us;
static unsigned calls;
static unsigned sleeps;
static int scenario;
static size_t total_sent;
static char output[64];
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    now_us += (int64_t)ticks * 10000;
    sleeps++;
}
static ssize_t fake_send(int fd, const void *buffer, size_t length, int flags)
{
    assert(fd == 7);
    assert((flags & MSG_DONTWAIT) != 0);
    assert((flags & MSG_OOB) != 0);
    calls++;
    now_us += advance_us;
    if (scenario == 1 || (scenario == 5 && calls == 1)) {
        errno = EINTR;
        return -1;
    }
    if (scenario == 2 || (scenario == 5 && calls == 2)) {
        errno = EAGAIN;
        return -1;
    }
    if (scenario == 3 || (scenario == 7 && calls == 2)) return 0;
    if (scenario == 4) { errno = ECONNRESET; return -1; }
    if (scenario == 6) { errno = EWOULDBLOCK; return -1; }
    size_t count = length < 2 ? length : 2;
    assert(total_sent + count <= sizeof(output));
    memcpy(output + total_sent, buffer, count);
    total_sent += count;
    return (ssize_t)count;
}
#define send fake_send
'''
tests = r'''
static void reset(int selected, int64_t advance)
{
    now_us = 0;
    advance_us = advance;
    calls = sleeps = 0;
    total_sent = 0;
    scenario = selected;
    errno = 0;
    memset(output, 0, sizeof(output));
}
static int transmit(const char *buffer, size_t length)
{
    return h264_socket_send_all(NULL, 7, buffer, length, MSG_OOB);
}
int main(void)
{
    const char payload[] = "0123456789abcdef";
    const int64_t budget = (int64_t)SI_H264_SEND_TIMEOUT_MS * 1000;
    assert(SI_H264_EGRESS_STOP_TIMEOUT_MS == 2U * SI_H264_SEND_TIMEOUT_MS + 500U);
    reset(0, 0);
    assert(transmit(payload, sizeof(payload)) == sizeof(payload));
    assert(total_sent == sizeof(payload));
    assert(memcmp(output, payload, sizeof(payload)) == 0);
    assert(calls > 1);

    /* Continuous progress must not create a fresh budget after each short write. */
    reset(0, budget / 4);
    assert(transmit(payload, sizeof(payload)) == -1);
    assert(errno == ETIMEDOUT && calls == 4 && now_us == budget);
    assert(total_sent == 8);

    reset(1, budget / 4);
    assert(transmit(payload, sizeof(payload)) == -1);
    assert(errno == ETIMEDOUT && calls == 4 && now_us == budget);

    for (int selected = 2; selected <= 6; selected += 4) {
        reset(selected, 0);
        assert(transmit(payload, sizeof(payload)) == -1);
        assert(errno == ETIMEDOUT && now_us == budget && sleeps == calls);
    }

    reset(5, 1);
    assert(transmit(payload, sizeof(payload)) == sizeof(payload));
    assert(memcmp(output, payload, sizeof(payload)) == 0 && sleeps == 1);

    reset(3, 0);
    assert(transmit(payload, sizeof(payload)) == -1 && errno == EPIPE);
    reset(7, 0);
    assert(transmit(payload, sizeof(payload)) == -1 && errno == EPIPE);
    assert(total_sent == 2); /* A partial WebSocket payload cannot report success. */
    reset(4, 0);
    assert(transmit(payload, sizeof(payload)) == -1 && errno == ECONNRESET);
    reset(0, 0);
    assert(transmit(NULL, 1) == -1 && errno == EINVAL);
    assert(transmit(payload, (size_t)INT_MAX + 1U) == -1 && errno == EOVERFLOW);
    assert(transmit(payload, 0) == 0 && calls == 0);
    puts("H.264 send deadline runtime: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="exoanchor-h264-send-") as tmp:
    test_c = Path(tmp) / "test.c"
    test_bin = Path(tmp) / "test"
    test_c.write_text(harness + "\n" + defines + "\n" + function + tests)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", str(test_c), "-o", str(test_bin),
    ], check=True)
    subprocess.run([str(test_bin)], check=True)

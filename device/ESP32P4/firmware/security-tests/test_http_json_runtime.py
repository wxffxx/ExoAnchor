#!/usr/bin/env python3
"""Exercise both production JSON readers with real cJSON and mocked HTTP I/O."""
from pathlib import Path
import subprocess
import tempfile

from test_http_security_runtime import extract


ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define HTTPD_400_BAD_REQUEST 400
typedef struct { size_t content_len; const char *body; size_t offset; } httpd_req_t;
static int sent, send_result, reads;
static int httpd_req_recv(httpd_req_t *req, char *out, size_t length) {
    reads++;
    size_t available = strlen(req->body) - req->offset;
    if (length > available) length = available;
    if (length > 2) length = 2; /* Exercise fragmented reads. */
    memcpy(out, req->body + req->offset, length);
    req->offset += length;
    return (int)length;
}
static int httpd_resp_send_err(httpd_req_t *req, int status, const char *message) {
    (void)req; (void)message; assert(status == 400); sent++; return send_result;
}
'''
CASES = r'''
int main(void) {
    char buffer[16];
    const char *invalid[] = {"{", "", "{\"x\":", "xxxxxxxxxxxxxxxx"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (int failure = 0; failure < 2; failure++) {
            httpd_req_t req = {strlen(invalid[i]), invalid[i], 0};
            cJSON *root = (cJSON *)1;
            sent = reads = 0; send_result = failure ? -7 : ESP_OK;
            int ret = si_http_recv_json(&req, buffer, sizeof(buffer), &root);
            if (ret == ESP_OK || root != NULL || sent != 1) {
                fprintf(stderr, "invalid JSON continued: case=%zu ret=%d replies=%d\n", i, ret, sent);
                return 1;
            }
            if (failure) assert(ret == -7);
            if (req.content_len >= sizeof(buffer)) assert(reads == 0);
        }
    }
    httpd_req_t good = {7, "{\"x\":1}", 0};
    cJSON *root = NULL;
    sent = reads = 0; send_result = ESP_OK;
    assert(si_http_recv_json(&good, buffer, sizeof(buffer), &root) == ESP_OK);
    assert(root && sent == 0 && reads > 1);
    assert(cJSON_GetObjectItemCaseSensitive(root, "x")->valueint == 1);
    cJSON_Delete(root);
    httpd_req_t truncated = {7, "{\"x", 0};
    root = NULL;
    assert(si_http_recv_json(&truncated, buffer, sizeof(buffer), &root) != ESP_OK);
    assert(root == NULL);
    assert(si_http_recv_json(&good, buffer, sizeof(buffer), NULL) == ESP_ERR_INVALID_ARG);
    return 0;
}
'''


def main():
    vendor = ROOT / "v0.86.6-dev/tests/host/vendor/cjson"
    failed = False
    with tempfile.TemporaryDirectory(prefix="exoanchor-json-runtime-") as directory:
        for version in ("v0.86-stable-kvm", "v0.86.6-dev"):
            source_path = ROOT / version / "main/adapters/http_api.c"
            code = PRELUDE + extract(source_path, "si_http_recv_body")
            code += extract(source_path, "si_http_recv_json") + CASES
            source = Path(directory) / (version + ".c")
            binary = source.with_suffix("")
            source.write_text(code)
            subprocess.run(["cc", "-std=c11", "-I" + str(vendor), str(source),
                            str(vendor / "cJSON.c"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], timeout=5)
            failed |= result.returncode != 0
            print(f"{version} JSON error propagation: {'FAIL' if result.returncode else 'PASS'}", flush=True)
    raise SystemExit(int(failed))


if __name__ == "__main__":
    main()

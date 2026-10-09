#!/usr/bin/env python3
"""A valid stale slot tail must not turn a prefix upload into a complete image."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_http_security_runtime import extract

ROOT=Path(__file__).resolve().parents[1]
path=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'v0.86.6-dev/main/services/web/ota_runtime_module.inc'
function=extract(path,'ota_finish_update')
PRELUDE=r'''
#include <assert.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int esp_ota_handle_t;
typedef struct { uint32_t address, size; } esp_partition_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t image_len; } esp_image_metadata_t;
#define ESP_OK 0
#define ESP_IMAGE_VERIFY 1
#define TAG "ota-test"
#define ESP_RETURN_ON_ERROR(x, ...) do { int r=(x); if(r) return r; } while(0)
static const esp_partition_t partition={0x420000,0x400000};
static int ends, verifications, boot_changes, end_result;
static const uint32_t full_image_size=3115840;
/* IDF accepts the already-valid complete flash slot, even with a short write. */
static int esp_ota_end(int handle) { assert(handle==7); ends++; return end_result; }
static int esp_image_verify(int mode,const esp_partition_pos_t *p,esp_image_metadata_t *out) {
    assert(mode==ESP_IMAGE_VERIFY && p->offset==partition.address);
    verifications++; out->image_len=full_image_size;
    return p->size<full_image_size ? -3 : 0;
}
static int esp_ota_set_boot_partition(const esp_partition_t *p) {
    assert(p==&partition); boot_changes++; return 0;
}
'''
call='ota_finish_update(7,&partition,n)' if 'uint32_t written' in function else 'ota_finish_update(7,&partition)'
CASES=r'''
int main(void) {
    assert(RUN_FINISH(4096)!=ESP_OK); assert(ends==1 && !boot_changes);
    assert(verifications==1);
    assert(RUN_FINISH(full_image_size)==ESP_OK); assert(boot_changes==1);
    end_result=-9;
    assert(RUN_FINISH(full_image_size)==-9);
    assert(ends==3 && verifications==2 && boot_changes==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='exoanchor-ota-extent-') as tmp:
    source=Path(tmp)/'extent.c'; binary=Path(tmp)/'extent'
    source.write_text(PRELUDE+function+'\n#define RUN_FINISH(n) '+call+'\n'+CASES)
    subprocess.run(['cc','-std=c11',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
print('OTA stale-tail rejection and boot-slot preservation: PASS')

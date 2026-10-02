#!/usr/bin/env python3
"""Run real patched UVC parsers with the official malformed USB fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    idf = Path(os.environ["IDF_PATH"])
    component = ROOT / "v0.86.6-dev/managed_components/espressif__usb_host_uvc"
    stubs = ROOT / "v0.86.6-dev/tests/host/idf_stubs"
    with tempfile.TemporaryDirectory(prefix="exoanchor-uvc-security-") as tmp:
        tmp = Path(tmp)
        (tmp / "usb").mkdir()
        (tmp / "usb/usb_host.h").write_text('#pragma once\n#include "usb/usb_helpers.h"\n#include "usb/usb_types_ch9.h"\n#include "usb/usb_types_stack.h"\n')
        (tmp / "sdkconfig.h").write_text("#define CONFIG_UVC_INTERVAL_ARRAY_SIZE 10\n")
        (tmp / "esp_check.h").write_text('#include "esp_err.h"\n#define ESP_RETURN_ON_FALSE(c,r,...) do {if(!(c))return (r);}while(0)\n')
        source = tmp / "test.c"
        source.write_text('#include "' + str(ROOT / "security-patches/usb_host_uvc-2.5.1/uvc_descriptor_parsing.c") + '"\n' + r'''
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>
#include "esp_err.h"
#include "usb/uvc_host.h"
#include "uvc_descriptors_priv.h"
#include "malicious_uvc.h"
int main(void) {
    const usb_config_desc_t *cfg=(const usb_config_desc_t *)cfg_iad_no_vc_header;
    size_t n=0;
    assert(uvc_desc_get_frame_list(cfg,0,NULL,&n)==ESP_ERR_NOT_FOUND);
    uvc_host_stream_format_t format={0};uint8_t interface;uint16_t version;
    assert(uvc_desc_get_streaming_interface_num(cfg,0,&format,&version,&interface)==ESP_ERR_NOT_FOUND);
    cfg=(const usb_config_desc_t *)cfg_huge_frame_interval_type;
    uvc_host_frame_info_t frames[4]={0};n=4;
    assert(uvc_desc_get_frame_list(cfg,0,&frames,&n)==ESP_OK);
    assert(n>=1 && frames[0].interval_type==1 && frames[0].h_res==640 && frames[0].v_res==480);
    format=(uvc_host_stream_format_t){.h_res=640,.v_res=480,.fps=30,.format=UVC_VS_FORMAT_MJPEG};
    const uvc_format_desc_t *fd=NULL;const uvc_frame_desc_t *fr=NULL;
    assert(uvc_desc_get_frame_format_by_format(cfg,1,&format,&fd,&fr)==ESP_OK);
    assert(fd && fr);
    assert(uvc_desc_continuous_fps_matches(333333,1000000,333333,30));
    assert(!uvc_desc_continuous_fps_matches(1,UINT32_MAX,1,10000001));
    assert(!uvc_desc_continuous_fps_matches(1,UINT32_MAX,UINT32_MAX,30));
    assert(!uvc_desc_continuous_fps_matches(0,UINT32_MAX,1,30));
    assert(!uvc_desc_continuous_fps_matches(1,UINT32_MAX,1,NAN));
    return 0;
}
''')
        binary = tmp / "uvc-tests"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-I" + str(tmp),
                        "-I" + str(stubs), "-I" + str(Path(__file__).parent),
                        "-I" + str(component / "include"), "-I" + str(component / "private_include"),
                        "-I" + str(idf / "components/usb/include"),
                        "-I" + str(idf / "components/esp_common/include"),
                        str(source),
                        str(idf / "components/usb/usb_helpers.c"), "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)
    print("UVC real parser missing-header and interval bounds: PASS")


if __name__ == "__main__":
    main()

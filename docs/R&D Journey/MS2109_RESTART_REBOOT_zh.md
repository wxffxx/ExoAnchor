# 2026-10-28 重启 MS2109 导致整板重启：排查与修复

- 时间：2026-10-28
- 固件：`0.88.0-dev AzureDanube`，台架免登录构建（`build-v24-dev-088-noauth`），ESP-IDF 5.5.5
- 硬件：ExoAnchor PrototypeV2.4（ESP32-P4 rev3.2，32 MB PSRAM @ 250 MHz），MS2109 USB 采集，
  3.3 V / 1.2 V 两路电源轨由固件控制（GPIO13 / GPIO18）
- 修复：[`CMakeLists.txt`](../../device/ESP32P4/firmware/v0.86.6-dev/CMakeLists.txt) 与
  [`main/drivers/video_input.c`](../../device/ESP32P4/firmware/v0.86.6-dev/main/drivers/video_input.c)；
  已在板上验证，**尚未提交**

## 1. 结论

在 KVM 页重启 MS2109 后，页面会有一段时间显示 UNKNOWN。原因不是接口变慢，而是整块板子重启了：
MS2109 重新上电后 USB 枚举因内部 DMA 内存不足失败，约 5 s 后固件的恢复逻辑去断 USB 根端口电源，
ESP-IDF 的 hub 驱动在没有设备节点的情况下按“设备仍在”处理，触发 `hub.c:435` 的断言。

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| MS2109 断电再上电后整板重启 | 5 次中 3 次 | 6 次中 0 次 |
| 循环期间 `/api/status` 失败（每 0.5 s 轮询，6 s 超时） | 8 次 | 0 次 |
| MS2109 上电到重新出帧 | — | 1.7–1.9 s |
| 串口 USB 内存错误（`HCD Pipe alloc` / `EP Alloc` `ESP_ERR_NO_MEM`） | 每次重启前都有 | 0 |
| 1080p25 H.264（独立客户端） | 24.3 fps（同日、描述符未移的构建） | 24.17 fps，采集丢帧 0，背压丢帧 0 |

[0.88 实验记录](VIDEO_STREAMING_0.88_zh.md)第 7 节列出的遗留问题“采集卡断电后 UVC 重开会有 1–10 s 的
`EP Alloc error: ESP_ERR_NO_MEM`”就是这个问题：`EP Alloc` 和 `HCD Pipe alloc` 两条报错都来自 IDF
`hcd_pipe_alloc()` 里传输描述符表的内部 DMA 内存分配，而不是当时推测的 usb_host_uvc 停止 URB 环
补丁；本次修复没有改动那个补丁，报错已消失。

## 2. 测试方法

**复现**：通过 `/api/ms2109/power` 关闭 MS2109，4 s 后打开；同时每 0.5 s 请求一次 `/api/status`
（超时 6 s），记录失败和 `server_uptime` 回退（回退即重启）。

**串口**：全程抓 USB 串口日志（115200），用 addr2line 解析崩溃地址。

**修复后的验证**：每轮关 4 s、开，然后等待 50 s；“重新出帧时间”取从打开到 `/api/status` 中
`video.frames_captured` 再次增长的时间。同时用台架的 `h264_measure.py` 以 1920×1080@25 循环拉
40 s 一段的 H.264（每 3 s 续租），模拟浏览器查看并重连。每次循环测试前先单独测一次 1080p25 吞吐。

**诊断**：在重插路径打印内部 DMA 堆（`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`）的空闲量和最大块；
临时加 `heap_caps_register_failed_alloc_callback` 打印失败的 DMA 分配（最终版本已移除）。

**注意事项**：后台遗留的串口抓日志进程占着端口时，esptool 会在 “Connecting” 处报
`device reports readiness to read but returned no data`，板子随后掉线，重新烧录才恢复。每次烧录前
先结束抓日志进程，并用 `lsof` 确认端口空闲。

## 3. 问题与根因

按发现顺序排列。

### 3.1 UNKNOWN 是整板重启

现象：第一次复现时，MS2109 上电约 5.5 s 后板子从网络上消失；恢复后 `server_uptime` 只有 19 s。
连续 5 次断电上电，3 次整板重启，每次重启后 uptime 从 10–12 s 重新计起。

重启后 Agent 要回放 `TASKS.LOG`（2480 条记录，1666831 B），实测 45188–58394 ms；期间
`si_agent_recove` 任务占住 CPU 0，`task_wdt` 报 IDLE0 未喂狗，大量 HTTPS 握手失败（`-0x7780`）。
这段时间页面同样拿不到状态，UNKNOWN 因而持续得更久。

### 3.2 崩溃点：`hub.c:435` 的断言

串口中每次重启前的序列相同（以其中一次为例）：

```
E (22630) USBH: HCD Pipe alloc error: ESP_ERR_NO_MEM
E (22630) HUB: Failed to add new device
W (27212) HUB: Device tree node (parent_port=0): not found
ESP_ERROR_CHECK failed: esp_err_t 0x105 (ESP_ERR_NOT_FOUND)
file: "/IDF/components/usb/hub.c" line 435
expression: dev_tree_node_dev_gone(NULL, 0)
```

根因：MS2109 上电后 EP0 管道分配失败，hub 没有建立设备节点。约 5 s 后
（`SI_UVC_MISSING_DEVICE_RECOVERY_DELAY_MS` = 5000），`video_input.c` 的“设备缺失”恢复调用
`usb_host_lib_set_root_port_power(false)`，hub 根端口进入 `NOT_POWERED`。IDF 5.5.5 在这个状态下
收到断开事件时一律认为端口上有设备，调用 `dev_tree_node_dev_gone(NULL, 0)`，找不到节点返回
`ESP_ERR_NOT_FOUND`，被 `ESP_ERROR_CHECK` 变成 abort。

修复：`power_cycle_uvc_root_port()` 先用 `usb_host_device_addr_list_fill()` 检查 hub 里是否有设备。
没有设备时不再断根端口电源，而是在 MS2109 已上电且不在冷启动中时调用
`si_ms2109_power_cycle(1000)` 给采集卡断电重插，对 hub 来说这是一次普通的拔插。

修复后 6 次循环零重启，`/api/status` 无失败。但画面没有回来：串口中每秒一次
`open retained UVC` 紧跟 `EP Alloc error: ESP_ERR_NO_MEM`，之后的枚举也仍报 `HCD Pipe alloc error`。

### 3.3 枚举失败后不会自动重试

根因：枚举失败后 hub 不会自己重试，而 supervisor 因为保留着上次的采集选择，一直停在“打开 UVC 失败”
的分支里，走不到“设备缺失”恢复。

修复：在打开失败的分支里记录“没有已枚举设备”的起始时间，持续 5 s（且距上次恢复至少 15 s）后调用
同一个重插函数。

结果：7 次循环零重启，但画面仍不恢复，`last_error` 为 `open UVC stream: ESP_ERR_NO_MEM` 或
`ESP_ERR_NOT_FOUND`。串口显示重插确实触发了（`no enumerated USB device; power-cycling MS2109`），
每次重插后的枚举照样 `NO_MEM`。

### 3.4 `NO_MEM` 不是瞬时的：内部 DMA 堆碎片

在重插日志里加上内部 DMA 堆的数据后：没有查看端时枚举成功；有 1080p25 H.264 查看端时，失败时刻的
空闲量为 3359–6999 B，最大块只有 624–2016 B。

IDF `hcd_pipe_alloc()` 为每个管道分配 2 个缓冲块，每块一张传输描述符表，用
`heap_caps_aligned_calloc(512, ...)` 申请，能力为 `MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED |
MALLOC_CAP_INTERNAL`。控制传输 3 个描述符（24 B），等时传输 64 个（512 B）。因为 512 字节对齐，
TLSF 需要约 1 KiB（EP0）/ 约 1.5 KiB（等时）的连续空闲块。失败分配回调证实了这一点：失败的是
24 B、能力 `0x80808` 的分配，当时最大块为 624 / 624 / 1440 B。sdkconfig 中
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，内部堆在有查看端时是碎片化的。

### 3.5 修复：只把描述符表放进 PSRAM

IDF 的 Kconfig 注明 ESP32-P4 的 USB-DWC DMA 可以访问外部 RAM。`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`
会同时移动描述符表（`hcd_dwc.c` 的 `XFER_DESC_LIST_CAPS`）和 URB 数据缓冲（`usb_private.c` 的
`DATA_BUFFER_CAPS`），后者会拖慢摄入（见第 4 节）。

修复：`CMakeLists.txt` 沿用项目里 esp_http_client 补丁的做法，校验 IDF `usb/hcd_dwc.c` 的 SHA256
（`33fea905…`），在构建目录生成一份副本，只把 `XFER_DESC_LIST_CAPS` 中的 `MALLOC_CAP_INTERNAL`
换成 `MALLOC_CAP_SPIRAM`，再替换 usb 组件的源文件。哈希或片段对不上时构建直接失败。构建日志会打印
`Using ExoAnchor USB descriptor lists in PSRAM`。

结果：5 次循环画面分别在 1.8 / 1.7 / 1.9 / 2.1 / 5.6 s 后恢复，失败分配回调不再报 USB 的 DMA 分配
失败。随后移除失败分配回调和第 4 节的 H.264 握手推迟，把“跳过根端口断电”的日志降为 debug 级，
主机测试全部通过，再做最终验证（第 5 节）。

## 4. 试过但没有采用

| 方案 | 结果 | 原因 |
| --- | --- | --- |
| 整体开启 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` | 画面能恢复（4 次分别 1.7 / 2.2 / 15.6 / 1.6 s），但 1080p25 单测只有 9.76 fps，循环期间各 40 s 窗口 8.86–13.53 fps（采集侧 9.71–18.89 fps）；串口出现 6 次 `invalid MJPEG SOI`，对照构建为 0 | 该选项同时把 URB 数据缓冲移进 PSRAM；只移描述符表就够了 |
| H.264 会话等 UVC 流打开后再申请编解码资源（先给 USB 管道，再给编解码器） | 画面仍不恢复；查看端 77 次连上后 0 个访问单元即被关闭；失败时内部 DMA 最大块仍为 624–1440 B | 推迟编解码器没有让内部 DMA 堆出现足够大的连续块 |

## 5. 最终实测

2026-10-28，最终构建（`build-v24-dev-088-noauth`，app sha256 `4d0f24e5…`），HTTPS，台架免登录镜像。

循环前单独测 1080p25 H.264 40 s：24.17 fps，采集侧 24.13 fps，采集丢帧 0，背压丢帧 0，设备侧平均
延迟 113.7 ms。

随后在查看端持续拉 1080p25 H.264 的情况下，MS2109 断电 4 s 再上电，共 6 次：

| 轮次 | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 上电到重新出帧 | 1.9 s | 1.9 s | 1.8 s | 1.7 s | 1.7 s | 1.8 s |

- 整板重启 0 次，`/api/status` 失败 0 次，uptime 连续增长到 464 s。
- 串口：`HCD Pipe alloc` 0 次，`EP Alloc` 0 次，abort / Guru Meditation 0 次，`task_wdt` 0 次；
  采集卡断开 6 次，H.264 `pipeline ready` 9 次；重插路径没有被触发。
- 主机测试（Python 3.11）全部通过。

## 6. 遗留问题

- 修复尚未提交。按项目规则，需要先用开启登录的配置构建，确认登录正常后再提交。
- 最终测试中每次都一次枚举成功，没有走到 MS2109 重插路径。重插路径只在修复内存问题之前跑过：能触发，
  但当时枚举仍因内存失败，并伴随 `si_ms2109_power_get_status(210): MS2109 power status busy` 报错。
- 循环期间查看端前 4 个 40 s 窗口为 20.69–20.96 fps，之后 4 个为 24.10–24.17 fps，原因未查。
- 任何一次重启后，Agent 回放 `TASKS.LOG` 都要 45–58 s，期间 CPU 0 触发 `task_wdt`、HTTPS 握手大量
  失败。已另开会话处理（“Speed up Agent TASKS.LOG boot recovery”）。
- 第一次抓到的串口日志里，有一次 abort 后的软件复位在启动阶段又出现 `Instruction access fault`，
  随后由 LP 看门狗复位（`CHIP_LP_WDT_RESET`）；另外两次 abort 后正常启动。本次未分析。

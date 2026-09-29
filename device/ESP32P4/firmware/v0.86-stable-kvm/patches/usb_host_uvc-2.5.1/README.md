# usb_host_uvc 2.5.1 ISOC MJPEG compatibility override

部分 HDMI USB 采集卡在 isochronous MJPEG 传输中会切换 Frame ID，
但不会设置 UVC payload 的 EOF 标志。`usb_host_uvc 2.5.1` 的原始实现会将
这种输入持续判定为 `missed EoF`，因而无法交付完整帧。

本目录中的 `uvc_isoc.c` 基于 Espressif `usb_host_uvc 2.5.1`，改为通过
JPEG SOI/EOI 标志组装 MJPEG 帧，同时保留 YUY2 的定长帧处理。顶层
`CMakeLists.txt` 会用该文件替换依赖中的同名源文件，并校验原始 2.5.1
文件的 SHA-256；依赖升级后必须重新审阅，不能静默沿用。

该兼容源文件保留 Espressif 的 `Apache-2.0` SPDX 声明，不属于仓库中
默认适用 MIT 的原创 ExoAnchor 源文件。若上游版本已提供等价的 ISOC
MJPEG SOI/EOI 兼容处理，应删除本覆盖并恢复直接使用上游实现。

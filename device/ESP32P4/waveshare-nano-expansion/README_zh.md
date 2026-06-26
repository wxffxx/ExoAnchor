# Waveshare NANO Expansion

这个形态是在 Waveshare ESP32-P4-NANO 上加扩展板，定位介于纯 DIY 直连和完整自研板之间。

## 能力边界

| 功能 | 状态 |
|------|------|
| UVC 视频 | 支持，走 ESP32-P4 高速 USB host |
| USB HID | 支持，默认 GPIO26(DM) / GPIO27(DP)，除非扩展板重新映射 |
| 以太网 | 支持，NANO RJ45 / IP101GRI |
| ATX 电源控制 | 扩展板侧保留 |
| Power 12V 检测 | 如果硬件接入则保留 |
| Power Standby 3V3AUX 检测 | 当前扩展板不支持 |

这里最关键的限制是 standby：当前扩展板没有办法检测 3V3AUX，所以 `Power Standby` 不能当作这个硬件的真实后端状态。

固件使用 `../firmware` 这一份共享 ESP-IDF 工程。

# ESP32-P4 Shared Firmware

Shared ESP-IDF firmware for all ESP32-P4 hardware variants under `device/ESP32P4`.

## Build

```bash
. "$HOME/esp/esp-idf-v5.4/export.sh"
idf.py set-target esp32p4
idf.py build
```

## Flash And Find IP

```bash
./tools/flash-monitor.sh /dev/cu.usbmodem5B5E1314701 --wait-ip --exit-on-ip
```

The detected DHCP address is written to `build/last_ip.txt`. Use the active serial port from `ls /dev/cu.usbmodem*`.

## Pages

```text
http://<board-ip>/
http://<board-ip>/kvm
http://<board-ip>/settings
```

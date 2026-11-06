# HMG01 LAN8720A diagnostic firmware v1

Standalone ESP32 application for the HMG01 V1.1 / ESP32-WROOM-32E board.
This is **not XZG**, not a factory firmware, and not a Zigbee firmware.
It has no web interface, Wi-Fi, DHCP, or Zigbee bridge. Hardware operation
has not yet been tested on the user's board.

It prints at **115200 baud, 8N1**:

- MAC/PHY installation and driver-start errors.
- PHY identification at addresses 0 and 1, followed by driver auto-detection.
- PHY register reads and writes performed during driver initialization.
- Link-up/link-down events and register snapshots every five seconds.
- BMCR reset, power-down, isolate and autonegotiation bits; double-read BMSR
  current link state; PHY address and mode straps.

Wiring is fixed to the connections confirmed for this board: MDC=GPIO23,
MDIO=GPIO18, RMII clock output=GPIO17, PHY reset=GPIO5, USB UART selector=GPIO33 LOW.
GPIO5 is used as the PHY driver's hardware-reset GPIO (the `pwrPin` setting
in Arduino Ethernet); no other wiring combinations are tried.
CC2652P7 reset/bootloader/UART pins and XZG NVS settings are not modified by
the application. It cannot measure supply voltage or verify a 50 MHz waveform.

## Back up before installing

Use a USB data cable. Close any serial monitor before running esptool.
Replace `COM5` with the actual port (`/dev/ttyUSB0` on Linux, for example).
These commands use **esptool 4.x** syntax.

```sh
python -m esptool --chip esp32 --port COM5 read_flash 0x0 0x400000 HMG_before_diag.bin
```

Keep this full 4 MB backup locally. It can contain passwords and other settings;
do not upload it to GitHub.

## Install the application

`HMG_LAN8720_DIAG_v1.app.bin` is an **application-only image**.
**Do not flash it at 0x0. Do not run erase_flash.**

The vendor image `bin/85873_XZG_20240914.bin` was inspected: its application
slots are at `0x10000` and `0x150000`, each 1280 KiB.
The command below replaces both slots so either active OTA slot boots the
diagnostic application. It preserves the bootloader, partition table, NVS,
OTA metadata, filesystem, and the separate Zigbee chip.
Use it only with that verified partition layout.

```sh
python -m esptool --chip esp32 --port COM5 --baud 115200 write_flash \
  0x10000 HMG_LAN8720_DIAG_v1.app.bin \
  0x150000 HMG_LAN8720_DIAG_v1.app.bin
```

On Windows cmd.exe, enter this command on one line without the backslashes.

Connect the known-good Ethernet cable/switch port, open the USB serial monitor
at 115200 8N1, and reboot. Capture the complete output for at least 30 seconds.
Then unplug/replug the Ethernet cable and capture another 15 seconds.
The switch's Link indicator, not a DHCP address, is the intended test.

## Restore normal operation

Close the monitor and restore your full backup:

```sh
python -m esptool --chip esp32 --port COM5 --baud 115200 write_flash 0x0 HMG_before_diag.bin
```

If no backup exists, HamGeek's file is a **full** flash image (unlike the
diagnostic file) and can be restored at `0x0`. This can erase XZG settings
inside the range covered by that image:

```sh
python -m esptool --chip esp32 --port COM5 --baud 115200 write_flash 0x0 ../bin/85873_XZG_20240914.bin
```

## Build and provenance

Install PlatformIO Core and run from this directory:

```sh
pio run -e hmg-diag
```

The output is `.pio/build/hmg-diag/firmware.bin`.
The provided binary was built with ESP32 platform 6.7.0, GCC
8.4.0 esp-2021r2-patch5, Arduino-ESP32 2.0.17 and esptool 4.5.1.
Because the PlatformIO registry was unreachable in the build environment,
the unmodified SDK and compiler were installed from official Espressif
GitHub releases, and SCons was installed locally. The portable configuration
above uses normal PlatformIO dependency resolution.

SHA-256 of the supplied application:

```text
23cbbf8c8785a2376059af2502828caf5c0d36bdbae4a4419fa5f10e4110e1fb
```

The new diagnostic application source is MIT licensed; see `LICENSE`.
The binary also includes Arduino-ESP32 (LGPL-2.1-or-later) and ESP-IDF
components under their respective licenses. Their corresponding source,
license notices and toolchain are available from:

- https://github.com/espressif/arduino-esp32/tree/2.0.17
- https://github.com/espressif/arduino-esp32/releases/tag/2.0.17
- https://github.com/espressif/esp-idf/tree/v4.4.7
- https://github.com/espressif/crosstool-NG/releases/tag/esp-2021r2-patch5

The complete application source and build configuration are supplied here
so the binary can be rebuilt/relinked with modified library sources.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/logo/snapmesh-logo-dark.svg">
  <img src="docs/logo/snapmesh-logo.svg" alt="SnapMesh" width="300">
</picture>

# ESP32-S3 Mesh Snapserver

**Updated:** 2026-10-08 · **Deutsch:** [README.md](README.md)

Several speakers play the same music in sync, with no router, PC or cables in between. The ESP32-S3 boards build their
own wireless network (ESP-Mesh-Lite) and pass the signal on from device to device.

With it you can:

* feed in music from Bluetooth (A2DP), an I2S device or a PC (USB) and hear it everywhere,
* set volume and delay for each speaker,
* make announcements from your phone,
* add a subwoofer through the built-in crossover,
* use existing Snapcast clients (PC, Android, iOS) and Snapcast apps.

At its core are two functions that actually contradict each other: **music** should play without dropouts and gets a
buffer of a few seconds for that. **Announcements** should be heard immediately (no latency). I got that down to
~90 ms.

All devices run the same firmware. One is the **server**, all others are **clients**.

---

## Installation

Firmware and app are under [Releases](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases). Flashing
works from the browser.

**Needed:** per location an ESP32-S3 with at least 4 MB flash and **octal PSRAM** (e.g. N16R8), a data cable, Chrome
or Edge. For the app an Android phone.

1. Open the **[flash page](https://pillepalle127.github.io/ESP32S3_Mesh_Snapserver/)**, connect the board at the
   socket labelled "USB", **Install**, pick "USB JTAG/serial debug unit".
   **For updates, leave "Erase device" unchecked**, or the settings are gone.
2. Join the Wi-Fi **`ESP32_provisioning_…`**, open **http://192.168.5.1/**, enter role, mesh name and password,
   **Save**. The server first, then the clients.
3. Download **`SnapAnnounce-….apk`** from the release to your phone and install it.

Board not showing up: try another cable, or hold BOOT while plugging it in (download mode). Without a browser, use
[esptool](https://github.com/espressif/esptool/releases):
```bash
esptool --chip esp32s3 --before usb_reset write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 snapmesh-app.bin 0x3d0000 ota_data_initial.bin
```

---

## Hardware

I run a **complete system** in an amplifier (PCM5102A, TinySine AudioB as Bluetooth input through a TXB0104) and the
**[SnapStreamer](#snapstreamer-build-idea)**. Any ESP32-S3 with at least 4 MB flash, octal PSRAM and USB-Serial/JTAG
will do, e.g. a YD-ESP32-S3 N16R8
([schematic](https://github.com/vcc-gnd/YD-ESP32-S3/blob/main/5-public-YD-ESP32-S3-Hardware%20info/YD-ESP32-S3-SCH-V1.4.pdf)).

| GPIO | Function |
|---|---|
| 12 | BCLK (PCM5102A BCK, TinySine BCLK) |
| 14 | LRCLK (PCM5102A LCK, TinySine LRCLK) |
| 11 | DIN ← TinySine DOUT |
| 13 | DOUT → PCM5102A DIN |
| 10 | volume pot (optional) |
| – | delay pot, power button (optional) |
| 48 | WS2812 status LED |

The pins can be changed, see [Pins](#pins).

---

## SnapStreamer build idea

You have an amplifier or active speaker and want it in the mesh? The SnapStreamer is my idea for that: ESP32-S3, DAC
and battery packed as tightly as possible into a small case.

<table>
  <tr>
    <td valign="top"><img src="docs/IMG_1684_copy.jpg" alt="Board from above: ESP32-S3 board with the PCM5102A underneath" width="220"></td>
    <td valign="top"><img src="docs/IMG_1695.jpeg" alt="Open case with the board, USB and jack plugged in" width="220"></td>
    <td valign="top"><img src="docs/IMG_1697.jpeg" alt="Closed case, the status LED shines through the lid" width="220"></td>
  </tr>
</table>

The PCM5102A sits on short pin headers under the ESP32-S3 board (GND and VIN crossed). A TP4056 charges through the
ESP's USB socket, the battery is on VIN, and a pot with switch turns it on and sets the volume. Printable case
(FreeCAD, 3MF) in [`mechanics/housing/`](mechanics/housing/).

| Part | Type |
|---|---|
| ESP32-S3 board | ≥ 4 MB flash, octal PSRAM, e.g. YD-ESP32-S3 N16R8 |
| DAC | PCM5102A with jack |
| Charger | TP4056 with protection (DW01) |
| Battery | 18650 Li-ion |
| Pot | 10 kΩ linear with switch |

> [!WARNING]
> Li-ion batteries can catch fire. Only cells and chargers with protection, insulate open contacts, don't charge
> unattended.

---

## Signal path

```text
I2S in / USB (stereo) ─► L+R → mono ─┬─► Opus ─► Snapcast TCP 1704 ─► clients
                                     │
                                     └─► delay line ─► DSP ─► I2S out
```

| Stage, in order | Crossover off | Crossover on | Announcement |
|---|---|---|---|
| Buffer, delay, delay pot | X | X | - |
| Mono (L+R)/2 | X | X | - |
| Subsonic high-pass | - | X | X |
| Gain sub / wideband | - | X | X |
| Sub phase 180° | - | X | X |
| Compressor | X | X | - |
| Pot, volume | X | X | X |
| Limiter −0.5 dBFS, 1.3 ms look-ahead | X | X | X |

**X** available, **-** not available. Announcements bypass everything that delays.

The clients keep their clock aligned with the server. Here the speakers stay within 2–3 ms of each other.

---

## Web UI

<img src="docs/webui-server.png" alt="Settings page of the server with the device list" width="300" align="right">

Every device has a settings page at its IP or `snapserver-<MAC>.local` / `snapclient-<MAC>.local`. The server's page
lists all speakers with name, volume, mute and delay; **Settings** opens a client's settings.

* **Role, mesh / Wi-Fi:** the same on all devices.
* **Buffer (server):** default 3000 ms.
* **Playback (client):** source, volume, mute.
* **USB sound card (server):** see [below](#usb-sound-card).
* **Crossover, subsonic, compressor:** apply immediately.
* **Pins, Opus:** collapsed.

Role, mesh, buffer, USB sound card and pins restart the device.

**Setting up the compressor:** match the branches with the gains, pot and source at maximum. Compressor on, ratio 3,
lower the threshold until loud passages come down by 3–6 dB. Then raise make-up until the limiter rarely steps in.

### USB sound card

The server can be a sound card for your PC on the socket labelled "USB", no driver needed. So far I have tested it on
Linux. While the PC plays, it replaces the I2S input. The volume on the PC applies to the whole mesh, so leave it at
100 % there. Log and flashing then go through the socket labelled "COM" or through **Restart for flashing** on the
server's page.

---

## Pins

The pins can be reassigned in the web UI. If the device does not start with them, it falls back to the default.

* **I2S clock:** normally from the ESP. If another device provides the clock (e.g. an ADAU1701), set it to **External**.
* **Power button:** from a GPIO (1–21) to GND. Hold 2 s = off (deep sleep), hold 1 s = on.
* **Pots:** 10 kΩ for volume and delay on an ADC1 pin (GPIO 1–10), direction reversible.

| Status LED | Meaning |
|---|---|
| blue, slow blink | setup |
| red, fast blink | no network |
| orange, blinking | no server |
| level green to red | playback |
| level, pulsing | announcement |

<br clear="all">

---

## SnapAnnounce App

<img src="docs/snapannounce-icon.png" alt="SnapAnnounce icon" width="72" align="left">

With **SnapAnnounce** you start announcements right in the mesh. No extra hardware, no cables, all from your phone.
Android only, sorry: I don't have a Mac and I'm not buying one.

The music pauses meanwhile. Announcements run without a buffer and therefore reach **only two mesh levels** deep.

<table>
  <tr>
    <td valign="top"><img src="docs/snapannounce-screenshot.jpg" alt="SnapAnnounce, announcement" width="250"></td>
    <td valign="top"><img src="docs/Screenshot_20260923_215400_SnapAnnounce_copy.jpg" alt="SnapAnnounce, devices" width="250"></td>
  </tr>
</table>

---

## License

MIT, see [LICENSE](LICENSE). The licenses of third-party components come with every release as
`THIRD_PARTY_LICENSES.txt`.

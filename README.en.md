<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/logo/snapmesh-logo-dark.svg">
  <img src="docs/logo/snapmesh-logo.svg" alt="SnapMesh" width="300">
</picture>

# ESP32-S3 Mesh Snapserver

**Updated:** 2026-09-29 · **Deutsch:** [README.md](README.md)

Several speakers play the same music in sync, with no router, PC or cables in between. The ESP32-S3 boards build their
own wireless network (ESP-Mesh-Lite) and pass the signal on from device to device.

With it you can:

* feed in one source and hear it everywhere,
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

Firmware and app are ready to use under [Releases](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases).
Flashing works from the browser, no development environment needed.

**You need:** one ESP32-S3 board per location with at least 4 MB flash and **octal PSRAM** (e.g. N16R8, N8R8), a USB
cable with data lines and Chrome or Edge on a PC. For the app, an Android phone.

### 1. Flash the firmware

1. Open the **[flash page](https://pillepalle127.github.io/ESP32S3_Mesh_Snapserver/)**, connect the board (if it has
   two sockets, use the one labelled "USB").
2. **Install**, pick "USB JTAG/serial debug unit", **Connect**.
3. **For updates, leave "Erase device" unchecked**, or the settings are gone.

Board not showing up: try another cable, or hold BOOT and briefly press RST. Without Chrome, use
[esptool](https://github.com/espressif/esptool/releases) and the single files from the release:
```bash
esptool --chip esp32s3 --before usb_reset write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 snapmesh-app.bin 0x3d0000 ota_data_initial.bin
```

### 2. Set up

1. Connect to the open Wi-Fi **`ESP32_provisioning_…`** and open **http://192.168.5.1/**.
2. Enter role, mesh name and password (the same on all devices), **Save**.

Set up the server first, then the clients. More under [Web UI](#web-ui).

### 3. App (Android)

Download **`SnapAnnounce-….apk`** from the [release](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases)
to your phone and install it. Join the mesh Wi-Fi, open the app, done.

---

## Hardware

Two setups run at my place:

* **Complete system** inside an amplifier: PCM5102A as output, TinySine AudioB I2S V2r0 as Bluetooth input, connected
  to the ESP through a TXB0104 level shifter.
* **[SnapStreamer](#snapstreamer-build-idea):** a build idea for a plain receiver with PCM5102A.

Any ESP32-S3 with at least 4 MB flash, octal PSRAM and USB-Serial/JTAG will do, for example a YD-ESP32-S3 N16R8
([schematic V1.4](https://github.com/vcc-gnd/YD-ESP32-S3/blob/main/5-public-YD-ESP32-S3-Hardware%20info/YD-ESP32-S3-SCH-V1.4.pdf)).
Two 10 kΩ pots are optional. The status LED is the WS2812 that many boards already have. You can change the default
pins (see [Pins](#pins)):

| GPIO | Function |
|---|---|
| 12 | BCLK (PCM5102A BCK, TinySine BCLK) |
| 14 | LRCLK (PCM5102A LCK, TinySine LRCLK) |
| 11 | DIN ← TinySine DOUT |
| 13 | DOUT → PCM5102A DIN |
| 10 | volume pot (wiper) |
| – | delay pot (default: none) |
| – | power button to GND (default: none) |
| 48 | WS2812 status LED |

---

## SnapStreamer build idea

You have an amplifier or active speaker and want it in the mesh? The SnapStreamer is my idea for that: ESP32-S3, DAC
and battery packed as tightly as possible into a small case. Heat-shrink tubing or an existing case will do as well.

<table>
  <tr>
    <td valign="top"><img src="docs/IMG_1684_copy.jpg" alt="Board from above: ESP32-S3 board with the PCM5102A below" width="220"></td>
    <td valign="top"><img src="docs/IMG_1687_copy.jpg" alt="From below: PCM5102A module under the ESP32-S3 board" width="220"></td>
    <td valign="top"><img src="docs/IMG_1688_copy.jpg" alt="From the side: GND and VIN crossed" width="220"></td>
  </tr>
</table>

The PCM5102A sits on short pin headers directly under the ESP32-S3 board. The LEDs stay visible, RST and BOOT
reachable, jack and USB sockets are on one level at one end. For this, GND and VIN have to cross (the X in the side
view).

The red and green wires (optional) take the 5 V from the ESP socket to the TP4056: one socket for programming and
charging. Charge current reduced to 500 mA (power dissipation). The battery is on VIN, not on 3V3: the cell reaches
4.2 V, the ESP takes 3.6 V max. The alternative would be a buck-boost converter. A pot with switch turns it on and
sets the volume.

> [!WARNING]
> Li-ion batteries can catch fire when short-circuited, damaged or charged incorrectly. Use only cells and charger
> modules with protection circuits, insulate open contacts, don't squeeze the cell into the case and don't charge
> unattended.

### Parts list

| Part | Type / note | Qty |
|---|---|---|
| ESP32-S3 board | ≥ 4 MB flash, octal PSRAM, e.g. YD-ESP32-S3 N16R8 | 1 |
| Wi-Fi antenna | optional, 2.4 GHz with U.FL cable; only for boards with a U.FL connector, a good antenna improves reception | 1 |
| DAC module | PCM5102A with 3.5 mm jack | 1 |
| Charger module | TP4056 with protection (DW01) | 1 |
| Battery | 18650 Li-ion with cell contacts | 1 |
| Pot with switch | 10 kΩ linear (B10K), switch for the supply, with knob | 1 |
| Case | 3D printed: bottom (PETG), lid (transparent PETG) | 1 |

### Case

<img src="docs/IMG_1689_copy.jpg" alt="Case bottom with 18650 cell and TP4056, the board next to it" width="220">

<table>
  <tr>
    <td valign="top"><img src="docs/IMG_1695.jpeg" alt="Open case with board, USB and jack plugged in" width="340"></td>
    <td valign="top"><img src="docs/IMG_1697.jpeg" alt="Closed case, the status LED shines through the lid" width="340"></td>
  </tr>
</table>

Battery and charger module sit at the bottom of the case, the board above them. USB-C and jack are reachable from the
front, plug forces go into the case. The status LED shines through the lid. The design files are in
[`mechanics/housing/`](mechanics/housing/): the FreeCAD model `Snapstreamer2.FCStd` plus bottom
(`Snapstreamer2-SStreamer GuT.3mf`) and lid (`Snapstreamer2-SStreamer GoT.3mf`) ready to print as 3MF.

---

## Signal path

```text
I2S in (stereo) ─► L+R → mono ─┬─► Opus ─► Snapcast TCP 1704 ─► clients
                               │
                               └─► delay line ─► DSP ─► I2S out
```

The server delays its own speaker by the buffer time so it plays together with the clients. The clients play from
their buffer into the same chain.

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

**X** available, **-** not available.

Only the buffer, the delay and the limiter's look-ahead (1.3 ms) add latency. Announcements bypass everything that
delays.

---

## Synchronisation

The clients keep their clock aligned with the server and play every block at its scheduled time. The clock comes from
a line fitted through the fastest measurements of the last 64 s. A resampler corrects small deviations. When the server
corrects its timeline, all clients jump at the same block. Here the speakers stay within 2–3 ms of each other.

---

## Network

The devices use ports 80, 1704, 1705 and 1706 (UDP). The clients open all connections themselves, so they reach the
server from every mesh level. Via mDNS the devices are called `snapserver-<MAC>.local` or `snapclient-<MAC>.local`.

Without a configuration, after a factory reset or if the mesh repeatedly fails to come up, the device opens the Wi-Fi
`ESP32_provisioning_<MAC>` for setup. After 3 minutes without saving it switches the radio off.

---

## Web UI

<img src="docs/webui-server.png" alt="Settings page of the server with the device list" width="300" align="right">

Every device has a settings page at its IP address or mDNS name. The values survive updates, **Factory Reset** sets
everything back.

### Device list (server)

Name, volume, mute and delay of every speaker, also from Snapcast control apps. The server remembers the values per
device. **Settings** loads a client's settings, however deep it sits in the mesh.

### Settings

* **Role:** server or client.
* **Buffer (server):** reserve against radio dropouts, default 3000 ms, applies to all devices.
* **Playback (client):** source, volume and mute. While a server is connected, the server sets the volume.
* **Mesh / Wi-Fi:** name, password and channel, the same on all devices.
* **Crossover:** crossover frequency, gain for sub and wideband (−24 to +18 dB), sub phase 180°.
* **Subsonic:** high-pass on the sub branch against bass below the tuning, rule of thumb 0.75 × F3.
* **Compressor:** threshold, ratio, make-up.
* **Pins:** see [Pins](#pins).
* **Opus (server):** bitrate and encoder complexity, collapsed.

Crossover, compressor and volume apply as you change them, everything else with **Save**. Role, mesh, buffer and pins
restart the device. The pins are collapsed.

### Setting up the compressor

1. Balance sub and wideband with the gains, the louder branch at 0 dB. Pot and source at maximum.
2. Compressor on, ratio 3, make-up 0 dB.
3. Lower the threshold from −30 dBFS until loud passages are reduced by 3–6 dB.
4. Raise the make-up until the limiter only rarely steps in, by 3 dB at most.
5. Turn the pot through its range: the last part must still get louder, otherwise the make-up is too high.

If it sounds flat or pumps, raise the threshold or lower the ratio. At 1:1 the compressor is plain gain.
Compressor and limiter log their reduction every 5 s on the serial console.

---

## Pins

Pins can be reassigned in the web UI without reflashing. The firmware rejects double assignments and unsuitable pins.
If the device does not start with a new assignment, it falls back to the default pins.

### I2S clock

Normally the ESP generates BCLK and LRCLK. If another device sets the clock, for example a DSP like the ADAU1701,
set **I2S clock** to **External**. Source, DAC and ESP then run on its clock. Without that clock there is no sound.

### Power button

A button from a GPIO (1–21) to GND, no other parts. Hold it for 2 s to switch off, for 1 s to switch on. Off means
deep sleep: only the ESP sleeps, DAC, amplifier and regulator on the battery keep drawing current.

### Pots

Two optional 10 kΩ pots set volume and delay on the device. The wiper goes to an ADC1 pin (GPIO 1–10). The
turning direction can be reversed.

### Status LED

A WS2812 flashes red, green and blue at startup and then shows the state:

| Display | Meaning |
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

The music pauses meanwhile. Since announcements run without a buffer, they are more sensitive to radio dropouts and
therefore reach **only two mesh levels** deep.

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

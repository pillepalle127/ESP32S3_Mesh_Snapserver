# ESP32-S3 Mesh Snapserver

**Stand:** 2026-09-23

Snapcast-kompatibles Mehrraum-Audiosystem auf ESP32-S3, ohne PC oder Raspberry Pi im Betrieb. Eine
Firmware, zwei Rollen, zur Laufzeit per Web-Konfiguration umschaltbar:

* **Server:** ESP-Mesh-Lite-Root. Nimmt I2S-Stereo auf, mischt zu Mono, encodiert Opus und streamt per
  Snapcast-Protokoll an alle Clients. Spielt zeitversetzt synchron auf dem eigenen Lautsprecher mit.
* **Client:** Mesh-Relay (nie Leaf). Empfängt den Stream, synchronisiert auf die Serveruhr und gibt ihn
  über dieselbe DSP/I2S-Kette aus. Wahlweise mit lokalem I2S-Eingang als Alternativquelle.

Offizielle Snapclients (PC, Android, iOS) und Snapcast-Control-Apps funktionieren ebenfalls.

---

## Funktionen

| Bereich | Umsetzung |
|---|---|
| Audio | I2S-Vollduplex 48 kHz, L+R → Mono, Opus (Vorgabe 96 kbit/s, Complexity 5) |
| DSP | LR4-Frequenzweiche (2 × Biquad je Zweig), Gain je Zweig, Kanalzuordnung, live änderbar |
| Sync | Vierzeiten-Zeitabgleich (Minimum-RTT aus 12 Messungen), Drift-Regelung per Resampling |
| Netz | ESP-Mesh-Lite, NAPT zwischen den Ebenen, Provisioning-AP als Rückfall |
| Steuerung | Web-UI + JSON-API (Port 80), Snapcast JSON-RPC (Port 1705), mDNS |
| Geräte | Geräteliste mit Lautstärke, Mute, Delay, Hops; Einstellungen jedes Clients über den Server |
| Hardware | Pinbelegung (I2S, LED, Potis) zur Laufzeit, Potis für Lautstärke und Delay, WS2812-Status-LED |
| Durchsagen | Android-App → UDP 1706, ~90 ms Latenz, Musik pausiert währenddessen |

---

## Hardware

* ESP32-S3 mit PSRAM (getestet: 16 MB Flash, 8 MB Octal-PSRAM, USB-Serial/JTAG)
* Eingang: TinySine AudioB I2S V2r0
* Ausgang: PCM5102A
* optional: 2 × 10-kΩ-Poti, WS2812-LED

Standardbelegung (änderbar, siehe [Pins](#pins)):

| GPIO | Funktion |
|---|---|
| 4 | BCLK (PCM5102A BCK, TinySine BCLK) |
| 6 | LRCLK (PCM5102A LCK, TinySine LRCLK) |
| 5 | DIN ← TinySine DOUT |
| 7 | DOUT → PCM5102A DIN |
| 10 | Lautstärke-Poti (Schleifer) |
| – | Delay-Poti (Vorgabe: keiner) |
| 48 | WS2812-Status-LED |

<img src="docs/Verdrahtungsplan.png" width="600">

---

## Signalweg

```text
I2S in (Stereo) ─► L+R → Mono ─┬─► Opus ─► Snapcast TCP 1704 ─► Clients
                               │
                               └─► Delay-Line (bufferMs + trim) ─► LR4 ─► Low/High ─► Gain/Vol ─► I2S out
```

Die Weiche arbeitet auf dem Monosignal. Die lokale Ausgabe des Servers wird um `bufferMs + delay_trim_ms`
verzögert, damit sie mit den Clients zusammen spielt. Der Netzwerk-Stream bleibt unverzögert. Die
Poti-Lautstärke greift am Ende der Ausgabestufe und beeinflusst den Stream nicht.

---

## Synchronisation (Client)

* Zeitabgleich über `SNAP_MSG_TIME`. Aus einem Fenster von 12 Messungen zählt die mit der kleinsten RTT;
  Mitteln würde verzögerte Pakete einrechnen.
* Soll-Abspielzeit je Chunk: `ts − offset + bufferMs − latency + delay_trim_ms`, verglichen mit
  `esp_timer` + 40 ms DMA-Latenz.
* Fehler > 100 ms: harter Resync (Überspringen bzw. Stille). Darunter PI-Regler auf das Resampling-Verhältnis,
  begrenzt auf ±500 ppm, Slew 5 ppm pro 20-ms-Frame. Resampler mit 32.32-Phasenakkumulator.
* Ringpuffer = 2 × `bufferMs` (PSRAM), Wiedergabe startet ab 80 % Soll-Füllung.
* Gemessen: Regelfehler wenige ms. Synchronität mehrerer Clients über Stunden ist noch nicht nachgemessen.

Ohne RTC/SNTP nutzt der Server `esp_timer` plus Offset als Zeitbasis. Eine plausible Wanduhr (> 2024) übernimmt
er einmalig vom ersten PC-/Android-Client. ESP-Clients melden nur ihre Uptime und werden dafür ignoriert.

---

## Netzwerk

| Port | Proto | Zweck |
|---|---|---|
| 80 | TCP | Web-UI, JSON-API |
| 1704 | TCP | Snapcast-Stream; zusätzlich Konfig-Kanal Server → eigene Clients |
| 1705 | TCP | Snapcast JSON-RPC, `Voice.Start`/`Voice.Stop` |
| 1706 | UDP | Durchsagen (Handy → Server → Clients) |

* Mesh-Ebenen sind durch NAPT getrennt: Ab Ebene 3 (2 Hops) ist ein Client vom Server aus nicht adressierbar.
  Deshalb laufen alle Wege zum Client über Verbindungen, die der Client selbst aufbaut.
* Clients finden den Server über `esp_mesh_lite_get_root_ip()` (funktioniert über alle Ebenen, anders als
  DHCP-Gateway oder mDNS). Eine feste Server-Adresse ist konfigurierbar.
* mDNS-Name pro Gerät: `snapserver-<MAC>.local` bzw. `snapclient-<MAC>.local` (letzte 3 Bytes der SoftAP-MAC).
* WLAN-Powersave ist in beiden Rollen aus (`WIFI_PS_NONE`).
* Fusion-Intervall 20 s, damit eine Mesh-Insel nach einem Root-Ausfall wieder zusammenfindet.

### Provisioning-AP

Offener AP `ESP32_provisioning_<MAC>` mit derselben Web-UI. Er startet bei fehlender Konfiguration, nach
Factory Reset, bei deaktiviertem Mesh oder nach 7 Boots in Folge ohne Station am Mesh-AP. Nach 3 Minuten ohne
Speichern schaltet er den Funk ab; ein neues Fenster gibt es erst nach einem Power-Cycle. Speichern startet das
Gerät neu.

### Snapcast-Erweiterungen

Alle Felder sind optional; fremde Clients und Server ignorieren sie.

| Wo | Feld | Bedeutung |
|---|---|---|
| Hello | `"SnapMesh":1` | eigener Client: versteht `announcement` und Nachrichtentyp 100 |
| Hello | `"MeshLevel":n` | Mesh-Ebene (Root = 1), ergibt die Hops in der Geräteliste |
| ServerSettings | `"announcement":bool` | Durchsage läuft, Musik pausieren |
| Nachrichtentyp 100 | JSON-Request/Antwort | Konfig-Anfrage Server → Client, `refersTo` = Request-ID |
| `Server.GetStatus` | `"snapmesh":{"hops","own"}` | Hops und Herkunft je Client |

Fremde Clients bekommen während einer Durchsage `muted:true`, weil sie den UDP-Kanal nicht empfangen.

---

## Web-UI und API

Jedes Gerät bietet die Seite auf Port 80 an. Alle Werte liegen im NVS und überleben Updates.

| Gruppe | Felder | Übernahme |
|---|---|---|
| Rolle | Server/Client | Neustart |
| Client-Wiedergabe | Quelle (Auto/Netz/lokal), Eingangsschwelle, `buffer_ms` (200–10000), Delay-Trim (±2000 ms), Server-Adresse | sofort; `buffer_ms` Neustart |
| Mesh | Enable, SSID, Passwort, Kanal (1–13), max. Hops (1–15) | Neustart |
| DSP | Enable, Trennfrequenz (40–500 Hz), Gain Sub/Wideband (−24…+12 dB), Sub-Kanal | sofort |
| Pins | I2S, LED, Potis, Delay-Poti-Bereich | Neustart; Bereich sofort |
| Opus | Bitrate (16–192 kbit/s), Complexity (0–10) | sofort, nur Server |

Kconfig (`idf.py menuconfig`) liefert nur die Vorgaben für den ersten Start und den Factory Reset;
`SNAPSERVER_STATUS_LED_ENABLE` und `SNAPSERVER_POTS_ENABLE` nehmen LED- bzw. Poti-Code ganz heraus. Factory Reset
setzt Konfiguration, Pins, Potis und die gespeicherten Client-Werte zurück.

### Geräteliste (Server)

Oben auf der Server-Seite, ein Eintrag je belieferten Lautsprecher:

* **Name** (inline umbenennbar), **Hops**, **Lautstärke**, **Mute**, **Delay** (ms, positiv = später;
  intern Snapcast-`latency` mit umgekehrtem Vorzeichen). Änderungen wirken sofort.
* Pro Client-ID (MAC) im NVS gespeichert (`client_store.c`, max. 24, LRU) und bei jedem Hello wieder eingespielt.
  Das gilt auch für Werte aus Control-Apps.
* **Settings** lädt die Einstellungen des Geräts in das Formular darunter. Die Überschrift zeigt den Namen,
  Speichern geht an dieses Gerät. Das funktioniert für eigene Clients in jeder Tiefe: Der Server sendet die
  Anfrage als Nachrichtentyp 100 über die Snapcast-Verbindung des Clients (`snapserver_remote_request()` →
  `webconfig_handle_remote_request()`). Nach einem Neustart zeigt die Seite „not connected“ und lädt neu,
  sobald der Client wieder verbunden ist. Factory Reset ist nur lokal möglich.
* Fremde Snapcast-Clients: Badge „Snapcast“, nur Lautstärke/Mute/Delay. Hops = 1, wenn ihre MAC direkt am
  Server-AP hängt, sonst unbekannt.

### Endpunkte

| Methode | Pfad | Inhalt |
|---|---|---|
| GET/POST | `/api/config` | Konfiguration des Geräts; POST antwortet `{"reboot":bool}` |
| GET | `/api/status` | Provisioning-Grund, Uptime, Poti-Werte, Pin-Probestatus, Server-Verbindung (Client) |
| POST | `/api/factory-reset` | Reset + Neustart |
| GET | `/api/devices` | Geräteliste (nur Server) |
| POST | `/api/devices` | `{"id", "volume_percent"\|"muted"\|"delay_ms"\|"name"}` |
| GET/POST | `/api/devices/config?id=` | `/api/config` eines Clients über den Server |
| GET | `/api/devices/status?id=` | `/api/status` eines Clients über den Server |

Fehler bei `?id=`: 404 = nicht verbunden bzw. kein eigener Client, 504 = keine Antwort (3 s, Status 2 s),
400 = vom Client abgelehnt.

---

## Pins

Alle Funktionen werden zur Laufzeit im Abschnitt *Pins* belegt; Neu-Flashen ist nicht nötig.

* I2S BCLK/LRCLK/DIN/DOUT sind immer belegt. LED, Lautstärke- und Delay-Poti können „none“ sein.
* Vorlagen: *Standard* 4/6/5/7, *Alternative* 17/8/5/18. Kollidiert die LED oder ein Poti mit der Vorlage,
  wird es auf „none“ gesetzt.
* Jeder Pin trägt eine Funktion. Die Firmware prüft die gesamte Belegung beim Speichern erneut, auch per API.
* **Probestart:** Eine geänderte Belegung zählt bei jedem Boot hoch und wird nach vollständigem Start bestätigt
  (`device_config_confirm_pins()`). Nach 3 unbestätigten Boots fällt das Gerät auf die Standardbelegung zurück
  und meldet das im Status. Eine falsch verdrahtete, aber zulässige Belegung erkennt die Firmware nicht.
* Gesperrte GPIOs (`pinmap.c`):

| GPIO | Grund |
|---|---|
| 0, 3, 45, 46 | Strapping |
| 19, 20 | USB |
| 22–25 | nicht vorhanden |
| 26–32 | SPI-Flash, PSRAM-CS |
| 33–37 | Octal-PSRAM (`CONFIG_SPIRAM_MODE_OCT`) |
| 43, 44 | UART0-Konsole (falls aktiv) |

### Potis

10 kΩ, Enden an 3V3/GND, Schleifer an GPIO. **Nur ADC1 (GPIO 1–10)**, weil ADC2 bei aktivem WLAN nicht lesbar
ist. Bei Standardbelegung sind 1, 2, 8, 9 und 10 frei.

* **Lautstärke** (Vorgabe GPIO 10): Pull-up, ohne Poti also 100 %. Kubische Kennlinie, multipliziert mit der
  Snapcast-Lautstärke. Wirkt nur auf den lokalen Lautsprecher.
* **Delay** (Vorgabe: keiner): ersetzt das Feld Delay-Trim. Mitte = 0 ms, Anschläge = ±Bereich (Vorgabe 200 ms,
  max. 2000 ms), linear. Pull-down, ein offener Pin steht also am negativen Anschlag.
* Abtastung alle 50 ms, Mittel aus 16 Messungen, ~1 % Totband (Endanschläge ausgenommen). Die Lautstärke wird
  über 20 ms eingeblendet.
* Fehler durch Pull-up/Pull-down: ±2,6 % in Mittelstellung, an den Enden exakt.
* Delay-Sprünge > 100 ms lösen auf Clients einen harten Resync aus. Kleinere Änderungen gleicht die
  Drift-Regelung aus (≤ 0,5 ms/s).
* Kein ADC: Lautstärke bleibt bei 100 %, Delay beim Feldwert.

---

## Sprachdurchsagen

Eigener Kanal neben dem Stream: niedrige Latenz statt Lückenlosigkeit.

1. App → `Voice.Start` über JSON-RPC 1705 (TCP, damit Start/Stopp sicher ankommen).
2. Opus 16 kHz Mono (~25 kbit/s) per UDP an Port 1706 des Servers.
3. Der Server spielt die Durchsage selbst und sendet sie an seine direkten Clients. Diese leiten sie einen Hop
   weiter, tiefere Knoten bekommen sie nicht.
4. Ende durch `Voice.Stop`, 1 s Stille, 3 min Maximaldauer oder Abbruch der Steuerverbindung.

* Latenz Mund → Lautsprecher ≈ 90 ms (Aufnahme/Encoder 30 ms, WLAN 6 ms, Puffer 10–20 ms, I2S 40 ms).
* Während der Durchsage pausieren alle eigenen Clients die Musik (`announcement`). Stream und Zeitachse laufen
  weiter, danach geht es ohne Neupuffern weiter. Lautstärke und Mute gelten auch für die Durchsage.
* Grenzen: keine Echounterdrückung gegenüber den Lautsprechern; eine Durchsage zur Zeit (sonst `busy`);
  das Handy muss im Mesh-WLAN sein.

**App** `android/SnapAnnounce` (Kotlin, Compose, ab Android 8), zwei Tabs:

* **Durchsage:** verriegelnder Sprechknopf, Foreground-Service mit WLAN-Lock. Einstellungen: Mikrofonquelle,
  max. Verstärkung, Durchsage-Pegel (AGC mit Limiter; Musik liegt bei etwa −24 bis −28 dBFS).
* **Geräte:** native Geräteliste über `/api/devices` (Poll 3 s): Lautstärke, Mute, Delay (±10/±100 ms, 0,7 s
  gesammelt), Umbenennen. **Einstellungen** öffnet die Firmware-Seite eines Geräts in einer WebView
  (`/?device=<id>&embed=1`: Gerät vorausgewählt, ohne eigene Liste). Solange sie offen ist, ist der Prozess an das
  Mesh-WLAN gebunden (`bindProcessToNetwork`), sonst ginge der Traffic über mobile Daten. Cleartext-HTTP ist
  erlaubt, weil die Server-Adresse frei einstellbar ist.

<img src="docs/snapannounce-screenshot.jpg" alt="SnapAnnounce" width="320">

---

## Build

ESP-IDF 5.4.x (getestet 5.4.3), Target `esp32s3`:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Ein Update ohne `erase_flash` behält Rolle, Pins und alle Einstellungen im NVS. Hängt der Reset über
USB-Serial/JTAG mit Schreib-Timeout, hilft `esptool.py --before usb_reset … write_flash @flash_args` aus `build/`.

Android-App: `android/SnapAnnounce` in Android Studio öffnen, oder mit Gradle 8.13 und JDK 21:

```bash
cd android/SnapAnnounce
JAVA_HOME=/pfad/zu/jdk-21 gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

(`gradlew` ist nicht im Repository, nur `gradle/wrapper/gradle-wrapper.properties`.)

---

## Struktur

```text
main/
├── app_main.c         Rollenwahl, Startreihenfolge
├── audio_i2s.c        I2S, Mono-Mix, LR4, Delay-Line (Server)
├── audio_opus.c       Opus-Encoder
├── audio_sink.c       Wiedergabe, Quellenwahl, Drift-Regelung (Client)
├── audio_resample.c   Resampler 32.32
├── snapserver.c       Snapcast-Server 1704, Konfig-Kanal zu Clients
├── snapclient.c       Snapcast-Client
├── snapcontrol.c      JSON-RPC 1705
├── voice_announce.c   Durchsagen, UDP 1706
├── mesh_root.c        Mesh-Root
├── mesh_client.c      Mesh-Relay
├── webconfig.c        Web-UI + API, Port 80
├── device_config.c    Konfiguration, Pins, Potis im NVS
├── pinmap.c           nutzbare GPIOs
├── client_store.c     gespeicherte Client-Werte
├── pots.c             Potis
├── provisioning.c     Provisioning-AP
├── status_led.c       WS2812
└── cpu_stats.c        CPU-Last je Task
android/SnapAnnounce/  Durchsage-App
docs/, tools/          Verdrahtung, Screenshots, Testskripte
```

---

## Status

Stabil im Betrieb: Streaming und Sync über das Mesh, LR4, Rollenwechsel, Web-UI, Durchsagen.

Offen (Details und Messwerte in [TODO.md](TODO.md)):

* Relays mit mehreren Kindern hängen sich gelegentlich auf.
* Ein blockierter Client belastet den internen Heap des Servers zu lange.
* Akustische Artefakte, die bereits im aufgenommenen Signal stecken.
* Messung des Versatzes zwischen Server- und Client-Lautsprechern.
* Auf Hardware noch ungetestet: Client-Einstellungen über den Server, Rückfall beim Pin-Probestart.

---

## Abhängigkeiten und Lizenz

ESP-IDF, ESP-Mesh-Lite, ESP-IoT-Bridge, ESP-Modem, ESP-mDNS, CMake Utilities (Apache 2.0); esp-opus (MIT).
Drittkomponenten unterliegen ihren eigenen Lizenzen.

Dieses Projekt: MIT License.

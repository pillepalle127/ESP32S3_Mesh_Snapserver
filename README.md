# ESP32-S3 Mesh Snapserver

**Stand:** 2026-09-23

Snapcast-kompatibles Mehrraum-Audiosystem auf ESP32-S3. Eine Firmware, zwei Rollen, zur Laufzeit per
Web-Konfiguration umschaltbar:

* **Server:** ESP-Mesh-Lite-Root. Nimmt I2S-Stereo auf, mischt zu Mono, encodiert Opus und streamt per
  Snapcast-Protokoll an alle Clients. Spielt zeitversetzt synchron auf dem eigenen Lautsprecher mit.
* **Client:** Mesh-Relay (nie Leaf). Empfängt den Stream, synchronisiert auf die Serveruhr und gibt ihn
  über dieselbe DSP/I2S-Kette aus. Wahlweise mit lokalem I2S-Eingang als Alternativquelle.

Offizielle Snapclients (PC, Android, iOS) und Snapcast-Control-Apps funktionieren ebenfalls.

---

## Installation

Firmware und App lassen sich **ohne Programmierkenntnisse** installieren. Es muss **keine Entwicklungsumgebung
und keine Toolchain** eingerichtet werden: Die Firmware wird direkt aus dem Browser auf das Board geschrieben,
die App ist eine normale Android-Installationsdatei. Beides wird automatisch aus diesem Repository gebaut und
bei jeder neuen Version unter [Releases](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases)
bereitgestellt.

### Was du brauchst

* **Ein ESP32-S3-Board je Standort** (ein Board versorgt dort z. B. Lautsprecher und Subwoofer über die
  eingebaute Frequenzweiche) mit **mindestens 4 MB Flash und Octal-PSRAM**, z. B. die Varianten
  „N16R8“ oder „N8R8“ (etwa das YD-ESP32-S3 N16R8). Auf Boards mit Quad-PSRAM (z. B. N8R2, N16R2) oder ganz
  ohne PSRAM (z. B. N16) startet diese Firmware nicht. Die Bezeichnung steht in der Produktbeschreibung oder
  auf dem Modul.
* **Ein USB-Kabel, das Daten überträgt.** Viele Kabel, die bei Geräten liegen, können nur laden; dann taucht
  das Board am PC nicht auf.
* **Einen PC oder Laptop mit Chrome oder Edge** (Windows, macOS oder Linux). Firefox, Safari und Handys können
  nicht flashen.
* Für die App: ein **Android-Handy** (ab Android 8). Für iPhones gibt es keine App; die Einstellungen gehen dort
  im Browser.

### 1. Firmware aufspielen

1. Die **[Flash-Seite](https://pillepalle127.github.io/ESP32S3_Mesh_Snapserver/)** in Chrome oder Edge öffnen.
2. Das Board per USB an den PC anschließen. Hat es zwei USB-Buchsen, die nehmen, die direkt zum ESP32-S3 führt
   (oft mit „USB“ beschriftet, nicht „COM“ oder „UART“).
3. Auf **Installieren** klicken. Im Fenster, das der Browser öffnet, den Eintrag **„USB JTAG/serial debug
   unit“** wählen und **Verbinden**.
4. Im nächsten Dialog die Installation bestätigen:
   * **Neues Board:** Das Häkchen **„Erase device“** darf gesetzt werden, es löscht alte Inhalte.
   * **Update eines Boards, das schon läuft:** **„Erase device“ nicht anhaken.** Dann bleiben alle
     Einstellungen erhalten.
5. Etwa eine Minute warten und das Kabel währenddessen nicht abziehen. Wenn die Seite „Installation complete“
   meldet, ist das Board fertig und startet neu.

**Wenn das Board nicht in der Liste erscheint:** anderes Kabel oder andere Buchse probieren. Hilft das nicht,
den Knopf **BOOT** gedrückt halten, kurz **RESET** (RST) drücken, BOOT loslassen und es erneut versuchen. Unter
Linux muss dein Benutzer in der Gruppe `dialout` sein.

### 2. Neues Gerät einrichten

Nach der ersten Installation weiß das Board noch nicht, welche Aufgabe es hat. Es öffnet deshalb ein eigenes,
offenes WLAN namens **`ESP32_provisioning_…`**.

1. Handy oder Laptop mit diesem WLAN verbinden. Meldet das Handy „kein Internet“: trotzdem verbunden bleiben.
2. Im Browser **http://192.168.5.1/** öffnen.
3. Einstellen:
   * **Rolle:** **Server** für genau ein Gerät, nämlich das, an dem die Musikquelle hängt (TinySine-Eingang).
     **Client** für alle weiteren Lautsprecher.
   * **Mesh:** Name (SSID) und Passwort des Lautsprecher-Netzes. **Auf allen Geräten dieselben Werte** eintragen;
     der Server baut das Netz damit auf, die Clients verbinden sich damit.
   * **Pins:** nur ändern, wenn die Verdrahtung von der [Standardbelegung](#hardware) abweicht. Die Vorlage
     „Alternative“ setzt die zweite übliche Belegung.
4. **Save** drücken. Das Gerät startet neu und verbindet sich mit dem Mesh.

Am besten zuerst den Server einrichten, danach die Clients nacheinander. Jeder Client erscheint anschließend in
der Geräteliste des Servers.

**Danach:** Mit dem Mesh-WLAN verbinden (der Name und das Passwort von eben) und **http://192.168.5.1/**
öffnen. Dort ist die Seite des Servers mit allen Lautsprechern: Lautstärke, Stummschaltung und Verzögerung je
Gerät, und über **Settings** die Einstellungen jedes Clients.

### 3. Updates

Ein Update geht genauso wie die Installation (Schritt 1), nur **ohne „Erase device“**. Rolle, Mesh, Pins und
alle anderen Einstellungen bleiben erhalten. Jedes Gerät wird einzeln per USB aktualisiert, der Server und
jeder Client. Welche Version ein Gerät hat, steht in seinem Statusfeld (`firmware: v…`).

### 4. App installieren (Android)

1. Auf dem Handy die [Release-Seite](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases) öffnen
   und die Datei **`SnapAnnounce-….apk`** herunterladen.
2. Die Datei öffnen. Android fragt, ob der Browser Apps installieren darf: **erlauben**, dann **Installieren**.
3. Das Handy mit dem Mesh-WLAN verbinden und die App öffnen. Die Server-Adresse `192.168.5.1` ist voreingestellt.
   * **Durchsage:** Knopf drücken und sprechen, noch einmal drücken zum Beenden.
   * **Geräte:** alle Lautsprecher mit Lautstärke, Stummschaltung und Verzögerung; **Einstellungen** öffnet die
     Konfiguration eines Geräts.

**Berechtigungen:** Beim ersten Druck auf den Durchsage-Knopf fragt Android nach zwei Rechten:

| Recht | Wozu | Was wählen |
|---|---|---|
| **Mikrofon** | Die Durchsage aufnehmen. Ohne dieses Recht gehen keine Durchsagen, die Geräteliste funktioniert trotzdem. | „Während der Nutzung der App“ genügt |
| **Benachrichtigungen** (ab Android 13) | Solange eine Durchsage läuft, zeigt eine Benachrichtigung, dass das Mikrofon offen ist, auch bei gesperrtem Bildschirm. Dort lässt sich die Durchsage auch beenden. | **Zulassen** |

Alle weiteren Rechte (WLAN- und Netzwerkzugriff, WLAN wach halten während einer Durchsage, Vordergrunddienst)
erteilt Android bei der Installation ohne Nachfrage. Standort, Kontakte, Speicher oder Kamera braucht die App
nicht. Wurde ein Recht versehentlich abgelehnt: Android-Einstellungen → Apps → SnapAnnounce → Berechtigungen.

Eine selbst gebaute Version der App vorher deinstallieren, sonst verweigert Android das Update (andere Signatur).
Neue Versionen der App lassen sich danach einfach darüber installieren.

### Alternativ: Flashen mit esptool

Statt über die Flash-Seite geht es auch mit dem eigenständigen
[esptool](https://github.com/espressif/esptool/releases) (kein Python nötig, auch ohne Chrome oder Edge) und den
vier Dateien aus dem Release:
```bash
esptool --chip esp32s3 --before usb_reset write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 snapmesh-app.bin 0x3d0000 ota_data_initial.bin
```
`ota_data_initial.bin` setzt den OTA-Datenbereich zurück, damit das Board die gerade geschriebene App startet.
`snapmesh-full.bin` ist ein Gesamt-Image ab `0x0` für eine Neuinstallation. Es füllt NVS und PHY-Daten
(`0x9000–0xFFFF`) mit `0xFF` und löscht damit alle Einstellungen, für Updates also nicht verwenden.

---

## Funktionen

| Bereich | Umsetzung |
|---|---|
| Betrieb | Eigenständig: Auch der Server läuft auf einem ESP32-S3, im Betrieb ist kein PC oder Raspberry Pi nötig |
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

* ESP32-S3 mit mindestens 4 MB Flash und Octal-PSRAM (N16R8, N8R8; USB-Serial/JTAG), z. B. YD-ESP32-S3 N16R8
  von VCC-GND Studio ([Schaltplan V1.4](https://github.com/vcc-gnd/YD-ESP32-S3/blob/main/5-public-YD-ESP32-S3-Hardware%20info/YD-ESP32-S3-SCH-V1.4.pdf))
* Eingang: TinySine AudioB I2S V2r0 mit Pegelwandler TXB0104 (siehe [Module an den Schnittstellen](#module-an-den-schnittstellen))
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

---

## Aufbau der Streamer

<img src="docs/IMG_1684.JPG" alt="Streamer von oben: ESP32-S3-Board mit dem PCM5102A darunter" width="280">
<img src="docs/IMG_1687.JPG" alt="Streamer von unten: PCM5102A-Modul unter dem ESP32-S3-Board" width="280">
<img src="docs/IMG_1688.JPG" alt="Streamer von der Seite: GND und VIN gekreuzt" width="280">

Der Aufbau spart Platz und Lötarbeit: Das PCM5102A-Modul sitzt direkt unter dem ESP32-S3-Board, über kurze
Stiftleisten verbunden. Die LEDs bleiben von oben sichtbar, RST- und BOOT-Taster zugänglich. Klinkenbuchse und
USB-Buchsen liegen an derselben Stirnseite auf einer Ebene, das vereinfacht den Bau des Gehäuses.

GND und VIN müssen dabei gekreuzt werden (in der Seitenansicht als X zu sehen).

Die rote und die grüne Leitung auf den Fotos gehören nicht zwingend zum Aufbau. Sie sind hier nur nötig, weil der
Strompfad von der USB-Buchse des ESP aufgetrennt wurde, um eine 18650-Zelle gezielt über einen TP4056 laden zu
können (Details folgen noch).

---

## Module an den Schnittstellen

### Ausgang: PCM5102A

Stereo-DAC von TI mit Line-Ausgang. Er erzeugt seine negative Versorgung selbst (Ladungspumpe), der Ausgang
liegt deshalb symmetrisch um Masse und braucht keine Koppelkondensatoren. Aus 3,3 V liefert er bei Vollaussteuerung
**2,1 V<sub>eff</sub>** und ist damit direkt zu gängigen Endstufen kompatibel, z. B. zum **TPA3255**. Der Ausgang
verträgt Lasten ab etwa 1 kΩ. Bei den üblichen rund 10 kΩ Eingangsimpedanz lassen sich also **etwa 8
Verstärkereingänge parallel** betreiben.

Der ESP liefert kein MCLK. Der PCM5102A erkennt, dass an SCK kein Takt anliegt, und erzeugt seinen Systemtakt
dann selbst per PLL aus BCK. SCK bleibt also frei; auf GND gelegt (auf den Modulen meist eine Lötbrücke) ist die
Erkennung robuster gegen Störungen, nötig ist es in der Regel nicht. Außerdem: FMT auf GND (I2S-Format), XSMT
auf High (nicht stummgeschaltet).

### Eingang: TinySine AudioB I2S

Bluetooth-Empfänger mit I2S-Ausgang (48 kHz, 16 Bit), unterstützt u. a. **aptX**. Klang, Reichweite (eigene
Antenne) und Verbindungsverhalten sind sehr gut. Er muss hier als **I2S-Slave** konfiguriert sein; der ESP
liefert BCLK und LRCLK.

Einen Haken hat er: Seine I2S-Leitungen arbeiten mit **1,8 V**. Der ESP32-S3 erkennt „High“ laut Datenblatt erst
ab etwa 2,5 V. Direkt angeschlossen kippen deshalb je nach Start einzelne oder sehr viele Bits, hörbar als
Rauschen und Knacksen, das von selbst kommt und geht. Abhilfe ist ein **Pegelwandler** wie der **TXB0104**
zwischen Modul und ESP:

| TXB0104 | anschließen an |
|---|---|
| VCCA | 1,8 V (Seite TinySine) |
| VCCB | 3,3 V (Seite ESP) |
| OE | VCCA |
| A1 ↔ B1 | TinySine BCK ↔ ESP BCLK |
| A2 ↔ B2 | TinySine LRCK ↔ ESP LRCLK |
| A3 ↔ B3 | TinySine SD ↔ ESP DIN |
| A4 | GND |

Der PCM5102A bleibt direkt am ESP. Ein I2C-Isolator wie der ISO1540 eignet sich nicht (Open-Drain, zu langsam
für 3 MHz). Nach dem Ende einer Wiedergabe treibt der TinySine SD einige Sekunden nicht; die Firmware hält DIN
deshalb mit einem Pull-down fest (Details in `TODO.md`, Fehler A und C).

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

<img src="docs/Screenshot_20260923_220820_Firefox.jpg" alt="Konfigurationsseite des Servers: Geräteliste und Einstellungen eines Clients" width="280">

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

Ein Vorwiderstand ist für den Betrieb nicht nötig: Durch das Poti fließen 0,33 mA, und der ADC-Eingang ist
hochohmig. Empfohlen sind trotzdem **1 kΩ zwischen Schleifer und GPIO** als Schutz. Die Pins sind zur Laufzeit
umbelegbar, und wird ein Poti-Pin versehentlich als Ausgang vergeben, schließt der Schleifer am Anschlag den
Ausgang sonst direkt gegen 3V3 oder GND kurz. Zusammen mit dem internen Pull-up bzw. Pull-down (~45 kΩ)
verschiebt der Widerstand einen der beiden Anschläge um etwa 2 %, sonst ändert er an der Messung nichts.

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

### Status-LED

Eine WS2812 (Vorgabe GPIO 48) zeigt Zustand und Pegel. Beim Start blitzt sie kurz rot, grün, blau (Selbsttest;
fehlt er, stimmt der LED-Pin nicht).

| Anzeige | Bedeutung |
|---|---|
| weiß, gedimmt | Start |
| blau, blinkt langsam (~2 s) | Provisionierung: offenes WLAN `ESP32_provisioning_…` |
| rot, blinkt schnell (~0,25 s) | kein Netz (Client) |
| orange, blinkt (~1 s) | Netz, aber kein Snapserver (Client) |
| Pegelanzeige | Wiedergabe |
| Pegelanzeige, alle ~3 s kurz dunkel | lokaler Eingang (A2DP) ohne Serververbindung |
| Pegelanzeige, pulsiert schnell (~0,5 s) | Sprachdurchsage |

Vorrang: Provisionierung vor Durchsage vor lokalem Eingang vor Verbindungszustand. Spielt ein Client den lokalen
Eingang ohne Server, zeigt er also den Pegel statt des orangen Blinkens.

**Pegelanzeige:** Die Farbe ist der Pegel, von Grün über Gelb nach Rot, die Helligkeit steigt mit (Grundhelligkeit
30 %). Gemessen wird der RMS-Pegel je 20 ms **vor** der Lautstärke, auf dem Client vor der Snapcast-Lautstärke,
auf dem Server vor Frequenzweiche, Poti und lokaler Lautstärke. Die LED zeigt also das Signal, nicht wie laut
der Lautsprecher gestellt ist, und bewegt sich auch bei leiser oder stummer Box. Skala −35 dBFS (grün) bis
−6 dBFS (rot), sofortiger Anstieg, Abklingen in ~100 ms (`LED_LEVEL_FLOOR_DB`/`LED_LEVEL_CEIL_DB` in
`status_led.c`).

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

<img src="docs/snapannounce-screenshot.jpg" alt="SnapAnnounce, Durchsage" width="280">
<img src="docs/Screenshot_20260923_215400_SnapAnnounce.jpg" alt="SnapAnnounce, Geräte" width="280">

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

### ESP-IDF-Konfiguration

**`sdkconfig.defaults` ist die vollständige Konfiguration.** `sdkconfig` liegt nicht im Repository; ein frischer
Build erzeugt es aus `sdkconfig.defaults` und kommt dabei exakt auf den Stand der Geräte (geprüft mit einem
leeren Klon). Die Release-Pipeline baut nur daraus. Wer eine Option per `idf.py menuconfig` ändert, muss sie
in `sdkconfig.defaults` nachtragen, sonst baut CI etwas anderes; `idf.py save-defconfig` zeigt die
Abweichungen.

**Versionen:** ESP-IDF **5.4.3** (in CI fest), Komponenten laut `main/idf_component.yml`, aufgelöst in
`dependencies.lock`:

| Komponente | Version | Zweck |
|---|---|---|
| `espressif/mesh_lite` | 1.0.2 | Mesh-Netz (zieht `iot_bridge` 1.0.1, `esp_modem`, `tinyusb` u. a. nach) |
| `esphome/micro-opus` | 0.4.1 | Opus-Encoder und -Decoder |
| `espressif/mdns` | 1.13.1 | `snapserver-<MAC>.local` / `snapclient-<MAC>.local` |

**Einstellungen in `sdkconfig.defaults`** (Begründungen als Kommentar in der Datei):

| Bereich | Option | Wert | Grund |
|---|---|---|---|
| Flash | `ESPTOOLPY_FLASHSIZE_4MB` | y | Mindestgröße im Image-Header: läuft auf 4-, 8- und 16-MB-Boards (IDF bricht nur bei kleinerem Chip ab) |
| | `PARTITION_TABLE_CUSTOM` | `partitions.csv` | 4-MB-App-Partition, siehe unten |
| CPU | `ESP32S3_DEFAULT_CPU_FREQ_240` | y | Opus-Encoder und DSP |
| PSRAM | `SPIRAM`, `SPIRAM_MODE_OCT`, `SPIRAM_SPEED_80M` | y | Octal-PSRAM (N16R8, N8R8); Quad-PSRAM bräuchte `SPIRAM_MODE_QUAD` |
| | `SPIRAM_USE_CAPS_ALLOC` | y | PSRAM nur gezielt (Puffer, Task-Stacks), `malloc()` bleibt intern |
| | `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | y | WLAN-/lwIP-Puffer ins PSRAM; ein Sendestau fraß sonst den internen RAM |
| WLAN | `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 10 | getestete Werte; ohne sie setzt IDF mit der Option oben 16 |
| | `ESP_WIFI_RX_BA_WIN` | 6 | dito, IDF-Vorgabe wäre 16 |
| lwIP | `LWIP_TCP_SND_BUF_DEFAULT`, `LWIP_TCP_WND_DEFAULT` | 2880 | 2 × MSS; größere Fenster puffern nur Audio, das keiner braucht, im knappen internen RAM |
| | `LWIP_TCP_OOSEQ_MAX_PBUFS` | 4 | getesteter Wert, IDF-Vorgabe mit PSRAM wäre unbegrenzt |
| | `LWIP_MAX_SOCKETS` | 24 | 10 Snapcast-Clients, JSON-RPC, Webserver, Mesh, Durchsagen |
| | `LWIP_TCPIP_TASK_AFFINITY_CPU0` | y | lwIP-Task (Prio 18) verdrängte sonst den Audio-Task auf Kern 1 |
| HTTP | `HTTPD_MAX_REQ_HDR_LEN` | 1024 | Header einer Android-WebView passten nicht in 512 (Fehler 431) |
| System | `ESP_SYSTEM_EVENT_TASK_STACK_SIZE` | 4096 | `sys_evt` lief auf Clients nach dem Mesh-IP-Ereignis über (Boot-Schleife) |
| | `FREERTOS_TIMER_TASK_STACK_DEPTH` | 4096 | Stack des FreeRTOS-Timer-Tasks (IDF: 2048); seit dem ersten Commit, Grund nicht dokumentiert |
| | `FREERTOS_HZ` | 1000 | 1-ms-Tick (IDF: 100); seit dem ersten Commit, Grund nicht dokumentiert |
| Diagnose | `FREERTOS_USE_TRACE_FACILITY`, `…_RUN_TIME_STATS` u. a. | y | CPU-Last je Task im Log (`cpu_stats.c`) |
| | `LOG_DEFAULT_LEVEL_INFO` | y | |
| Mesh | `SNAPSERVER_ENABLE_MESH_LITE`, `MESH_LITE_ENABLE` | y | Mesh einschalten (im Projekt-Kconfig standardmäßig aus) |

**Projektoptionen** (`idf.py menuconfig` → *Snapserver Mesh Project Configuration*, `main/Kconfig.projbuild`).
Die Schalter wirken beim Bauen; alle übrigen Werte sind nur Vorgaben für den ersten Start und den Factory
Reset, danach gilt, was auf der Web-Seite eingestellt und im NVS gespeichert ist.

| Option | Vorgabe | Bedeutung |
|---|---|---|
| `SNAPSERVER_STATUS_LED_ENABLE` | y | WS2812-Status-LED; `n` nimmt den LED-Code ganz heraus |
| `SNAPSERVER_STATUS_LED_GPIO` | 48 | LED-Pin (0 = keine LED) |
| `SNAPSERVER_POTS_ENABLE` | y | Potis für Lautstärke und Delay; `n` nimmt den Code heraus |
| `MESH_SOFTAP_SSID_PREFIX` | `SnapMesh` | Mesh-SSID |
| `MESH_SOFTAP_PASSWORD` | `criticalmass` | Mesh-Passwort |
| `MESH_CHANNEL` | 6 | WLAN-Kanal (1–13) |
| `SNAPSERVER_OPUS_BITRATE` | 96000 | Opus-Bitrate (16000–192000) |
| `SNAPSERVER_OPUS_COMPLEXITY` | 5 | Opus-Complexity (0–10) |
| `SNAPSERVER_CROSSOVER_HZ` | 120 | Trennfrequenz der Weiche (40–500 Hz) |

**Partitionstabelle** (`partitions.csv`, passt in 4 MB Flash):

| Name | Typ | Offset | Größe | Inhalt |
|---|---|---|---|---|
| `nvs` | data/nvs | `0x9000` | 24 KB | Konfiguration, Pins, Potis, Client-Werte, DHCP-Bereich |
| `phy_init` | data/phy | `0xF000` | 4 KB | WLAN-Kalibrierung |
| `ota_0` | app | `0x10000` | 1,875 MB | Firmware (belegt ~1,3 MB, 30 % frei) |
| `ota_1` | app | `0x1F0000` | 1,875 MB | zweiter App-Bereich für spätere OTA-Updates |
| `otadata` | data/ota | `0x3D0000` | 8 KB | welcher App-Bereich startet; beim Flashen leer → `ota_0` |

`nvs` und `phy_init` liegen an denselben Adressen wie in der früheren 16-MB-Tabelle. Ein Board mit älterer
Firmware lässt sich deshalb ohne „Erase device“ aktualisieren und behält seine Einstellungen. Die OTA-Bereiche
sind vorbereitet, Updates über die Web-Seite gibt es noch nicht (siehe TODO.md). Keine Coredump-Partition.

### Android-App bauen

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

## Haftungsausschluss

Dies ist ein privates Bastelprojekt. Firmware, App und Anleitungen werden kostenlos und **ohne jede Gewähr**
bereitgestellt; es gilt die [MIT-Lizenz](LICENSE). Die Nutzung erfolgt **auf eigene Verantwortung**. Soweit
gesetzlich zulässig, übernehme ich keine Haftung für Schäden, die durch Nachbau, Installation oder Betrieb
entstehen, etwa an Boards, Lautsprechern, Verstärkern oder anderen Geräten, für Datenverlust oder für
Folgeschäden.

Besonders zu beachten:

* **Stromversorgung und Verstärker:** Netzteile, Verstärker und Verkabelung fachgerecht aufbauen. Arbeiten an
  Netzspannung (230 V) nur von Fachleuten.
* **Lautstärke:** Die Durchsage- und Musiklautstärke kann hoch sein. Pegel vorsichtig einstellen, um Gehör und
  Lautsprecher zu schützen.
* **Flashen:** Beim Aufspielen der Firmware kann ein Board unbrauchbar werden, etwa bei einer Unterbrechung.
  Mit „Erase device“ gehen gespeicherte Einstellungen verloren.
* **Funk:** Das System betreibt ein eigenes WLAN. Es ist für den privaten Einsatz gedacht und ersetzt keine
  Alarm-, Notruf- oder Sicherheitsanlage.

## Abhängigkeiten und Lizenz

ESP-IDF, ESP-Mesh-Lite, ESP-IoT-Bridge, ESP-Modem, ESP-mDNS, CMake Utilities (Apache 2.0); esp-opus (MIT).
Drittkomponenten unterliegen ihren eigenen Lizenzen.

Dieses Projekt: MIT License.

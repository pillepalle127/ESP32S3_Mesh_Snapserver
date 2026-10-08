<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/logo/snapmesh-logo-dark.svg">
  <img src="docs/logo/snapmesh-logo.svg" alt="SnapMesh" width="300">
</picture>

# ESP32-S3 Mesh Snapserver

**Stand:** 2026-10-08 · **English:** [README.en.md](README.en.md)

Mehrere Lautsprecher spielen synchron dieselbe Musik, ohne Router, PC oder Kabel dazwischen. Die ESP32-S3 bauen sich
ihr eigenes Funknetz (ESP-Mesh-Lite) und reichen das Signal von Gerät zu Gerät weiter.

Damit könnt ihr:

* Musik von Bluetooth (A2DP), einem I2S-Gerät oder dem PC (USB) einspeisen und überall hören,
* Lautstärke und Verzögerung für jeden Lautsprecher einzeln einstellen,
* Durchsagen vom Handy machen,
* einen Subwoofer über die eingebaute Weiche anschließen,
* vorhandene Snapcast-Clients (PC, Android, iOS) und Snapcast-Apps mitnutzen.

Im Kern stecken zwei Funktionen, die sich eigentlich widersprechen: **Musik** soll ohne Aussetzer laufen und bekommt
dafür einige Sekunden Puffer. **Durchsagen** sollen sofort zu hören sein (keine Latenz). Ich habe es
auf ~90 ms runtergeschafft.

Alle Geräte haben dieselbe Firmware. Eins ist der **Server**, alle anderen sind **Clients**.

---

## Installation

Firmware und App liegen unter [Releases](https://github.com/pillepalle127/ESP32S3_Mesh_Snapserver/releases).
Geflasht wird aus dem Browser.

**Benötigt:** je Standort ein ESP32-S3 mit mindestens 4 MB Flash und **Octal-PSRAM** (z. B. N16R8), ein Datenkabel,
Chrome oder Edge. Für die App ein Android-Handy.

1. **[Flash-Seite](https://pillepalle127.github.io/ESP32S3_Mesh_Snapserver/)** öffnen, Board an der Buchse „USB“
   anschließen, **Installieren**, „USB JTAG/serial debug unit“ wählen.
   **Bei Updates „Erase device“ nicht anhaken**, sonst sind die Einstellungen weg.
2. Mit dem WLAN **`ESP32_provisioning_…`** verbinden, **http://192.168.5.1/** öffnen, Rolle, Mesh-Name und Passwort
   eintragen, **Save**. Erst den Server, dann die Clients.
3. **`SnapAnnounce-….apk`** aus dem Release aufs Handy laden und installieren.

Taucht das Board nicht auf: anderes Kabel, oder BOOT gedrückt halten und einstecken (Download-Modus). Ohne Browser
mit [esptool](https://github.com/espressif/esptool/releases):
```bash
esptool --chip esp32s3 --before usb_reset write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 snapmesh-app.bin 0x3d0000 ota_data_initial.bin
```

---

## Hardware

Bei mir laufen ein **Komplettsystem** im Verstärker (PCM5102A, TinySine AudioB als Bluetooth-Eingang über einen
TXB0104) und der **[SnapStreamer](#bauvorschlag-snapstreamer)**. Geeignet ist jeder ESP32-S3 mit mindestens 4 MB
Flash, Octal-PSRAM und USB-Serial/JTAG, z. B. ein YD-ESP32-S3 N16R8
([Schaltplan](https://github.com/vcc-gnd/YD-ESP32-S3/blob/main/5-public-YD-ESP32-S3-Hardware%20info/YD-ESP32-S3-SCH-V1.4.pdf)).

| GPIO | Funktion |
|---|---|
| 12 | BCLK (PCM5102A BCK, TinySine BCLK) |
| 14 | LRCLK (PCM5102A LCK, TinySine LRCLK) |
| 11 | DIN ← TinySine DOUT |
| 13 | DOUT → PCM5102A DIN |
| 10 | Lautstärke-Poti (optional) |
| – | Delay-Poti, Ein/Aus-Taster (optional) |
| 48 | WS2812-Status-LED |

Die Belegung lässt sich ändern, siehe [Pins](#pins).

---

## Bauvorschlag: SnapStreamer

Ihr habt einen Verstärker oder Aktivlautsprecher und wollt ihn ins Mesh holen? Der SnapStreamer ist meine Idee
dazu: ESP32-S3, DAC und Akku so dicht wie möglich in einem kleinen Gehäuse.

<table>
  <tr>
    <td valign="top"><img src="docs/IMG_1684_copy.jpg" alt="Platine von oben: ESP32-S3-Board mit dem PCM5102A darunter" width="220"></td>
    <td valign="top"><img src="docs/IMG_1695.jpeg" alt="Offenes Gehäuse mit Platine, USB und Klinke angesteckt" width="220"></td>
    <td valign="top"><img src="docs/IMG_1697.jpeg" alt="Geschlossenes Gehäuse, die Status-LED leuchtet durch den Deckel" width="220"></td>
  </tr>
</table>

Der PCM5102A sitzt über kurze Stiftleisten unter dem ESP32-S3-Board (GND und VIN gekreuzt). Ein TP4056 lädt über
die USB-Buchse des ESP, der Akku hängt an VIN, ein Poti mit Schalter schaltet ein und regelt die Lautstärke.
Gehäuse zum Drucken (FreeCAD, 3MF) in [`mechanics/housing/`](mechanics/housing/).

| Teil | Typ |
|---|---|
| ESP32-S3-Board | ≥ 4 MB Flash, Octal-PSRAM, z. B. YD-ESP32-S3 N16R8 |
| DAC | PCM5102A mit Klinke |
| Lademodul | TP4056-Modul mit Schutz (DW01), Last an OUT+/OUT− |
| Akku | 18650 Li-Ion |
| Poti | 10 kΩ linear mit Schalter |

> [!WARNING]
> Li-Ionen-Akkus können brennen. Nur Zellen und Lademodule mit Schutzschaltung, offene Kontakte isolieren, nicht
> unbeaufsichtigt laden.

---

## Signalweg

```text
I2S in / USB (Stereo) ─► L+R → Mono ─┬─► Opus ─► Snapcast TCP 1704 ─► Clients
                                     │
                                     └─► Delay-Line ─► DSP ─► I2S out
```

| Stufe, in Reihenfolge | Weiche aus | Weiche an | Durchsage |
|---|---|---|---|
| Puffer, Delay, Delay-Poti | X | X | - |
| Mono (L+R)/2 | X | X | - |
| Subsonic-Hochpass | - | X | X |
| Gain Sub / Breitband | - | X | X |
| Sub-Phase 180° | - | X | X |
| Kompressor | X | X | - |
| Poti, Lautstärke | X | X | X |
| Begrenzer −0,5 dBFS, 1,3 ms Vorausschau | X | X | X |

**X** verfügbar, **-** nicht verfügbar. Durchsagen umgehen alles, was verzögert.

Die Clients gleichen ihre Uhr laufend mit dem Server ab. Bei mir liegen die Lautsprecher unter 2–3 ms beieinander.

---

## Web-UI

<img src="docs/webui-server.png" alt="Einstellungsseite des Servers mit Geräteliste" width="300" align="right">

Jedes Gerät hat eine Einstellungsseite unter seiner IP oder `snapserver-<MAC>.local` bzw. `snapclient-<MAC>.local`.
Die Werte bleiben bei Updates erhalten. Von oben nach unten, wie im Bild die Seite des Servers:

**Status:** Mesh aktiv oder nicht, verbundene Clients (eigene und fremde Snapcast-Clients), Firmware-Stand,
Fehlstarts in Folge und Laufzeit.

**Geräte (nur Server):** jeder Lautsprecher mit Namen, Tiefe im Mesh (Hops), Lautstärke, Stumm und Delay. Den Namen
könnt ihr hier ändern, ± kehrt das Vorzeichen des Delays um. Der Server merkt sich die Werte je Gerät, auch wenn sie
aus einer Snapcast-App kommen. **Einstellungen** öffnet die Seite eines Clients, egal wie tief er im Mesh hängt.

**Rolle:** Server oder Client. Am Server der **Puffer** (Vorgabe 3000 ms, gilt für alle Geräte) und die
**USB-Soundkarte** (siehe [unten](#usb-soundkarte)). Am Client stattdessen die **Wiedergabe**: Quelle (automatisch,
Netzwerk oder lokaler I2S-Eingang), Schwelle für den lokalen Eingang, Lautstärke und Stumm. Solange ein Server
verbunden ist, stellt er die Lautstärke ein.

**Mesh / WLAN:** Mesh an oder aus, Name, Passwort, Kanal und die maximale Tiefe des Mesh. Auf allen Geräten gleich.

**DSP / Weiche:** Weiche an oder aus, Trennfrequenz (40–500 Hz), Verstärkung für Sub und Breitband (−24 bis
+18 dB), Subsonic-Hochpass im Sub (15–60 Hz, Richtwert 0,75 × F3), Sub-Phase 180° und der Ausgang, auf dem der Sub
liegt.

**Kompressor:** Schwelle, Verhältnis und Aufholen, auf beiden Zweigen vor dem Poti. Zum Einstellen die Zweige mit den
Gains abgleichen, Poti und Quelle auf Maximum. Kompressor an, Verhältnis 3, Schwelle senken, bis laute Stellen um
3–6 dB sinken. Dann Aufholen erhöhen, bis der Begrenzer nur selten eingreift.

**Pins** und **Opus** sind eingeklappt. Pins siehe [unten](#pins), Opus sind Bitrate und Rechenaufwand des Encoders
am Server.

Weiche und Kompressor wirken beim Ändern, alles andere mit **Speichern**. Rolle, Mesh, Puffer, USB-Soundkarte und
Pins starten das Gerät neu. **Werkseinstellungen** setzt nach einer Rückfrage alles zurück. **Neustart zum Flashen**
erscheint nur bei eingeschalteter USB-Soundkarte.

### USB-Soundkarte

Der Server kann an der Buchse „USB“ eine Soundkarte für den PC sein, ohne Treiber. Getestet habe ich unter Linux und
Windows 11. Spielt der PC, ersetzt er den I2S-Eingang. Die Lautstärke am PC gilt für das ganze Mesh, also dort auf 100 %
lassen. Log und Flashen laufen dann über die Buchse „COM“ oder über **Neustart zum Flashen** auf der Seite des
Servers.

---

## Pins

Die Pins lassen sich in der Web-UI umbelegen, ohne neu zu flashen. **Vorlage für die Verdrahtung** setzt gängige
Belegungen auf einmal. Die Firmware lehnt Doppelbelegungen und ungeeignete Pins ab (Flash, PSRAM, USB,
Strapping-Pins). Startet das Gerät mit einer neuen Belegung dreimal nicht, fällt es auf die Standardbelegung zurück.

* **I2S:** BCLK, LRCLK, DIN (vom Bluetooth-Modul oder ADC) und DOUT (zum DAC).
* **I2S-Takt:** Normal erzeugt der ESP BCLK und LRCLK. Gibt ein anderes Gerät den Takt vor, z. B. ein DSP wie der
  ADAU1701, auf **Extern** stellen. Ohne diesen Takt bleibt es still.
* **Status-LED:** Datenleitung der WS2812.
* **Ein/Aus-Taster:** von einem GPIO (1–21) gegen GND, ohne weitere Bauteile. 2 s halten = aus (Deep Sleep),
  1 s halten = an. Nur der ESP schläft, DAC und Verstärker am Akku ziehen weiter Strom.
* **Potis:** 10 kΩ für Lautstärke und Delay, Schleifer an einem ADC1-Pin (GPIO 1–10). Drehrichtung umkehrbar,
  Bereich des Delay-Potis einstellbar.

Die Status-LED blitzt beim Start rot, grün und blau und zeigt danach den Zustand:

| Status-LED | Bedeutung |
|---|---|
| blau, blinkt langsam | Einrichtung |
| rot, blinkt schnell | kein Netz |
| orange, blinkt | kein Server |
| Pegel grün bis rot | Wiedergabe |
| Pegel, pulsiert | Durchsage |

<br clear="all">

---

## SnapAnnounce App

<img src="docs/snapannounce-icon.png" alt="SnapAnnounce-Icon" width="72" align="right">

Mit **SnapAnnounce** startet ihr Durchsagen direkt im Mesh. Ohne extra Hardware und ohne Kabel, alles übers Handy.
Leider nur für Android, ich habe keinen Mac und kaufe mir auch keinen.

Die Musik pausiert solange. Durchsagen laufen ohne Puffer und reichen deshalb **nur zwei Mesh-Ebenen** tief.

<table>
  <tr>
    <td valign="top"><img src="docs/snapannounce-screenshot.jpg" alt="SnapAnnounce, Durchsage" width="250"></td>
    <td valign="top"><img src="docs/Screenshot_20260923_215400_SnapAnnounce_copy.jpg" alt="SnapAnnounce, Geräte" width="250"></td>
  </tr>
</table>

---

## Lizenz

MIT, siehe [LICENSE](LICENSE). Die Lizenzen der Fremdkomponenten liegen jedem Release als
`THIRD_PARTY_LICENSES.txt` bei.

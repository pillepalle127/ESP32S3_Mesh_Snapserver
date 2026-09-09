# ESP32-S3 Mini Snapserver

**Stand:** 2026-09-09  


## Überblick

Dieses ESP-IDF-Projekt implementiert einen kompakten Snapcast-kompatiblen Audiostreaming-Server auf einem ESP32-S3 mit PSRAM. Der ESP32-S3 arbeitet gleichzeitig als autonomer ESP-Mesh-Lite-Root, liest ein Stereo-I2S-Signal vom TinySine AudioB I2S V2r0 ein, mischt das Signal zu Mono, codiert es mit Opus und verteilt den Stream an mehrere Snapclients.

Parallel zum Netzwerkstream verarbeitet der ESP32-S3 dasselbe Monosignal lokal mit einer Linkwitz-Riley-Frequenzweiche 4. Ordnung und gibt Tiefpass und Hochpass über die beiden Kanäle eines PCM5102A aus.

## Aktueller Funktionsumfang

- autonomer ESP-Mesh-Lite-Root ohne externen Router
- gemeinsamer I2S-Full-Duplex-Bus für Eingang und lokalen Ausgang
- Stereo-Eingang mit 48 kHz und 16-Bit-Audiodaten in 32-Bit-I2S-Slots
- überlaufsichere Mischung von links und rechts zu Mono
- lokale Linkwitz-Riley-Frequenzweiche 4. Ordnung
- Opus-Codierung mit 48 kHz, Mono und 20-ms-Frames
- Snapcast-Binärprotokoll auf TCP-Port 1704
- JSON-RPC-Control-Schnittstelle auf TCP-Port 1705
- Verwaltung mehrerer gleichzeitig verbundener Clients
- synchronisierte Socket-Schreibzugriffe und kontrollierter Clientabbau
- monotone Audiozeitbasis mit Laufzeitabgleich auf eine plausible Client-Wanduhr
- Status- und Clientdaten für Control-Anwendungen

## Signalfluss

```text
Bluetooth-Quelle
    |
    v
TinySine AudioB I2S V2r0
    |  Stereo, 48 kHz, 16-Bit-Daten in 32-Bit-Slots
    v
ESP32-S3 I2S0 RX
    |
    +--> Mono-Mischung
           |
           +--> Opus-Encoder
           |      |
           |      +--> Snapcast TCP 1704 --> ESP-/PC-/Android-Clients
           |
           +--> LR4-Frequenzweiche
                  |
                  +--> Tiefpass  --> PCM5102A links  --> Subwoofer
                  +--> Hochpass  --> PCM5102A rechts --> Breitband
```

Das Bluetoothsignal wird lokal nicht unverändert als Stereo durchgeschleift. Das im ESP32-S3 gebildete Monosignal wird vor der lokalen Ausgabe durch die Frequenzweiche verarbeitet.

## Verbindliche I2S-Konfiguration

TinySine und PCM5102A verwenden einen gemeinsamen Full-Duplex-I2S-Bus ohne MCLK. BCLK und LRCLK werden gemeinsam genutzt, während RX- und TX-Daten getrennte Leitungen verwenden.

| Signal | GPIO | Verbindung |
|---|---:|---|
| BCLK | 4 | ESP32-S3 zu TinySine und PCM5102A |
| LRCLK / WS | 6 | ESP32-S3 zu TinySine und PCM5102A |
| DIN | 5 | TinySine SD OUT zum ESP32-S3 |
| DOUT | 7 | ESP32-S3 zum PCM5102A DIN |
| MCLK | nicht verwendet | keine Verbindung |

Weitere Parameter:

- I2S-Controller: I2S0
- Betriebsart: gemeinsamer Full-Duplex-Bus
- ESP32-S3: I2S-Master
- TinySine und PCM5102A: I2S-Slaves
- Abtastrate: 48 kHz
- Nutzdatenbreite: 16 Bit
- Slotbreite: 32 Bit
- Hardwarekanäle: Stereo
- Netzwerkstream: Mono

Die aktuelle I2S-Konfiguration ist funktional bestätigt. Der lokale I2S-Audiopfad spielt mit 16-Bit-Audiodaten in 32-Bit-Slots einwandfrei; das DMA-Datenlayout ist daher kein offener Verifikationspunkt.

## Audioverarbeitung

### Mono-Mischung

Für jedes Stereo-Sample wird aus linkem und rechtem Kanal ein Monosample gebildet. Die Berechnung erfolgt mit 32-Bit-Zwischenwerten, bevor das Ergebnis auf 16 Bit zurückgeführt wird.

### Lokale Frequenzweiche

Die lokale Ausgabe verwendet zwei kaskadierte Butterworth-Biquads je Zweig und bildet damit eine Linkwitz-Riley-Weiche 4. Ordnung. Die Trennfrequenz wird über `CONFIG_SNAPSERVER_CROSSOVER_HZ` konfiguriert.

Standardzuordnung:

- PCM5102A links: Tiefpass / Subwoofer
- PCM5102A rechts: Hochpass / Breitband

### Opus-Stream

- Abtastrate: 48 kHz
- Kanäle: 1
- Auflösung des PCM-Eingangs: 16 Bit
- Framegröße: 960 Samples
- Framedauer: 20 ms
- Bitrate: `CONFIG_SNAPSERVER_OPUS_BITRATE`
- Komplexität: `CONFIG_SNAPSERVER_OPUS_COMPLEXITY`

## Netzwerkprotokolle

### Audiostream auf TCP 1704

Der Server implementiert die für den aktuellen Betrieb benötigten Snapcast-Nachrichten:

- Hello
- ServerSettings
- CodecHeader
- WireChunk
- Time
- ClientInfo-Empfang

### Control-Schnittstelle auf TCP 1705

Die JSON-RPC-Schnittstelle liefert Server-, Gruppen-, Stream- und Clientinformationen. Clientidentitäten werden aus der Hello-Nachricht des Audiokanals übernommen.

Aktuell vorgesehene Methoden:

- `Server.GetStatus`
- `Server.GetRPCVersion`
- `Client.GetStatus`
- `Client.SetVolume`
- `Client.SetName`
- `Client.SetLatency`
- `Group.GetStatus`
- `Group.SetStream`
- `Group.SetMute`
- `Group.SetClients`
- `Server.DeleteClient`

Die vollständige semantische Kompatibilität aller schreibenden Methoden ist noch zu verifizieren.

## Zeitbasis

Der ESP32-S3 besitzt in dieser Anwendung keine dauerhaft gültige Wanduhr. Deshalb verwendet der Server `esp_timer_get_time()` als monotone Zeitquelle und ergänzt einen Laufzeit-Offset.

- Uptime-basierte ESP-Clients werden nicht als Wanduhrquelle verwendet.
- Eine plausible Epoch-Zeit kann von einem PC- oder Android-Client übernommen werden.
- Nachrichtenheader und Audiochunks verwenden dieselbe monotone Zeitbasis.
- Audiozeitstempel beziehen sich auf den Beginn des jeweiligen PCM-Frames.

## Projektstruktur

```text
main/
|-- app_main.c
|-- audio_i2s.c
|-- audio_i2s.h
|-- audio_opus.c
|-- audio_opus.h
|-- mesh_root.c
|-- mesh_root.h
|-- snapcontrol.c
|-- snapcontrol.h
|-- snapserver.c
|-- snapserver.h
`-- CMakeLists.txt
```

## Build

Voraussetzung ist eine eingerichtete ESP-IDF-5.4.3-Umgebung.

```powershell
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py flash monitor
```

## Wichtige Konfigurationspunkte

Vor dem Build prüfen:

- `CONFIG_SNAPSERVER_ENABLE_MESH_LITE`
- `CONFIG_MESH_SOFTAP_SSID_PREFIX`
- `CONFIG_MESH_SOFTAP_PASSWORD`
- `CONFIG_MESH_CHANNEL`
- `CONFIG_SNAPSERVER_OPUS_BITRATE`
- `CONFIG_SNAPSERVER_OPUS_COMPLEXITY`
- `CONFIG_SNAPSERVER_CROSSOVER_HZ`

Root und Clients müssen denselben Mesh-Namen, dasselbe Kennwort und denselben Funkkanal verwenden.

## Diagnose

### Erwartete Streamrate

Bei 20-ms-Frames sind ungefähr 50 WireChunks pro Sekunde und Client zu erwarten.

### Control-Status abfragen

Die von der verwendeten Android-App bestätigte Antwortstruktur enthält den vollständigen Status unter `result.server`. Die verbundenen Clients stehen unter `result.server.groups[0].clients[]` und werden in der Android-App korrekt angezeigt.

### Relevante Logs

- `Client connected`
- `Hello from ...`
- `Client handshake complete`
- `chunks/s=50`
- `send_errors=0`
- `RPC request: Server.GetStatus`
- `Server.OnUpdate sent after client-set change`

## Bekannte offene Punkte

- Benachrichtigungen der Control-Schnittstelle bei Laufzeitänderungen weiter prüfen
- tatsächliche Wirkung der schreibenden JSON-RPC-Methoden vollständig implementieren oder als nicht unterstützt kennzeichnen
- Opus-Encoder bei Fehlern während der Konfiguration vollständig freigeben
- Speicher-, Stack- und CPU-Auslastung mit mehreren Clients über längere Zeit messen
- Audioaussetzer und Rebuffer-Ereignisse unter Funklast weiter beobachten

## Testreihenfolge

1. Server starten und I2S-, Opus-, Port-1704- und Port-1705-Initialisierung prüfen.
2. Einen ESP-Snapclient direkt am Root verbinden.
3. Einen PC-Snapclient verbinden und Wanduhrabgleich prüfen.
4. `Server.GetStatus` abfragen und Clientliste kontrollieren.
5. Android-Control-App verbinden; die Clientanzeige ist bestätigt, Laufzeitupdates separat prüfen.
6. Serverneustart und Client-Reconnect testen.
7. Mehrere Clients sowie Mesh-Hops unter Dauerlast testen.
8. Lokale Tiefpass-/Hochpass-Ausgabe und Netzwerkstream parallel prüfen.


## Project status

Experimental implementation. Not an official Snapcast server and not
affiliated with the Snapcast project.

## Security notice

Change the default Mesh SSID and password before deployment.
Do not commit a local sdkconfig containing credentials.

## Compatibility

Tested with:

- ESP-IDF 5.4.3
- ESP32-S3 with PSRAM
- ESP-Mesh-Lite 1.0.2
- ESP-IoT-Bridge 1.0.1
- Snapclient 0.35.0 on Windows
- ESP32 Snapclient
- Android Snapcast control/client application
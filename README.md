# ESP32-S3 Mini Snapserver

**Stand:** 2026-09-09

## Projektziel

Dieses Projekt implementiert einen eigenständigen **Snapcast-kompatiblen Audio-Server auf einem ESP32-S3**.

Der ESP32-S3 übernimmt dabei mehrere Aufgaben:

* Audioeingang über I2S
* Stereo-zu-Mono-Mischung
* Opus-Encoding
* Snapcast-kompatibler Audiostream
* TCP-Verbindung für Audio auf Port **1704**
* JSON-RPC-Steuerung auf Port **1705**
* eigenständiger ESP-Mesh-Lite-Root
* Verteilung des Audiosignals an Snapclients
* lokale digitale Frequenzweiche für die angeschlossene Audiohardware

Das Ziel ist ein vollständig eigenständiger Audio-Server ohne Raspberry Pi oder PC im laufenden Betrieb.

---

## Aktueller Funktionsumfang

* ESP32-S3 als Snapserver
* PSRAM-Unterstützung
* ESP-Mesh-Lite als eigenes Mesh-Netzwerk
* ESP32-S3 arbeitet als Mesh-Root
* I2S-Audioeingang
* Stereo-Eingang wird vor der weiteren Verarbeitung zu Mono gemischt
* Opus-Encoding
* Snapcast-Audiostream über TCP Port 1704
* Snapcast JSON-RPC über TCP Port 1705
* monotone Audiozeitbasis mit Abgleich mit der absoluten Client-Zeit
* Audioübertragung an normale Snapclients
* lokale digitale Frequenzweiche
* Ausgabe der getrennten Frequenzbereiche über I2S
* vorgesehen für die Kombination mit externen I2S-DACs und Verstärkern

---

## Hardware

### ESP32-S3

Der Server basiert auf einem ESP32-S3 mit PSRAM.

Die zusätzliche Speichergröße wird unter anderem für Audioverarbeitung, Netzwerk- und Opus-Puffer verwendet.

### Audioeingang

Als Audioquelle wird ein I2S-Audioeingang verwendet.

Aktuell vorgesehen:

**TinySine AudioB I2S V2r0**

Das Eingangssignal wird zunächst als Stereo verarbeitet und anschließend zu einem Monosignal gemischt.

### Audioausgang

Für die lokale Audioausgabe ist ein I2S-DAC vorgesehen:

**PCM5102A**

Das Ausgangssignal wird nach der digitalen Frequenzweiche entsprechend der jeweiligen Frequenzbereiche ausgegeben.

---

## Audioverarbeitung

Der grundsätzliche Signalweg ist:

```text
I2S Stereo Input
       │
       ▼
  L + R → Mono
       │
       ├──────────────► Opus Encoder
       │                     │
       │                     ▼
       │              Snapcast Stream
       │
       ▼
 Digitale Frequenzweiche
       │
       ├────────► Low Band
       │
       └────────► High Band
                     │
                     ▼
                 I2S DAC
```

Die Stereo-Kanäle werden **vor der Frequenzweiche** zu einem gemeinsamen Monosignal gemischt.

Damit wird ausdrücklich keine getrennte Frequenzweiche für den linken und rechten Kanal betrieben.

---

## Digitale Frequenzweiche

Die lokale DSP-Verarbeitung erfolgt direkt auf dem ESP32-S3.

Vorgesehen ist eine **Linkwitz-Riley-Frequenzweiche 4. Ordnung (LR4)**.

Die Frequenzweiche arbeitet auf dem zuvor aus L und R gebildeten Monosignal.

Beispiel:

```text
             Mono
               │
               ▼
        ┌──────────────┐
        │     LR4      │
        │ Frequenzweiche│
        └──────────────┘
             │     │
             │     │
             ▼     ▼
            LOW   HIGH
             │     │
             ▼     ▼
           Output Output
```

Die konkrete Trennfrequenz wird über die Projektkonfiguration festgelegt.

Die DSP-Verarbeitung benötigt für diesen Signalweg kein externes DSP-System und ist auf dem ESP32-S3 ohne zwingende Verwendung von PSRAM für die eigentliche Filterberechnung vorgesehen.

---

## Snapcast-Kompatibilität

Der ESP32-S3 stellt die für Snapcast benötigten Netzwerkdienste bereit.

### Audio

**TCP Port 1704**

Über diesen Port wird der Audiostream an die Snapclients übertragen.

### Steuerung

**TCP Port 1705**

Über diesen Port erfolgt die JSON-RPC-Kommunikation.

Damit kann sich beispielsweise ein normaler PC-Snapclient mit dem ESP32-S3 verbinden.

---

## Zeitbasis

Der ESP32-S3 besitzt in dieser Anwendung keine dauerhaft gültige Echtzeituhr (RTC) mit verlässlicher absoluter Zeit.

Deshalb verwendet der Server `esp_timer_get_time()` als monotone Zeitquelle und ergänzt einen Laufzeit-Offset.

* Uptime-basierte ESP-Clients werden nicht als Quelle für die absolute Zeit verwendet.
* Eine plausible Epoch-Zeit kann von einem PC- oder Android-Client übernommen werden.
* Nachrichtenheader und Audiochunks verwenden dieselbe monotone Zeitbasis.
* Audiozeitstempel beziehen sich auf den Beginn des jeweiligen PCM-Frames.

Ziel ist ein sauberer **Abgleich mit der absoluten Client-Zeit**, ohne die Audiozeitbasis selbst von einer möglicherweise unstabilen Echtzeituhr abhängig zu machen.

---

## Netzwerk

Der ESP32-S3 arbeitet als eigenständiger **ESP-Mesh-Lite-Root**.

Die Mesh-Struktur ermöglicht die Verbindung weiterer ESP-Geräte, ohne dass für die reine Audioverteilung zwingend ein separater Raspberry-Pi-Snapserver erforderlich ist.

Der Snapserver selbst stellt seine Dienste über das lokale Netzwerk beziehungsweise Mesh-Netzwerk bereit.

---

## Projektstruktur

Die wichtigsten Komponenten befinden sich unter anderem in:

```text
components/
├── audio_i2s/
│   ├── CMakeLists.txt
│   ├── audio_i2s.c
│   └── include/
│
├── ...
│
main/
├── ...
│
CMakeLists.txt
sdkconfig
README.md
```

Die genaue Struktur kann sich während der Entwicklung noch ändern.

---

## Abhängigkeiten

Das Projekt basiert auf **ESP-IDF**.

Verwendete beziehungsweise vorgesehene Komponenten:

* ESP-IDF
* ESP-Mesh-Lite
* esp-opus
* ESP-IoT-Bridge
* CMake Utilities
* ESP-Modem

Die konkreten Versionen sind abhängig vom jeweiligen Entwicklungsstand und werden über die ESP-IDF-Komponentenverwaltung festgelegt.

---

## Build

ESP-IDF muss zunächst eingerichtet sein.

Danach im Projektverzeichnis:

```powershell
idf.py set-target esp32s3
idf.py build
```

Flashen:

```powershell
idf.py flash
```

Serielle Ausgabe:

```powershell
idf.py monitor
```

Build und Monitor können auch kombiniert werden:

```powershell
idf.py flash monitor
```

---

## Konfiguration

Die ESP-IDF-Konfiguration erfolgt über:

```powershell
idf.py menuconfig
```

Die daraus erzeugte `sdkconfig` ist projektspezifisch.

Passwörter, Zugangsdaten und andere private Netzwerkdaten gehören **nicht in das Git-Repository**.

---

## Sicherheit / Repository

Das Repository darf keine privaten Zugangsdaten enthalten.

Insbesondere nicht:

* WLAN-Passwörter
* API-Keys
* Tokens
* private Schlüssel
* Zertifikats-Private-Keys
* persönliche Zugangsdaten
* reale private Netzwerkdetails, sofern diese nicht für die Dokumentation erforderlich sind
* erzeugte NVS-Datenbanken mit gespeicherten Credentials

Auch die **Git-Historie** muss bei der Veröffentlichung des Projekts frei von solchen Daten sein.

---

## Testreihenfolge

1. ESP32-S3 flashen und starten.
2. Mesh-Lite-Netzwerk initialisieren.
3. I2S-Audioeingang prüfen.
4. Stereo-zu-Mono-Mischung prüfen.
5. Opus-Encoding prüfen.
6. Snapcast-Port 1704 prüfen.
7. JSON-RPC-Port 1705 prüfen.
8. Einen PC-Snapclient verbinden und den **Abgleich mit der absoluten Client-Zeit** prüfen.
9. Audioübertragung und Synchronität prüfen.
10. Lokale Frequenzweiche und die beiden Ausgangssignale prüfen.

---

## Entwicklungsstatus

Das Projekt befindet sich in aktiver Entwicklung.

Der Schwerpunkt liegt derzeit auf:

* stabiler Audioübertragung
* korrekter Snapcast-Synchronisation
* stabiler Zeitbasis
* ESP-Mesh-Lite-Integration
* DSP-Frequenzweiche
* zuverlässiger I2S-Verarbeitung
* möglichst geringer zusätzlicher Latenz

Änderungen an Audioformat, Buffergrößen, Filterparametern und Netzwerkverhalten sind während der Entwicklung möglich.

---

## Lizenz

MIT License
	
	
	


## Dependencies

This project uses components from the ESP-IDF ecosystem,
including:

- ESP-IDF
- ESP-Mesh-Lite
- esp-opus
- ESP-IoT-Bridge
- CMake Utilities
- ESP-Modem

Please refer to the respective component repositories
for their individual license terms.
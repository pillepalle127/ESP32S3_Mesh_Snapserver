# ESP32-S3 Mini Snapserver

**Stand:** 2026-09-20

## Projektziel

Dieses Projekt implementiert ein eigenständiges **Snapcast-kompatibles Audiosystem auf ESP32-S3**,
das in einem eigenen ESP-Mesh-Lite-Netzwerk sowohl als **Server** als auch als **Client**
laufen kann — dieselbe Firmware, die Rolle wird per Web-Konfiguration gewählt.

Als **Server** übernimmt der ESP32-S3 mehrere Aufgaben:

* Audioeingang über I2S
* Stereo-zu-Mono-Mischung
* Opus-Encoding
* Snapcast-kompatibler Audiostream
* TCP-Verbindung für Audio auf Port **1704**
* JSON-RPC-Steuerung auf Port **1705**
* Sprachdurchsagen vom Handy auf Port **1706**, siehe [Sprachdurchsagen](#sprachdurchsagen)
* eigenständiger ESP-Mesh-Lite-Root
* Verteilung des Audiosignals an Snapclients
* lokale digitale Frequenzweiche für die angeschlossene Audiohardware
* Potis für Lautstärke und Delay am eigenen Lautsprecher, siehe [Potis für Lautstärke und Delay](#potis-für-lautstärke-und-delay)
* Web-Konfigurationsoberfläche für Mesh-, DSP- und Opus-Einstellungen
* Provisioning-AP-Fallback, falls das Gerät sonst nicht erreichbar wäre

Als **Client** (siehe [Client-Rolle](#client-rolle)) tritt ein weiterer ESP32-S3 demselben
Mesh als Relay bei, empfängt den Snapcast-Stream des Servers und gibt ihn über dieselbe
DSP-/I2S-Kette an einem eigenen Lautsprecher wieder — wahlweise mit lokalem I2S-Eingang
als Alternativquelle.

Das Ziel ist ein vollständig eigenständiges Mehr-Lautsprecher-Audiosystem ohne Raspberry Pi
oder PC im laufenden Betrieb.

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
* Web-Konfigurationsseite (HTTP, Port 80) für Mesh-Zugangsdaten, Mesh-Hop-Tiefe,
  DSP-Frequenzweiche (Enable, Trennfrequenz, Kanal-Gains, Kanalzuordnung) und
  Opus-Bitrate/Complexity, persistent in NVS gespeichert
* mDNS-Erreichbarkeit unter einem pro Gerät eindeutigen Namen
  (`snapserver-<MAC>.local` bzw. `snapclient-<MAC>.local`)
* Factory-Reset über die Web-Oberfläche
* Offener Provisioning-Access-Point (`ESP32_provisioning_<MAC>`) als Fallback bei
  Erstinbetriebnahme, nach Factory-Reset, bei deaktiviertem Mesh oder nach
  mehreren erfolglosen Mesh-Boots
* Client-Rolle (Snapcast-Empfang, Mesh-Relay) über dieselbe Web-Konfiguration
  wählbar, siehe [Client-Rolle](#client-rolle)
* Sprachdurchsagen von einem Android-Handy über einen eigenen, ungepufferten
  Kanal, mit automatischer Stummschaltung der Musik, siehe
  [Sprachdurchsagen](#sprachdurchsagen)

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

Umgesetzt ist eine **Linkwitz-Riley-Frequenzweiche 4. Ordnung (LR4)**, je
Zweig als zwei kaskadierte Biquads in `audio_i2s.c`. Trennfrequenz, Gains und
Kanalzuordnung kommen aus der Konfiguration und lassen sich im laufenden
Betrieb ändern.

Die Frequenzweiche arbeitet auf dem zuvor aus L und R gebildeten Monosignal.

Beispiel:

```text
             Mono
               │
               ▼
        ┌────────────-──┐
        │     LR4       │
        │ Frequenzweiche│
        └──────────-────┘
             │     │
             │     │
             ▼     ▼
            LOW   HIGH
             │     │
             ▼     ▼
           Output Output
```

Trennfrequenz, Enable/Bypass, Kanal-Gains und Kanalzuordnung sind zur Laufzeit über
die Web-Konfigurationsseite änderbar (siehe unten) und wirken sofort, ohne Neustart.
Die Projektkonfiguration (Kconfig) legt dafür nur noch die Standardwerte für
Erstinbetriebnahme und Factory-Reset fest.

Die DSP-Verarbeitung benötigt für diesen Signalweg kein externes DSP-System und ist auf dem ESP32-S3 ohne zwingende Verwendung von PSRAM für die eigentliche Filterberechnung vorgesehen.

---

## Web-Konfiguration

Das Gerät stellt unter Port **80** eine Konfigurationsseite bereit, erreichbar über
seine IP-Adresse oder per mDNS. Der mDNS-Name enthält Rolle und die letzten
drei MAC-Bytes (`http://snapserver-E3B689.local/`,
`http://snapclient-E2BDFD.local/`) — ein gemeinsamer Name für alle Geräte war
nicht brauchbar, weil zwischengespeicherte Namensauflösungen dann auf dem
falschen Gerät landen. Dieselbe Seite läuft auf jedem Gerät, unabhängig von
der Rolle — Screenshots beider Rollen in [Client-Rolle](#client-rolle).

<img src="docs/webconfig-server-screenshot.png" width="360">

Konfigurierbar:

* **Rolle:** Server oder Client (siehe [Client-Rolle](#client-rolle)). Änderung
  löst einen Neustart aus.
* **Mesh / Wi-Fi:** Enable, SSID, Passwort, Kanal, maximale Hop-Tiefe
  (`esp_mesh_lite`-Level). Änderungen an diesen Werten lösen einen Neustart aus, um
  sie zu übernehmen.
* **DSP / Frequenzweiche:** Enable, Trennfrequenz, Gain pro Kanal, Kanalzuordnung
  (Sub/Wideband). Wirkt sofort, ohne Neustart. Gilt für beide Rollen — auch der
  Client führt sein wiedergegebenes Signal durch dieselbe Weiche.
* **Opus:** Bitrate, Complexity. Wirkt sofort, ohne Neustart. Nur in der
  Server-Rolle relevant (Encoder-Einstellungen).
* **Client-Wiedergabe:** Quellenwahl, Pegelschwelle des lokalen Eingangs,
  Delay-Trim und feste Server-Adresse wirken sofort. Die Puffergröße
  (`bufferMs`) löst dagegen einen Neustart aus: aus ihr werden beim Start
  mehrsekündige Puffer dimensioniert — der Ringpuffer des Clients und die
  Verzögerungsleitung der lokalen Ausgabe des Servers.
* **Potentiometers:** Pin für das Lautstärke- und das Delay-Poti (jeweils
  auch „none") und der Bereich des Delay-Potis. Angeboten werden nur freie
  Pins. Ein Pinwechsel löst einen Neustart aus, der Bereich wirkt sofort.
  Siehe [Potis für Lautstärke und Delay](#potis-für-lautstärke-und-delay).

Alle Werte werden persistent im NVS gespeichert und überleben Neustarts und
Firmware-Updates (solange sich das Konfigurationsschema nicht ändert).

Ein **Factory-Reset**-Button setzt die Konfiguration auf die Kconfig-Standardwerte
zurück und startet das Gerät neu.

### Provisioning-Access-Point

Ist das Gerät über sein konfiguriertes Mesh nicht erreichbar, öffnet es automatisch
einen offenen, unverschlüsselten Access Point (`ESP32_provisioning_<MAC>`), über
den dieselbe Konfigurationsseite erreichbar ist. Auslöser sind:

* Erstinbetriebnahme (noch keine gespeicherte Konfiguration)
* Factory-Reset
* Mesh in der Konfiguration deaktiviert
* 7 aufeinanderfolgende Boots ohne dass sich eine Station am eigenen Mesh-AP anmeldet

Der Provisioning-AP bleibt 3 Minuten aktiv; läuft dieses Fenster ab, ohne dass
gespeichert wurde, schaltet sich der Funk komplett ab — ein erneutes Fenster öffnet
sich erst nach einem Stromzyklus. Ein Speichern innerhalb des Fensters startet das
Gerät neu und übergibt an die normale Mesh-Entscheidung.

---

## Client-Rolle

Dieselbe Firmware kann statt als Server auch als Snapcast-**Client** laufen —
z. B. auf einem zweiten ESP32-S3 an einem anderen Lautsprecher im selben
Mesh. Umschaltbar über die Web-Konfiguration (Feld „Rolle", siehe
[Web-Konfiguration](#web-konfiguration)), Übernahme per Neustart.

<img src="docs/webconfig-client-screenshot.png" width="360">

In der Client-Rolle:

* Das Gerät tritt dem Mesh als **Non-Root-Relay** bei, niemals als Leaf —
  ein Leaf könnte in einer langgestreckten, mehrere Hops tiefen Topologie
  keine weiteren Kinder mehr annehmen und die Kette damit vorzeitig beenden.
* Der Snapserver wird automatisch über `esp_mesh_lite_get_root_ip()`
  gefunden (funktioniert auch über mehrere Hops und nach
  Mesh-Umstrukturierungen hinweg, anders als das lokale DHCP-Gateway oder
  mDNS, die beide nur bis zur ersten Ebene reichen). Alternativ ist in der
  Web-Konfiguration eine feste Server-Adresse eintragbar.
* Empfangenes Snapcast-Audio (Opus, mono) wird dekodiert und über dieselbe
  DSP-/I2S-Ausgabekette wie beim Server wiedergegeben.
* Der lokale I2S-Eingang steht als **alternative Audioquelle** zur
  Verfügung (z. B. Aux-Eingang) — per Pegelerkennung automatisch priorisiert,
  sobald dort ein Signal anliegt, oder über die Web-Konfiguration fest auf
  „nur Netzwerk" oder „nur lokaler Eingang" erzwingbar.
* Puffergröße (`bufferMs`) und ein zusätzlicher Delay-Trim sind konfigurierbar,
  um Mesh-Umstrukturierungen in einem dynamischen Funkumfeld zu überbrücken.
* Die **Lautstärke ist pro Client** einstellbar und wird über die
  Snapcast-Steuerschnittstelle gesetzt (`Client.SetVolume`, also z. B. aus
  einer Snapcast-App heraus); der Server meldet jede Änderung sofort an den
  betroffenen Client. Sie wirkt auf beide Quellen, weil sie die Lautstärke
  dieses Lautsprechers ist und nicht die des Netzwerkstreams. Die eigene
  Web-Konfigurationsseite hat dafür bisher kein Feld.

### Zeitabgleich und Drift

Der Client gleicht seine Uhr über `SNAP_MSG_TIME` mit dem Server ab
(Vierzeiten-Austausch; aus einem gleitenden Fenster zählt die Messung mit der
kleinsten Laufzeit, weil verzögerte Pakete ihre eigene Schätzung verfälschen).
Daraus ergibt sich für jeden Chunk ein Soll-Abspielzeitpunkt
`Zeitstempel + bufferMs − latency + delay_trim_ms`. Abweichungen über 100 ms
werden in einem Schritt korrigiert, darunter kontinuierlich und unhörbar über
das Resampling-Verhältnis (±200 ppm).

Damit der Lautsprecher des **Servers** nicht `bufferMs` vor den Clients spielt,
verzögert dieser seine eigene lokale Ausgabe um denselben Betrag. Der
Netzwerk-Stream bleibt davon unberührt und geht unverzögert raus.

**Noch nicht verifiziert:** Die tatsächliche Synchronität mehrerer Clients über
längere Zeit ist mangels Messaufbau bisher nicht nachgemessen; die Parameter der
Drift-Regelung sind konservativ voreingestellt.

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

### Fremde Snapclients

Offizielle Snapclients (PC, Android, iOS) funktionieren für Musik ohne
Einschränkung: Sie verbinden sich auf Port 1704, werden von einer
Snapcast-Control-App wie jeder andere Client in Lautstärke und Stummschaltung
geregelt und laufen synchron mit den ESP32-Clients.

Zwei Dinge sind ihnen gegenüber anders:

* **Sprachdurchsagen empfangen sie nicht.** Die laufen über einen eigenen
  UDP-Kanal, der nicht Teil des Snapcast-Protokolls ist. Damit Musik nicht
  gegen eine laufende Durchsage anspielt, schaltet der Server fremde Clients
  für deren Dauer stumm (`muted` in den ServerSettings), unabhängig davon, wo
  im Mesh sie hängen. Danach kehrt der vorherige Zustand zurück; die im
  Control-App gesetzte Stummschaltung bleibt davon unberührt.
* **Eigene Protokollfelder ignorieren sie.** Unsere Clients kennzeichnen sich
  im Hello mit `"SnapMesh":1` und werten in den ServerSettings zusätzlich
  `"announcement"` aus. Beide Felder sind Erweiterungen; ein fremder Client
  überliest sie, und ein fremder Snapserver würde `"SnapMesh":1` ebenso
  überlesen.

---

## Sprachdurchsagen

Zusätzlich zur Musik kann über ein Android-Handy eine **Durchsage** gesprochen
werden. Sie läuft bewusst nicht über den Snapcast-Stream: Der ist auf
Lückenlosigkeit ausgelegt und puffert dafür rund drei Sekunden. Eine Durchsage
braucht das Gegenteil, nämlich niedrige Latenz, und verwirft verspätete Pakete
lieber, als auf sie zu warten.

### Ablauf

1. Die App meldet die Durchsage über die bestehende JSON-RPC-Verbindung an
   (`Voice.Start` auf Port 1705). Über TCP, weil ein verlorenes Start- oder
   Stoppsignal nicht passieren darf.
2. Sie nimmt auf und sendet Opus-Pakete an **UDP Port 1706** des Servers,
   16 kHz Mono, etwa 25 kbit/s.
3. Der Server gibt die Durchsage auf seinem eigenen Lautsprecher aus und
   verteilt sie an seine direkt verbundenen Clients.
4. Jeder dieser Clients reicht sie an seine eigenen Kinder weiter, einen Hop
   weit. Weiter entfernte Knoten bekommen sie nicht.
5. `Voice.Stop`, eine Sekunde ohne Ton, drei Minuten Gesamtdauer oder der
   Abbruch der Steuerverbindung beenden die Durchsage.

Gemessene Latenz vom Mund bis zum Lautsprecher, etwa **90 ms**, aufgeteilt in
rund 30 ms Aufnahme und Opus-Encoder im Handy, 6 ms WLAN, 10 bis 20 ms
Sprachpuffer im ESP und 40 ms I2S-Ausgabe. Die Werte stehen in den Logzeilen
der App (`mic_lag`, `enc_lag`, `rtt`) und der Firmware (`voice mailbox: …
wait`).

### Reichweite und Stummschaltung

Warum nur ein Hop: Jede Ebene erbt das Risiko eines sich umbauenden Meshes.
Ein Relay kann beim Neuverbinden Sekunden stehen, was die Musik dank Puffer
überspielt und eine Durchsage nicht. Warum überhaupt ein Hop: Clients, die
wenige Meter vom Server entfernt nebeneinander stehen, hängen sich im Betrieb
aneinander statt an den Server, weil der Nachbar das stärkere Signal ist. Ohne
Weiterleitung wären sie stumm, obwohl sie im selben Raum stehen.

Während einer Durchsage spielt **kein** Gerät Musik. Der Server sendet dazu in
den ServerSettings eine eigene Flagge (`"announcement"`), und jeder unserer
Clients hält seine Musik an, solange sie steht. Wer die Durchsage empfängt,
gibt sie aus; wer nicht, bleibt still. Lautstärke und Stummschaltung des
Nutzers behalten ihre Bedeutung und wirken auch auf die Durchsage: Ein leise
gestellter Lautsprecher gibt auch die Durchsage leiser wieder, ein
stummgeschalteter bleibt stumm.

Nach dem Ende läuft die Musik sofort weiter. Der Stream wird währenddessen
nämlich nicht angehalten, sondern nur nicht ausgegeben. Dadurch bleiben
Ringpuffer und Zeitachse intakt, und es entsteht keine Wartezeit für neues
Vorpuffern.

### Android-App

Die App liegt unter `android/SnapAnnounce` (Kotlin, Jetpack Compose, ab
Android 8).

<img src="docs/snapannounce-screenshot.jpg" alt="SnapAnnounce" width="320">

Der Hauptbildschirm hat nur das Nötige: die Server-IP, den Status und den
verriegelnden Knopf. Ein Druck startet die Durchsage, der nächste beendet sie.
Aufnahme und Versand laufen in einem Vordergrunddienst weiter, auch bei
gesperrtem Bildschirm, und solange eine Durchsage läuft hält die App eine
WLAN-Sperre, damit Android das Funkmodul nicht schlafen legt.

Hinter **Einstellungen** liegt, was einmal je Handy und Raum eingestellt und
dann in Ruhe gelassen wird:

* **Mikrofonquelle.** Vier Möglichkeiten, Standard ist „Standard-Mikrofon“.
  Auf einem Galaxy A56 liefert „Telefonat“ nur −40 dBFS, wo das
  Standard-Mikrofon −5 dBFS erreicht.
* **Max. Verstärkung.** Obergrenze der Pegelautomatik. Mehr heißt lauter, aber
  auch mehr Raum und mehr Rückkopplungsgefahr.
* **Durchsage-Pegel.** Wie laut die Durchsage neben der Musik stehen soll. Zur
  Orientierung: Musik erreicht die Clients mit etwa −24 bis −28 dBFS.

Eine Pegelautomatik mit Begrenzer hält den eingestellten Pegel. Die richtigen
Werte hängen von Handy und Raum ab und werden nach Gehör eingestellt.

### Grenzen

* **Keine Echounterdrückung gegenüber den Lautsprechern.** Androids
  Echounterdrückung kennt nur die eigene Telefonwiedergabe. Steht der Sprecher
  neben einem Lautsprecher, hallt es. Abhilfe: Handy nah an den Mund, weg von
  den Lautsprechern.
* **Knoten tiefer als einen Hop hinter dem Server bleiben stumm**, sie hören
  weder Musik noch Durchsage.
* **Eine Durchsage zur Zeit.** Ein zweites `Voice.Start` wird mit `busy`
  abgelehnt.
* **Das Handy muss im Mesh-WLAN sein**, am besten direkt am Server.

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

Im Normalbetrieb arbeitet der ESP32-S3 als eigenständiger **ESP-Mesh-Lite-Root**.

Die Mesh-Struktur ermöglicht die Verbindung weiterer ESP-Geräte, ohne dass für die reine Audioverteilung zwingend ein separater Raspberry-Pi-Snapserver erforderlich ist.

Der Snapserver selbst stellt seine Dienste über das lokale Netzwerk beziehungsweise Mesh-Netzwerk bereit.

Ist das Mesh nicht erreichbar oder nicht konfiguriert, fällt das Gerät automatisch
auf einen offenen Provisioning-Access-Point zurück (siehe [Web-Konfiguration](#web-konfiguration)),
damit es niemals dauerhaft unerreichbar wird. Das gilt für beide Rollen: auch
ein als Client konfiguriertes Gerät, das keinen Parent findet, fällt auf den
Provisioning-AP zurück statt dauerhaft unerreichbar zu bleiben.

Weitere Geräte können dem Mesh als **Client** beitreten und dort empfangenes
Audio wiedergeben, siehe [Client-Rolle](#client-rolle).

### Ports

| Port | Protokoll | Zweck |
| --- | --- | --- |
| 80 | TCP | Web-Konfiguration |
| 1704 | TCP | Snapcast-Audiostream |
| 1705 | TCP | Snapcast JSON-RPC, dazu `Voice.Start`/`Voice.Stop` |
| 1706 | UDP | Sprachdurchsagen, Handy → Server → Clients |

Die Ebenen des Meshes sind durch NAPT getrennt. Ein Gerät ab Ebene 2 ist
deshalb vom Server aus nicht direkt adressierbar und erscheint dort unter der
Adresse seines Elternknotens. Aus demselben Grund reicht bei Durchsagen jeder
Knoten selbst an seine Kinder weiter, statt dass der Server sie anspricht.

---

## Projektstruktur

Die wichtigsten Komponenten befinden sich unter anderem in:

```text
main/
├── app_main.c            Rollenwahl und Start
├── audio_i2s.c           I2S, Mono-Mischung, Frequenzweiche
├── audio_opus.c          Opus-Encoder (Server)
├── audio_sink.c          Wiedergabe, Quellenwahl, Drift (Client)
├── audio_resample.c      Feinregelung der Abspielrate
├── snapserver.c          Snapcast-Server, Port 1704
├── snapclient.c          Snapcast-Client
├── snapcontrol.c         JSON-RPC, Port 1705
├── voice_announce.c      Sprachdurchsagen, UDP 1706
├── mesh_root.c           Mesh als Root (Server)
├── mesh_client.c         Mesh-Beitritt als Relay (Client)
├── webconfig.c           Konfigurationsseite, Port 80
├── device_config.c       Einstellungen im NVS
├── provisioning.c        Provisioning-AP
├── status_led.c          Status-LED
└── cpu_stats.c           Diagnose: CPU-Last je Task

android/SnapAnnounce/     Android-App für Durchsagen
tools/                    Hilfsskripte
docs/                     Notizen und Screenshots
```

Die genaue Struktur kann sich während der Entwicklung noch ändern.

---
## Abhängigkeiten

Dieses Projekt verwendet Komponenten aus dem ESP-IDF-Ökosystem, darunter:

* **ESP-IDF** — Apache License 2.0
* **ESP-Mesh-Lite** — Apache License 2.0
* **ESP-IoT-Bridge** — Apache License 2.0
* **ESP-Modem** — Apache License 2.0
* **ESP-mDNS** — Apache License 2.0
* **CMake Utilities** — Apache License 2.0
* **esp-opus** — MIT License

Die jeweiligen Drittanbieter-Komponenten unterliegen weiterhin ihren
ursprünglichen Lizenzbedingungen.

Für die vollständigen Lizenztexte und weitere Informationen wird auf die
jeweiligen Upstream-Repositories und den ESP-IDF-Komponenten-Registry
verwiesen.

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

### Android-App

Am einfachsten über Android Studio: `android/SnapAnnounce` öffnen und
ausführen. Auf der Kommandozeile mit einem installierten Gradle 8.13 und
JDK 21:

```bash
cd android/SnapAnnounce
JAVA_HOME=/pfad/zu/jdk-21 gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Das Wrapper-Skript `gradlew` liegt nicht im Repository, nur
`gradle/wrapper/gradle-wrapper.properties` mit der erwarteten Version.

---


## Pinning

| GPIO | Funktion | konfiguriert in |
|------|----------|-----------------|
| 4 | ESP32-S3 → PCM5102A BCK und TinySine BCLK | `main/audio_i2s.h` |
| 6 | ESP32-S3 → PCM5102A LCK und TinySine LRCLK | `main/audio_i2s.h` |
| 5 | TinySine DOUT → ESP32-S3 DIN | `main/audio_i2s.h` |
| 7 | ESP32-S3 DOUT → PCM5102A DIN | `main/audio_i2s.h` |
| 10 | Schleifer des Lautstärkepotis (Vorgabe) | Web-Oberfläche |
| – | Schleifer des Delay-Potis (Vorgabe: keiner) | Web-Oberfläche |
| 48 | WS2812-Status-LED | `menuconfig` |

Dieselbe Verdrahtung als Zeichnung, mit Spannungsversorgung und Masse:

<img src="docs/Verdrahtungsplan.png" width="600">

### Wo die Pinbelegung konfiguriert wird

Drei Orte, je nachdem, wann ein Pin feststehen muss:

**I2S — im Quelltext.** Die vier Pins stehen als `#define`-Block am Kopf von
`main/audio_i2s.h` und sind **vor dem ersten Bauen** an die eigene Hardware
anzupassen. Darunter liegt ein zweiter, auskommentierter Block für eine
abweichende Verdrahtung (GPIO 17, 8, 5, 18). Der I2S-Treiber übernimmt die
Pins einmal beim Start, deshalb sind sie keine Laufzeiteinstellung.

**Status-LED — im `menuconfig`**, zur Bauzeit:

```
idf.py menuconfig  →  Snapserver Mesh Project Configuration
    Status LED (on-board WS2812)          (ein/aus, Vorgabe: ein)
    Status LED GPIO                       (0 bis 48, Vorgabe: 48)
    Potentiometer inputs (volume, delay)  (ein/aus, Vorgabe: ein)
```

Der letzte Schalter nimmt nur den Poti-Code ganz heraus; welche Pins die
Potis benutzen, wird dort nicht eingestellt.

**Potis — auf der Web-Oberfläche**, Abschnitt *Potentiometers*. Beide Pins
sind dort frei wählbar, jeweils auch „none". Angeboten werden **nur Pins,
die tatsächlich frei sind**: Die Firmware berechnet die Liste aus ihrer
eigenen Belegung (I2S-Pins aus `audio_i2s.h`, Status-LED, Strapping-Pin) und
blendet den vom jeweils anderen Poti belegten Pin aus. Wird die I2S-Belegung
geändert, passt sich die Liste nach dem Neu-Flashen von selbst an. Ein
Pinwechsel startet das Gerät neu; die Firmware prüft den Pin beim Speichern
noch einmal selbst, ein Pin außerhalb der Liste wird auch über die API
abgewiesen.

Alles Übrige — Mesh, DSP, Opus, Puffergrößen — wird ebenfalls zur Laufzeit
über die Web-Oberfläche eingestellt und im NVS gehalten.

---


## Potis für Lautstärke und Delay

Zwei 10-kΩ-Potentiometer lassen sich anschließen, beide wirken nur auf den
Lautsprecher an **diesem** Gerät. Server und Clients haben jeweils ihre
eigenen.

```text
3V3 ──┬── Anschluss 1
      │
      ├── Schleifer ────► GPIO (siehe unten)
      │
GND ──┴── Anschluss 3
```

### Lautstärke

Vorgabe **GPIO 10**. **Ohne angeschlossenes Poti liegt volle Lautstärke an**:
Ein interner Pull-up hält den offenen Eingang oben, das Gerät spielt mit
100 % und braucht keine Konfiguration.

Der Regler greift ganz am Ende der Ausgabestufe, nach der Frequenzweiche. Der
Opus-Stream an die Clients wird aus einer anderen Kopie gespeist und bleibt
unberührt: Wer den Server leiser dreht, ändert nichts an dem, was die Clients
hören. Die Lautstärke pro Client aus einer Snapcast-Control-App bleibt
wirksam — beide multiplizieren sich. Kennlinie kubisch, wie bei der
Snapcast-Lautstärke.

### Delay

Vorgabe **kein Pin**. Ist einer gewählt, **ersetzt das Poti das Feld
„Delay trim"** — das Feld wird auf der Seite ausgegraut. Mittelstellung ist
0 ms, die Anschläge sind ±Bereich; der Bereich ist auf der Seite einstellbar,
Vorgabe **±200 ms**, höchstens ±2000 ms. Bei ±200 ms entspricht ein Prozent
Drehweg etwa 4 ms. Linear, denn es ist eine Zeitverschiebung, keine
Lautstärke.

Wirkung wie beim Feld: Ein Client verschiebt seinen Wiedergabezeitplan, der
Server die Verzögerungsleitung vor seinem eigenen Lautsprecher. Die
Bereichsänderung gilt sofort, ohne am Knopf zu drehen.

Warum kein Pin als Vorgabe: Mit internen Pulls lässt sich ein offener Eingang
nur an ein Ende ziehen, nicht in die Mitte. Ein gewählter, aber abgezogener
Delay-Pin liegt deshalb per Pull-down am **negativen Anschlag** (−Bereich),
statt zufällig zu wandern.

Die Seite zeigt beide Potiwerte live im Statusfeld.

### Mögliche Pins

**Nur ADC1, also GPIO 1 bis 10.** ADC2 teilt sich die Hardware mit dem
WLAN-Funkmodul und ist nicht lesbar, solange das Funkmodul läuft — was hier
immer der Fall ist. Ein Pin auf ADC2 würde am Schreibtisch funktionieren und
ausfallen, sobald das Mesh hochkommt. Die Web-Oberfläche bietet ihn deshalb
gar nicht erst an.

Innerhalb von ADC1, bei der aktiven I2S-Belegung:

| GPIO | ADC1-Kanal | Status |
|------|-----------|--------|
| 1 | CH0 | frei |
| 2 | CH1 | frei |
| 3 | CH2 | **ungeeignet** — Strapping-Pin (JTAG-Auswahl), das Poti zöge ihn beim Booten auf einen beliebigen Pegel |
| 4 | CH3 | belegt — I2S BCLK |
| 5 | CH4 | belegt — I2S DIN |
| 6 | CH5 | belegt — I2S LRCLK |
| 7 | CH6 | belegt — I2S DOUT |
| 8 | CH7 | frei; in der auskommentierten Alternativbelegung wäre es LRCLK |
| 9 | CH8 | frei |
| 10 | CH9 | frei, **Vorgabe** für die Lautstärke |

### Einschränkungen

- **Messfehler durch den Pull-up** (Lautstärke). Rund 45 kΩ gegen die
  Schleiferimpedanz, die bei 10 kΩ in Mittelstellung mit etwa 2,5 kΩ am
  höchsten ist: Die Anzeige liegt dort **rund 2,6 %** zu hoch, an beiden
  Enden exakt. Nicht hörbar. Beim Delay-Poti gilt dasselbe mit dem
  Pull-down, dort **rund 2,6 % zu niedrig** in Mittelstellung — bei ±200 ms
  etwa 5 ms. Wer genau „0 ms" in der Mitte braucht, gleicht nach Gehör ab.
- **Pinwechsel nur mit Neustart.** Kanäle und Pulls werden beim Start
  gesetzt. Die Seite startet das Gerät nach dem Speichern selbst neu.
- **Ein gewählter Pin bleibt belegt**, auch wenn kein Poti dran hängt. Wird er
  anderweitig gebraucht: auf „none" stellen.
- **Kein Totalausfall bei Fehlern.** Lässt sich der ADC nicht öffnen, bleibt
  die Lautstärke bei 100 % und das Delay beim Feldwert; der Rest läuft
  weiter, die Meldung steht im Log.
- **Glättung.** Abgefragt wird alle 50 ms, 16 Messungen gemittelt, mit einem
  Totband von gut 1 % gegen Zittern der letzten Bits. Die Lautstärke wird
  zusätzlich über einen 20-ms-Frame eingeblendet — ein Ruck am Knopf braucht
  also bis zu 70 ms, dafür knackt es nicht. Die Endanschläge sind vom
  Totband ausgenommen und bleiben immer erreichbar.
- **Große Delay-Sprünge auf einem Client** (mehr als 100 ms auf einmal) löst
  die Wiedergabe als harten Resync aus: ein kurzer Aussetzer oder
  Stillstand, dann sitzt sie. Kleine Drehungen gleicht die Driftregelung
  gleitend aus, bei 500 ppm höchstens 0,5 ms pro Sekunde — langsames Drehen
  wirkt also verzögert.

---


## Entwicklungsstatus

Das Projekt befindet sich in aktiver Entwicklung.

Im Betrieb bewährt: Snapcast-Übertragung und Zeit-Sync über das Mesh
(Regelfehler wenige Millisekunden), LR4-Frequenzweiche, Rollenumschaltung
Server/Client, Web-Konfiguration, Sprachdurchsagen mit rund 90 ms Latenz.

In Arbeit:

* Stabilität der Relays: ein Knoten mit mehreren Kindern hängt sich
  gelegentlich auf
* der Server hält einen blockierten Client zu lange durch und geht dabei
  selbst auf dem internen Heap auf Grund
* ungeklärte akustische Artefakte, die bereits im aufgenommenen Signal
  stecken
* echte Messung des Versatzes zwischen Server- und Client-Lautsprecher
  statt Beurteilung nach Gehör

Offene Punkte und Messergebnisse aus dem Gerätebetrieb stehen in
[TODO.md](TODO.md).

Änderungen an Audioformat, Buffergrößen, Filterparametern und Netzwerkverhalten sind während der Entwicklung möglich.

---

## Lizenz

MIT License

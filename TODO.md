# Bugfix-Backlog

Aus einer Code-Analyse (2026-09-16) auf `test/fixup`. Alle identifizierten
Bugs sind umgesetzt:

- Stufe 1: Client-Slot-Leak durch fehlenden Recv-Timeout/Keepalive
  (Commit 151007f)
- Stufe 2: Unsynchronisierter 64-Bit-Zugriff auf die Zeitbasis + tote
  Kconfig-Option `SNAPSERVER_MESH_ROOT_ONLY` (Commit 585758d)
- Stufe 3: Race bei `client->peer` beim Slot-Recycling (Commit 7d6fb86)
- Stufe 4 (aus Live-Log 2026-09-17 gefunden): `audio_task` versendet
  Wire-Chunks synchron und blockierend an jeden Client nacheinander
  (`SO_SNDTIMEO`=2s). Ein einzelner langsamer/überlasteter Client konnte die
  I2S-Capture-Pipeline dadurch für alle Clients bis zu mehrere Sekunden
  blockieren (im Log bis 2,6s Drift beobachtet). Fix: Vor jedem Send prüft
  `client_socket_writable()` per Zero-Timeout-`select()`, ob der Socket
  aktuell beschreibbar ist; falls nicht, wird der Chunk für diesen Client
  übersprungen (`chunks_skipped`-Zähler) statt die Pipeline zu blockieren.
  Auf dem Gerät verifiziert: keine Drift-Warnungen mehr bei parallel
  belastetem Client.
- Stufe 5 (Code-Review auf `test/provisioning`, 2026-09-17, Effort `high`,
  12 Befunde): Passwort-Leak bei 1-7-Zeichen-Mesh-Passwort (offener AP,
  aber `/api/config` gab das Passwort weiterhin heraus); Config-Server
  konnte durch einen hängenden POST dauerhaft blockiert werden
  (`esp_http_server` ist single-threaded); Build brach bei
  `SNAPSERVER_ENABLE_MESH_LITE=n` (zwei verschiedene Ursachen: abhängige
  Kconfig-Symbole existieren nicht, und ein bool-Kconfig-Symbol ist bei `n`
  gar nicht definiert, nicht `0` — beides in `device_config.c` gefixt und
  mit `idf.py build` bei deaktiviertem Mesh gegengeprüft); SSID-Länge bei
  genau 32 Zeichen falsch berechnet (`strlcpy`-Ziel vs. Quelle); NVS-Writes
  im knapp bemessenen `esp_timer`-Task-Stack statt eigenem Task;
  `device_config_t`-Cache ohne Synchronisation zwischen HTTP- und
  Timer-Task; Factory-Reset konnte durch einen noch laufenden
  Grace-Window-Write still rückgängig gemacht werden; `schedule_reboot()`
  konnte einen NULL-Timer starten; Config-Seite startete erst nach
  Audio-Init (bei Audio-Fehler nie erreichbar); dazu zwei
  Design-Entscheidungen: Provisioning-AP rebootet nicht mehr nach 3 Minuten
  (schaltet nur noch den Funk ab, Wiedereinstieg nur per Power-Cycle oder
  Save), und ein Save während aktivem Provisioning-AP rebootet jetzt immer.
  Auf dem Gerät verifiziert: normaler Mesh-Boot ohne Regression, Config-Web-
  server startet jetzt vor Audio-Init. Nicht auf dem Gerät verifizierbar
  (kein WLAN-fähiges Zweitgerät in dieser Sandbox verfügbar): Passwort-
  Ablehnung <8 Zeichen per curl, 32-Zeichen-SSID im WLAN-Scan, volles
  3-Minuten-Timeout-ohne-Reboot-Fenster, Stresstest paralleler Saves
  während des Grace-Windows — siehe Plan-Datei für den vollständigen
  Verifikationsplan.

- Stufe 6 (Feature auf `test/ServerClient`, Plan aus Analyse der Client-Rolle
  und des Referenzprojekts `ESP32_Mesh_Snapclient`, Stufe 1 von 2): der
  ESP32-S3 kann nun per NVS-Feld `role` wahlweise als Server (Mesh-Root, wie
  bisher) oder als Client (Mesh-Non-Root-Relay, Snapcast-Empfang) booten,
  umschaltbar über die Web-Config-Seite, Übernahme per Reboot. Neu:
  `mesh_client.c` (Non-Root-Join, nie Leaf — ein Leaf könnte in der
  schlauchförmigen Zieltopologie keine Kinder annehmen; Server-Adresse über
  `esp_mesh_lite_get_root_ip()`, da lokales DHCP-Gateway ab Level 3 nur den
  Elternknoten liefert und mDNS die NAPT-Grenze zwischen den Ebenen nicht
  überquert), `snapclient.c` (Snapcast-Protokoll, portiert aus dem
  Referenzprojekt, aber ohne A2DP/Bluetooth — der ESP32-S3 hat kein
  Bluetooth Classic — und mit tatsächlicher Auswertung von Rate/Kanälen aus
  dem CodecHeader statt hartcodiertem Stereo, da unser eigener Server mono
  sendet), `audio_sink.c` (Quellen-Arbiter zwischen Netzwerk-Audio und
  lokalem I2S-Eingang als Ersatz für die A2DP-Priorität der Referenz,
  PSRAM-Ringpuffer). `audio_i2s.c` wurde dafür in Capture- und
  DSP+Ausgabe-Hälfte aufgetrennt (`audio_i2s_capture_mono()`/
  `audio_i2s_write_mono()`), ohne das bestehende Server-Verhalten zu ändern.
  `bufferMs` ist jetzt konfigurierbar statt hart 1000 (Server sendet den
  Wert aus `device_config`, Client dimensioniert seinen Ringpuffer danach).
  Auf dem Gerät verifiziert: Build sauber, sowohl mit
  `CONFIG_SNAPSERVER_ENABLE_MESH_LITE=y` als auch `=n`. Nicht verifiziert
  (kein zweites WLAN-fähiges Testgerät in dieser Sandbox verfügbar):
  tatsächlicher Mesh-Join als Non-Root, Snapcast-Wiedergabe, automatische
  Quellenumschaltung auf den lokalen Eingang, Rollenwechsel per Reboot.
  Stufe 2 (noch offen): Zeit-Sync (`SNAP_MSG_TIME`) und Drift-Kompensation,
  damit mehrere Clients über Stunden synchron bleiben — siehe Plan-Datei.

## Nicht als Bug, aber vorgemerkt

- `partitions.csv` hat keine OTA- oder Coredump-Partition. Kein Problem für
  den aktuellen Funktionsumfang, aber falls OTA-Updates oder
  Crash-Diagnose per Coredump später gewünscht sind, fehlt dafür die
  Partitionierung.

- **Lautstärke der Quelle wirkt auf alle Clients gemeinsam (gemeldet
  2026-09-17):** Wird am A2DP-Gerät, das den I2S-Eingang des Servers
  speist, die Lautstärke verändert, ändert sich der Pegel für sämtliche
  Clients gleichzeitig. Das ist derzeit systembedingt und kein Fehler im
  engeren Sinn: Die Regelung sitzt **vor** der Aufnahme, der Server
  encodiert also bereits das abgesenkte Signal, und jeder Client bekommt
  es so. Zwei Konsequenzen daraus, die zusammen gehören:
  1. **Erledigt 2026-09-18:** Das Snapcast-Protokoll hat eine Lautstärke
     **pro Client**, die der Server bereits verwaltet
     (`client->volume_percent`, über `Client.SetVolume` der JSON-RPC in
     `snapcontrol.c`) und nach jeder Änderung erneut in den ServerSettings
     annonciert. Der Client wertet `volume`/`muted` jetzt aus und wendet
     sie an. Die Kette ist damit über eine Snapcast-Control-App komplett
     bedienbar. Angewendet wird am Ende des Wiedergabepfads, nicht beim
     Einspeisen — sonst wäre der Pegel im Ringpuffer eingebacken und eine
     Änderung erst `buffer_ms` später hörbar. Die Kennlinie ist kubisch
     (perzeptive Näherung, sonst drängt sich der ganze nutzbare Bereich
     ins obere Ende des Reglers); falls sie in der Praxis zu steil wirkt,
     ist sie in `audio_sink_set_volume()` mit einer Zeile zu ändern.
  2. **Bleibt offen:** Damit das etwas nützt, muss die Quelle auf
     konstantem Pegel bleiben (A2DP-Lautstärke voll aufdrehen und nicht
     mehr anfassen), sonst multipliziert sich beides. Den Pegel der Quelle
     zurückzurechnen geht ohne Rückkanal vom A2DP-Empfänger nicht sauber.
  3. **Bleibt offen:** Die eigene Web-Config kennt kein Lautstärkefeld. Für
     den Alltag reicht die Control-App, aber wer nur den Browser hat,
     kommt an die Einstellung nicht heran.
  4. **Bleibt offen:** Der Server hält die Lautstärke nur im RAM
     (`s_clients[i].volume_percent`, beim Start und beim Belegen eines Slots
     auf 100 gesetzt) — nach einem Server-Neustart stehen alle Clients
     wieder auf voll. Für eine dauerhafte Einstellung müsste sie pro
     Client-ID ins NVS, was einen Speicherplatz pro bekanntem Client
     bedeutet und deshalb eine eigene kleine Entscheidung ist.

- **Clients im laufenden Mesh konfigurieren (gemeldet 2026-09-17):** Aktuell
  ist unklar dokumentiert, wie man an die Config-Seite eines Clients kommt,
  der bereits im Mesh hängt. Beim Durchsehen der Logs zeigt sich, dass der
  Fall einfacher liegt als früher in diesem Projekt angenommen, aber nur
  für die erste Ebene:
  - Ein Client, der direkt am Root hängt, bekommt seine STA-Adresse aus dem
    DHCP des Roots und liegt damit im **selben** Subnetz wie ein Handy, das
    sich mit dem Root-AP verbindet (im Log: Root-AP `192.168.5.1`, Client
    `192.168.5.2`). Dazwischen liegt kein NAT — `http://192.168.5.2/`
    sollte also bereits heute funktionieren. Das ist ungetestet und wäre
    als Erstes zu verifizieren, weil es ohne jede Codeänderung auskäme.
  - Ab Ebene 3 greift die NAT-Schachtelung von `esp_bridge` wieder: ein
    Enkelknoten liegt hinter dem NAT seines Elternknotens und ist von oben
    nicht adressierbar.
  - Es fehlt in jedem Fall die Auffindbarkeit — welche IP gehört zu welchem
    Gerät. Der Server kennt das bereits (`client[i]`-Statuszeile mit IP und
    Hostname aus dem Hello) und könnte es auf seiner Config-Seite als Liste
    mit Links anzeigen. Das wäre der kleinste sinnvolle Schritt.
  - Für tiefere Ebenen bliebe ein Proxy über den Server (Weiterleitung an
    einen ausgewählten Knoten über die Mesh-Lite-interne Verbindung) — das
    ist deutlich aufwendiger und sollte erst geplant werden, wenn die
    Topologie wirklich mehr als zwei Ebenen hat.

- **Root-Failover-Risiko im Client-Modus (aus Analysegespräch 2026-09-17,
  zurückgestellt):** `esp_mesh_lite_set_disallowed_level(1)` in
  `mesh_client.c` schließt den Client beim regulären Beitritt sicher von
  der Root-Rolle aus (API-Vertrag, keine Timeout-basierte Übernahme). Die
  Mesh-Lite-Doku beschreibt aber einen separaten Selbstheilungspfad bei
  Root-Ausfall ("Root Node Failure"), bei dem ein Kind-Knoten nach
  mehreren fehlgeschlagenen Reconnect-Versuchen selbst Root werden kann —
  dort beschrieben für den Router-verbundenen Modus ("connect directly to
  the router"), was auf unser No-Router-Setup (Client hat keine
  Router-Config) vermutlich nicht direkt zutrifft. `esp_mesh_lite_core`
  liegt nur als vorkompilierte `.a` vor (`lib/libesp_mesh_lite_esp32s3.a`),
  daher nicht quellcodeseitig verifizierbar, ob `disallowed_level` auch in
  diesem Pfad greift. Risiko laut Nutzer als kritisch eingestuft: würde
  der Client dennoch Root werden, kollidiert er mit dem echten Server-Root
  und bleibt es dauerhaft, da nichts in unserem eigenen Code den
  Mesh-Level nach dem Boot überprüft.

  Vorgeschlagene Absicherung (noch nicht umgesetzt, zurückgestellt):
  periodischer Wächter in `mesh_client.c`, der `esp_mesh_lite_get_level()`
  zyklisch abfragt und bei Level 1 sofort `esp_wifi_stop()` auslöst
  (gleiches Muster wie der Provisioning-AP-Timeout) statt den Client
  stillschweigend als Root weiterlaufen zu lassen.

  Pflicht-Testszenario vor Stufe 2: Server im laufenden Betrieb
  ausschalten, während ein Client verbunden ist, und das Client-Log
  beobachten — aktuell ungetestet (kein zweites WLAN-fähiges Testgerät in
  dieser Sandbox verfügbar).

- Stufe 6 Nachbesserungen (erster echter Zwei-Geräte-Test auf `test/
  ServerClient`, 2026-09-17): vier Bugs beim tatsächlichen Betrieb
  gefunden und gefixt: (1) `esp_mesh_lite_get_root_ip()` liefert die IP
  mit vertauschter Byte-Reihenfolge zurück (`192.168.5.1` kam als
  `1.5.168.192` an) — Client fand den Server dadurch nie; Fix per
  `__builtin_bswap32()` in `mesh_client.c`. (2) Jeder Client hatte eine
  eigene, MAC-suffigierte SSID (`SnapMesh_XXXXXX`), was bei mehreren
  Clients zu SSID-Wildwuchs führte; Mesh-Lite erkennt Eltern-Knoten aber
  über eine Vendor-IE-Kennung im Beacon, nicht über den SSID-Text, daher
  jetzt einheitliche SSID (identisch zum Server) für alle Rollen. (3)
  Jedes Gerät (Server und alle Clients) meldete denselben mDNS-Namen
  `snapserver.local`, wodurch DNS/mDNS-Caches beim Wechsel zwischen
  Geräten auf der falschen Config-Seite landen konnten; jetzt eindeutiger
  Name pro Rolle+MAC (`snapserver-XXXXXX`/`snapclient-XXXXXX`). (4) Der
  Server trennt einen Client nach 30s Funkstille von dessen Seite
  (`CLIENT_RECV_TIMEOUT_US`, gedacht um tote Verbindungen zu erkennen) —
  unser eigener Snapclient sendet aber (Zeit-Sync ist ja erst Stufe 2)
  nichts zurück und wurde dadurch zuverlässig alle ~31s getrennt; Fix:
  `snapclient.c` sendet jetzt alle 2s eine leere `SNAP_MSG_TIME`-Anfrage
  rein als Herzschlag (Antwort wird ignoriert). Zusätzlich fiel auf, dass
  der Ringpuffer trotz `buffer_ms=3000` in der Praxis nur 40-120ms Füllstand
  hielt, weil `audio_sink.c` sofort bei der ersten eintreffenden
  Netzwerk-Anfrage umschaltete statt erst ein Polster aufzubauen — jede
  kleine Timing-Schwankung erzeugte dadurch einen hörbaren Underrun; Fix:
  Umschalten auf Netzwerkquelle erst ab 80% Füllstand
  (`NETWORK_PREBUFFER_PERCENT`), spätere Einbrüche lösen kein erneutes
  Prebuffering mehr aus.

  Auf dem Gerät verifiziert: Client verbindet nach den Fixes zuverlässig
  zum Server, Ringpuffer füllt sich wie erwartet, `output_rms`-Log
  bestätigt echtes Signal am Ausgang. Nicht behoben: deutliches Knistern
  im Audio, das laut Nutzer auch am **Server** selbst auftritt (dort ohne
  jede Code-/HW-Änderung an diesem Pfad) und nur bei aktivem Stream zu
  hören ist, nicht bei Stille — daher software-, nicht hardwarebedingt.
  Klingt nach ca. einer Minute Laufzeit spürbar ab. Ursache nicht
  identifiziert, auf Nutzerwunsch zurückgestellt statt weiter untersucht.

- Stufe 7 (Stufe 2 des Server/Client-Plans: Zeit-Sync und Drift, 2026-09-17):
  der Client richtet seine Wiedergabe jetzt auf die Serveruhr aus statt nur
  dem Ringpuffer zu folgen. `snapclient.c` macht den vollständigen
  Vierzeiten-Austausch über `SNAP_MSG_TIME` (t1 lokal gesendet, t2/t3 aus
  `received_*`/`sent_*` der Antwort, t4 lokal empfangen) und veröffentlicht
  aus einem gleitenden Fenster von 12 Messungen die **mit der kleinsten
  RTT** — im Multi-Hop-Mesh hat die RTT-Verteilung einen langen Schwanz, und
  ein verzögertes Paket verfälscht seine eigene Offset-Schätzung um etwa die
  halbe Zusatzverzögerung; Mitteln würde das hineinrechnen statt es zu
  verwerfen. Die Anfragen ersetzen zugleich den in Stufe 6 eingebauten
  reinen Herzschlag. `handle_server_settings()` wertet jetzt `bufferMs` und
  `latency` aus, `handle_wire_chunk()` reicht den Chunk-Zeitstempel weiter.
  `audio_sink.c` führt daraus eine Wiedergabe-Zeitachse: Soll-Zeitpunkt =
  `chunk_ts - offset + bufferMs - latency + delay_trim_ms`, verglichen mit
  `esp_timer` plus `AUDIO_I2S_TX_LATENCY_US` (die DMA-Kette hängt der
  Schreibfunktion um 40 ms hinterher, dafür sind die DMA-Konstanten jetzt in
  `audio_i2s.h` öffentlich). Fehler über 100 ms werden hart korrigiert
  (Überspringen wenn zu spät, Stille halten wenn zu früh), darunter
  kontinuierlich über das Resampling-Verhältnis: PI-Regler, auf +-200 ppm
  begrenzt, mit Slew-Limit von 5 ppm je 20-ms-Frame. Neu dafür
  `audio_resample.c` mit 32.32-Phasenakkumulator (die 16.16-Variante der
  Referenz quantisiert auf ~15 ppm und ist damit gröber als die zu
  korrigierende Drift).

  Zwei Dimensionierungsfehler fielen beim Durchrechnen auf und sind
  mitgefixt: (1) Der Ring war auf exakt `buffer_ms` dimensioniert, obwohl
  das die *Dauerfüllung* ist und nicht die Obergrenze — der Server stempelt
  einen Chunk beim Aufnehmen, der Client spielt ihn `buffer_ms` später, also
  sind permanent `buffer_ms` unterwegs. Er lief damit dauerhaft am Anschlag
  und hätte ankommendes Audio verworfen; Kapazität jetzt doppelt so groß
  wie die Soll-Füllung, die Prebuffer-Schwelle bezieht sich auf letztere.
  (2) Solange kein Zeit-Sync steht, wurde der Regler je Frame komplett
  zurückgesetzt — das verwirft auch den Staging-Rest des Resamplers und
  hätte 50-mal pro Sekunde ein Sample verschluckt; getrennt in vollen Reset
  (nur bei echten Brüchen) und reine Reglerneutralisierung.

  Ebenfalls umgesetzt (Plan-Punkt 13): der Server verzögert seine **eigene**
  lokale Ausgabe um `buffer_ms + delay_trim_ms`. Ohne das spielt sein
  Lautsprecher systematisch `bufferMs` — also per Default drei Sekunden — vor
  jedem Client, weil er einen Frame direkt nach der Aufnahme ausgibt, während
  die Clients denselben Chunk laut Zeitplan erst `bufferMs` später spielen.
  Die Verzögerungsleitung sitzt in `audio_i2s_read_frame()` und damit
  ausschließlich im Server-Pfad; der Client darf nicht ein zweites Mal
  verzögert werden, sein Scheduler platziert die Ausgabe ja bereits. Wichtig
  dabei: verzögert wird nur die Kopie für den Lautsprecher, **nicht** der
  Puffer, den der Opus-Encoder bekommt — sonst ginge der Netzwerk-Stream
  ebenfalls verzögert raus und der Effekt verdoppelte sich beim Client. Der
  Ring ist für den vollen `delay_trim_ms`-Bereich dimensioniert, damit der
  Trim im Betrieb verstellbar bleibt (wirkt über `apply_live_params()`, nur
  in der Server-Rolle). `buffer_ms` löst dafür jetzt einen Reboot aus, weil
  es auf beiden Rollen mehrsekündige Puffer beim Start dimensioniert.
  Konfigurierbarkeit der Server-Verzögerung wurde bewusst weggelassen: ohne
  sie ist das System schlicht unsynchron, ein Schalter dafür hätte keinen
  sinnvollen zweiten Zustand.

  Verifiziert: Build sauber, sowohl mit `CONFIG_SNAPSERVER_ENABLE_MESH_LITE=y`
  als auch `=n`.

- Stufe 8 (zwei Befunde aus dem ersten Zwei-Geräte-Lauf mit Zeit-Sync,
  2026-09-17): (1) Der Regelfehler blieb dauerhaft bei +72…+100 ms stehen,
  während `ppm` an seiner Begrenzung von −200 klebte. Ursache war meine
  eigene Hysterese: Der „zu früh"-Zweig des harten Resyncs hielt Stille nur,
  *bis der Fehler unter die Schwelle fiel*, übergab also systematisch einen
  Restfehler von einer vollen Schwellenbreite (100 ms) an den Feinregler —
  und der braucht bei 200 ppm rund 400 Sekunden für 80 ms. Die
  Einstiegsbedingung war richtig, die Ausstiegsbedingung falsch; gehalten
  wird jetzt, bis der Fehler tatsächlich null ist. Danach gemessen: `err`
  zwischen −3 und −6 ms, `ppm` zwischen 4 und 11, also im Bereich der
  reinen Quarzdrift. (2) Der Client fiel für ~25 s komplett aus dem Netz:
  der Sendepuffer des Servers lief voll (`chunks/s=0`, `skipped` +51/s),
  die Gegenrichtung stand still (`time_msgs` eingefroren), und am Ende warf
  der AP die Station nach sechs unbeantworteten SA-Query-Versuchen raus.
  Verdächtig war ESP-IDFs Default-Powersave für verbundene Stationen
  (`WIFI_PS_MIN_MODEM`, Listen-Interval 3 ⇒ bis zu ~307 ms Funkstille) —
  das passt sowohl zu den vollgelaufenen TCP-Puffern als auch zu den
  verpassten Managementframes und bringt bei netzbetriebenen Lautsprechern
  ohnehin nichts. `esp_wifi_set_ps(WIFI_PS_NONE)` in beiden Rollen; laut
  Nutzer läuft es damit spürbar besser.

  Offen und noch nicht gemessen: Synchronität zweier Clients über längere
  Zeit, Verhalten bei Parent-Wechsel, Wirkung von `delay_trim_ms`, und ob
  Server- und Client-Lautsprecher nach der Delay-Line hörbar zusammenpassen.
  Die PI-Parameter (Kp/Ki, ±200 ppm, 5 ppm Slew je Frame) sind ohne Messung
  konservativ gesetzt; die `err=`/`ppm=`-Werte der `AUDIO_SINK`-Statuszeile
  sind zum Nachjustieren gedacht. Falls die Funkverbindung weiterhin
  aussetzt, ist der nächste Kandidat `esp_mesh_lite_set_wifi_reconnect_interval(2, 3, 5)`
  in `mesh_client.c`: alle 5 s ein voller Scan zwingt das Funkmodul vom
  Kanal und ist eine bekannte Ursache für TCP-Hänger. Der Wert stammt aus
  Stufe 6 und war bewusst aggressiv gewählt, weil das Zielumfeld
  „hochdynamisch" sein soll — schnelles Wiederfinden gegen stabilen Stream
  ist gegeneinander abzuwägen.

- Stufe 9 (Mehr-Client-Betrieb, 2026-09-18). Fünf Clients gleichzeitig
  laufen jetzt mit `skipped=0`; die Ursachenkette dahin steht in den Commits
  `9c00054`…`49bb6c0`. Was daraus offen bleibt:

  **Fusion-Intervall auf 20 s — beobachten.** `esp_mesh_lite_set_fusion_config()`
  in `mesh_client.c` steht auf `fusion_frequency_sec = 20` statt der
  voreingestellten 600. Der Grund war eine Mesh-Insel: fällt der Root weg,
  sehen die übrigen Knoten weiterhin gegenseitig ihre Beacons und melden sich
  aneinander an. Das Ergebnis ist assoziiert, hat eine DHCP-Lease und sieht
  von innen gesund aus, hat aber keinen Weg zum Server — und scannt deshalb
  nie neu. Fusion ist der Mechanismus, der getrennte Meshes wieder
  zusammenführt, und zehn Minuten Wartezeit darauf sind zu lang.
  Der Haken: die Doku sagt „during fusion, the device to be fused will scan
  the primary fusion device". Ob dieser Scan auch dann läuft, wenn gar keine
  Insel existiert, ist nicht nachlesbar — die Bibliothek liegt nur
  vorkompiliert vor. Falls ja, wäre das alle 20 s ein kurzer Ausflug vom
  Kanal, also genau die Störungsart, die uns die Aussetzer beschert hat.
  Im Lauf vom 2026-09-18 war davon nichts zu sehen (65 s durchgehend
  `err=-7…-11 ms`, `ppm 12–25`, serverseitig `skipped=0`). **Wenn wieder
  regelmäßige kleine Einbrüche auftauchen, ist das der erste Verdächtige**;
  Gegenmaßnahme wäre 60 s statt 20 s — immer noch zehnmal schneller als der
  Standard. Dasselbe gilt für das schon weiter oben vermerkte
  `esp_mesh_lite_set_wifi_reconnect_interval(2, 3, 5)`.

  **Doppelte IP nach Server-Neustart — nur die Folge behoben.** Die
  DHCP-Lease-Tabelle des Servers liegt im RAM und ist nach einem Neustart
  leer. Clients, die durchgelaufen sind, behalten ihre Adresse, während der
  frische DHCP-Server dieselbe an den nächsten Anfragenden vergibt. Zwei
  Stationen, eine Adresse, ein ARP-Eintrag am Root — die SYNs des einen
  laufen ins Leere (auf Gerät gesehen: 48 s Timeouts bei −36 dBm direkt am
  Root, behoben erst durch einen Rejoin mit neuer Adresse). Der
  Unreachable-Watchdog in `snapclient.c` räumt das nach ~20 s auf, die
  Ursache bleibt. Denkbare echte Fixes: feste Adressen je Client statt DHCP,
  oder die Lease beim Verbindungsverlust freigeben. Erst messen, ob es nach
  dem Watchdog überhaupt noch stört.

  **`SNAPSERVER_MAX_CLIENTS = 10` ist gerechnet, nicht getestet.** Nachdem
  die Task-Stacks im PSRAM liegen, kostet ein Client intern nur noch seinen
  lwIP-Socket, rund 6 kB. Bei ~121 kB freiem internem Heap nach dem Start
  sollten zehn Clients rund 60 kB übrig lassen. Gemessen wurde bisher bis
  fünf (`min_ever` ≈ 43–105 kB). Ob der WLAN-Treiber bei zehn
  Unicast-Streams noch etwas anderes reißt, ist offen — die `heap:`-Zeile
  beantwortet es.

  **`frame delta`-Spitzen wachsen mit der Client-Zahl.** Der Mittelwert
  bleibt bei 20000 µs, aber `max` ist von ~22 ms auf 30–38 ms gestiegen. Bei
  40 ms DMA-Reserve ist das noch tragbar, der Abstand ist aber kleiner als
  vorher. Frühwarnzeichen wäre die Rückkehr von
  `AUDIO_I2S: Capture timeline drifted`.

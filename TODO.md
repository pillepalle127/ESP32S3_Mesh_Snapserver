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

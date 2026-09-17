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

## Nicht als Bug, aber vorgemerkt

- `partitions.csv` hat keine OTA- oder Coredump-Partition. Kein Problem für
  den aktuellen Funktionsumfang, aber falls OTA-Updates oder
  Crash-Diagnose per Coredump später gewünscht sind, fehlt dafür die
  Partitionierung.

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

## Nicht als Bug, aber vorgemerkt

- `partitions.csv` hat keine OTA- oder Coredump-Partition. Kein Problem für
  den aktuellen Funktionsumfang, aber falls OTA-Updates oder
  Crash-Diagnose per Coredump später gewünscht sind, fehlt dafür die
  Partitionierung.

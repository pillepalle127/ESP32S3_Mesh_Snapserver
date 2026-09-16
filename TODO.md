# Bugfix-Backlog

Aus einer Code-Analyse (2026-09-16) auf `test/fixup`. Stufe 1 ist umgesetzt
(Commit 151007f: Client-Slot-Leak durch fehlenden Recv-Timeout/Keepalive).

## Stufe 2 – Mittel (Korrektheit)

- **Unsynchronisierter 64-Bit-Zugriff auf die Zeitbasis**
  `main/snapserver.c`: `s_wall_offset_us` wird in `handle_time()` außerhalb
  jeder Critical Section geschrieben, während `now_us()` denselben Wert aus
  mehreren Tasks (jeder `client_task`, `audio_task`) ungeschützt liest. Auf
  der 32-Bit-Xtensa-CPU ist ein int64_t-Zugriff nicht atomar — ein
  gleichzeitiger Schreib-/Lesezugriff kann einen zerrissenen Wert liefern.
  Fix: Zugriffe unter `portENTER_CRITICAL`/`portEXIT_CRITICAL`
  synchronisieren, analog zu den bestehenden `s_clients_lock`-Stellen.

- **Tote Kconfig-Option `SNAPSERVER_MESH_ROOT_ONLY`**
  `main/Kconfig.projbuild:7-10` deklariert die Option (default y), aber
  `main/mesh_root.c` liest sie nirgends — Root-Only-Verhalten ist fest
  verdrahtet (`config.leaf_node = false;`,
  `esp_mesh_lite_set_allowed_level(1);`). Deaktivieren der Option in
  menuconfig hat keinerlei Effekt. Fix: entweder im Code tatsächlich
  auswerten oder die Option entfernen.

## Stufe 3 – Niedrig (Robustheit)

- **Race bei `client->peer` beim Slot-Recycling**
  `main/snapserver.c`: Schreiben (Annahme neuer Verbindung) und Lesen
  (Session-Summary-Logzeile in `close_client()`) von `client->peer`
  passieren außerhalb der Critical Section. Trennt sich ein Client exakt in
  dem Moment, in dem ein neuer Client denselben Slot übernimmt, kann die
  Log-Zeile eine teilweise überschriebene IP zeigen. Rein kosmetisch (fester
  16-Byte-Puffer, kein Speicherfehler), aber ein echtes Datenrennen.

## Nicht als Bug, aber vorgemerkt

- `partitions.csv` hat keine OTA- oder Coredump-Partition. Kein Problem für
  den aktuellen Funktionsumfang, aber falls OTA-Updates oder
  Crash-Diagnose per Coredump später gewünscht sind, fehlt dafür die
  Partitionierung.

# Code-Review: Sprachdurchsagen (Branch `test/voice`)

Stand 2026-09-19, Basis `7ba6726` + uncommittete Änderungen.

**Geprüft:** alle Änderungen für die Sprachdurchsagen — `main/voice_announce.c/.h`,
die Anpassungen in `audio_sink.c`, `audio_i2s.c`, `snapserver.c`, `snapcontrol.c`,
`status_led.c`, `app_main.c`, dazu die Android-App (`android/SnapAnnounce/`) und
`tools/voice_test.py`.

**Nicht geprüft:** der bereits committete Teil des Branches gegenüber `main`.
Nichts davon lief auf Hardware. Die App wurde nicht gebaut, weil auf dem
Rechner kein Android-SDK installiert ist. Alle Befunde unten sind am Code
nachvollzogen; die Szenarien sind hergeleitet, nicht beobachtet.

## Übersicht

| Nr | Schwere | Ort | Befund |
|----|---------|-----|--------|
| K1 | kritisch | `voice_announce.c:152` | Level-2-Clients werden nicht stummgeschaltet |
| K2 | kritisch | `voice_announce.c:451` | Clients verwerfen jede weitere Durchsage |
| K3 | kritisch | `voice_announce.c:147` | 3,4 KB Array auf dem 6-KB-Stack von `ctrl_conn_task` |
| K4 | kritisch | `audio_sink.c` (Player/Ring) | Bei Durchsagen über ca. 4 s kommt die Musik auf Level-1 nicht nahtlos zurück |
| H1 | hoch | `voice_announce.c` / `snapserver.c:1675` | Clients, die während einer Durchsage (neu) verbinden, sind nicht stumm |
| H2 | hoch | `voice_announce.c:176/227` | Start/Stop-Races: Clients bleiben dauerhaft stumm oder der Zustand hängt |
| H3 | hoch | Architektur | Level-1-Clients spielen Musik, bis das erste Sprachpaket ankommt |
| M1 | mittel | `voice_announce.c:305` | Race beim Nachstellen des Watchdogs kann eine schnelle Folgedurchsage abwürgen |
| M2 | mittel | `voice_announce.c:213` | Stop-Arbeit läuft im esp_timer-Task: blockiert, Stack nicht geprüft |
| M3 | mittel | `audio_sink.c:869`, `audio_i2s.c:667` | Mailbox ohne Vorpuffer; bei Überlauf fliegen die neuesten Samples raus |
| M4 | mittel | `voice_announce.c` | Lautstärke-/Mute-Änderungen während einer Durchsage gehen verloren |
| M5 | mittel | `VoiceAnnounceService.kt:166` | App-Timeout von 1,5 s gegen synchrones Muten auf dem Server |
| N1 | niedrig | `audio_sink.c:753` | LED bleibt im Durchsage-Puls hängen |
| N2 | niedrig | `voice_announce.c` | UDP 1706 nimmt während einer Durchsage Audio von jedem Absender an |
| N3 | niedrig | `snapcontrol.c:471`, `VoiceAnnounceService.kt:178` | Ablehnung wird als „Method not found“ gemeldet |

K1 bis K3 lassen sich mit wenigen Zeilen beheben. K4 und die H-Punkte brauchen
die beiden Umbauten im Abschnitt „Empfohlene Umbauten“.

**Stand 2026-09-19 abends:** Alles außer M3 ist umgesetzt. Die Firmware und
die App bauen, auf Hardware getestet ist davon noch nichts. K3 war vorher auf
Hardware reproduziert (Stack-Overflow in `snapctrl_conn` beim ersten
`Voice.Start`).

| Punkt | Umsetzung |
|---|---|
| K1, H1, M4 | Umbau A: `snapserver_set_announcement()`; `send_server_settings()` mutet per MAC-Test zusätzlich zum App-Mute, auch beim Handshake. Einmal pro Sekunde wird nachgeprüft (Ebenenwechsel). |
| K2 | Client setzt die Sequenz nach 400 ms Pause zurück; außerdem nummeriert der Server seinen Strom jetzt selbst durchgehend. |
| K3 | Client-Snapshot im PSRAM; das Stummschalten läuft ohnehin nicht mehr im RPC-Handler. |
| K4 | Umbau B: Die Durchsage ist ein Overlay, die Musik wird darunter weiter berechnet und nur nicht ausgegeben. |
| H2 | Ein Mutex über Start/Stop; es gibt keinen gespeicherten Mute-Zustand mehr, der überschrieben werden könnte. |
| H3 | Stille-Pakete an Level 1 ab `Voice.Start`, bis das Handy liefert. |
| M1, M2 | Umbau C: keine esp_timer mehr, Watchdogs per Zeitstempel im `voice_server_task`. |
| M5 | Der RPC mutet nicht mehr synchron; die App wartet 5 s auf Antworten. |
| N1 | Die LED wird während der Durchsage jeden Frame gesetzt und danach passend zur Quelle zurückgestellt. |
| N2 | Nur UDP von der IP der Owner-Verbindung wird verteilt. |
| N3 | Ablehnung als `result.busy`; die App unterscheidet „läuft schon“ und „Firmware kann das nicht“. |
| M3 | offen, nach dem ersten Hörtest |

Nebenbei gefunden und behoben: `tools/voice_test.py` hätte bei einer
Ablehnung `Voice.Stop` geschickt und damit die fremde Durchsage beendet.

---

## Kritisch

### K1 — Level-2-Clients werden nicht stummgeschaltet
**Ort:** `mute_non_level1_clients()`, `voice_announce.c:152`

Ob ein Client auf Level 1 hängt, wird hier über die **IP** entschieden. Wegen
NAPT kommt ein Level-2-Client beim Server aber mit der IP seines
Level-1-Parents an (dieselbe Ursache wie `rssi=127` im Stats-Log). Seine IP
steht damit in der Level-1-Liste, er wird übersprungen und spielt während der
Durchsage weiter Musik. Genau das darf laut Anforderung „keinesfalls“ passieren.

Die Verteilerliste selbst ist korrekt, denn sie entsteht über den MAC-Abgleich
in `snapserver_get_level1_client_ips()`. Falsch ist nur die Mute-Prüfung.

**Fix:** Auch hier über die MAC entscheiden (`level1_match_rssi()` in
`snapserver.c`). Am saubersten löst das der Umbau A unten.

### K2 — Clients verwerfen jede weitere Durchsage
**Ort:** `voice_client_task()`, `voice_announce.c:451`

`last_sequence` und `have_sequence` werden nie zurückgesetzt. App und
`voice_test.py` beginnen aber jede Durchsage bei Sequenz 0. Nach einer
30-s-Durchsage (3000 Pakete) gelten bei der nächsten die ersten ~30 s als
veraltet und werden verworfen. Weil die übrigen Clients gleichzeitig stumm
sind, ist dann nirgends etwas zu hören. Das zeigt sich schon beim zweiten
Lauf von `voice_test.py` in Stufe 2.

**Fix:** Den Sequenz-Zustand zurücksetzen, wenn seit dem letzten Paket mehr
als `VOICE_INACTIVITY_TIMEOUT_US` (400 ms) vergangen sind. Zusätzlich einen
großen Rückwärtssprung (z. B. mehr als 1000 Pakete) als neue Sitzung werten.

### K3 — 3,4 KB Array auf dem 6-KB-Stack von `ctrl_conn_task`
**Ort:** `mute_non_level1_clients()`, `voice_announce.c:147`

`snapserver_client_info_t clients[SNAPSERVER_MAX_CLIENTS]` braucht 10 × 344 B,
also etwa 3,4 KB Stack. Aufgerufen wird die Funktion tief in
`process_line → handle_request_object → build_result →
voice_announce_rpc_start`, darunter folgen noch
`snapserver_set_client_volume → send_server_settings → send_all` samt
`ESP_LOGI`. `CTRL_CONN_STACK` ist 6144 B.

`snapcontrol.c:62-69` beschreibt, dass genau dieses Array aus genau diesem
Grund schon einmal vom Stack ins PSRAM verlegt wurde. Dazu kommt die Lehre
aus dem `snapsend3`-Überlauf: Der Pfad läuft erst bei der ersten Durchsage
mit mehreren Clients überhaupt an.

**Fix:** Das Array per `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` holen oder
statisch anlegen. Statisch geht, sobald Start und Stop wie in H2 serialisiert
sind.

### K4 — Bei Durchsagen über ca. 4 s kommt die Musik auf Level-1 nicht nahtlos zurück
**Ort:** Zusammenspiel von `player_task` (VOICE-Zweig) und
`audio_sink_feed_network()` in `audio_sink.c`

Die Architekturentscheidung setzt darauf, dass der Ring während der Durchsage
weiterläuft und danach sofort aktuelles Audio bereitsteht. Das stimmt nur für
kurze Durchsagen:

1. Im Normalbetrieb hält der Ring etwa `buffer_ms` (3 s); seine Kapazität ist
   `2 × buffer_ms` (6 s). Solange VOICE aktiv ist, liest der Player nichts.
   Nach ~3 s ist der Ring voll, und `ring_write()` schneidet neue Chunks ab.
2. `fill` bleibt dabei konstant, während die Chunk-Zeitstempel weiterlaufen.
   Nach einer weiteren Sekunde gilt `gap_us > STREAM_RESTART_US`: Der Ring
   wird geleert und `s_network_ready = false` gesetzt (`audio_sink.c:924-931`).
3. Das wiederholt sich zyklisch. Wie es beim Ende der Durchsage aussieht,
   hängt vom Zeitpunkt ab:
   - Ist der Ring unter 80 % von `buffer_ms` gefüllt, folgt bis zu **2,4 s
     Stille** durch den Prebuffer.
   - Sonst stehen bis zu 6 s Rückstand im Ring, und der Scheduler springt per
     Hard-Resync nach vorn.

Level-2-Clients betrifft das nicht, weil sie gemutet weiterlaufen. Level-1-
und Level-2-Lautsprecher setzen danach also auch unterschiedlich wieder ein.
Übliche Durchsagen (5–30 s) liegen alle in diesem Bereich.

**Fix:** Siehe Umbau B.

---

## Hoch

### H1 — Clients, die während einer Durchsage (neu) verbinden, sind nicht stumm
**Ort:** Die Mute-Liste entsteht nur einmal in `voice_announce_rpc_start()`;
`snapserver.c:1675` setzt `muted = false` für jeden neuen Slot.

Im dynamischen Mesh ist ein Reconnect während einer 30-s-Durchsage
realistisch. Ein Level-2-Client, der sich neu verbindet, bekommt
`muted=false` und spielt Musik. Ebenso ein Client, der mitten in der
Durchsage von Level 1 auf Level 2 rutscht: Er fällt aus der Verteilerliste
(der Re-Poll erkennt das), wird aber nie gemutet.

**Fix:** Umbau A. Der Server entscheidet dann bei **jeder**
`ServerSettings`-Nachricht, auch beim Handshake.

### H2 — Start/Stop-Races
**Ort:** `voice_stop_internal()` (`:176`) und `voice_announce_rpc_start()`
(`:227`)

Beide Funktionen setzen bzw. löschen `s_owner_fd` zuerst und arbeiten danach
ohne Schutz auf `s_muted_clients`/`s_muted_count`. Das Wiederherstellen
blockiert pro Client bis zu `SEND_MUTEX_WAIT_MS` (1 s).

- **Stop, dann sofort Start:** Der neue Start liest die noch gemuteten
  Clients als `muted=true`, speichert das als „Originalzustand“ und
  überschreibt die Liste, die gerade wiederhergestellt wird. Diese Clients
  bleiben nach der nächsten Durchsage **dauerhaft stumm**.
- **Stop (von einer anderen Verbindung) während eines Starts:** Das Restore
  läuft über eine halb gefüllte Liste, danach mutet der Start den Rest und
  setzt `audio_i2s_set_voice_active(true)` sowie die LED. `s_owner_fd` ist
  dann aber `-1`. Ergebnis: Der Server-Lautsprecher bleibt stumm, weil der
  Voice-Override aktiv ist, aber keine Pakete mehr angenommen werden. Clients
  bleiben gemutet, die LED pulsiert — bis zum nächsten Start/Stop.

**Fix:** Ein FreeRTOS-Mutex (kein Spinlock, es wird blockiert gesendet), der
Start und Stop jeweils komplett umschließt. Mit Umbau A entfällt ohnehin der
gespeicherte Zustand, der hier überschrieben wird.

### H3 — Level-1-Clients spielen Musik, bis das erste Sprachpaket ankommt
**Ort:** Architektur; Level-1 wird nur über ankommende Pakete umgeschaltet.

Der Plan nahm für dieses Fenster „typischerweise << 100 ms“ an. Das ist zu
optimistisch: Die App meldet sich zuerst beim Server an und öffnet erst
danach das Mikrofon, und `VOICE_COMMUNICATION` braucht auf manchen Geräten
mehrere hundert ms. Deshalb gibt es `VOICE_START_GRACE_MS = 3000`. So lange
laufen Level-1-Lautsprecher mit Musik weiter, während Level-2 schon stumm ist.

**Fix:** Der Server schickt ab `Voice.Start` sofort Stille-Pakete an die
Level-1-Clients, bis echtes Audio vom Handy kommt. Die Clients wechseln dann
sofort auf VOICE. Das passt zu Umbau C.

---

## Mittel

### M1 — Race beim Nachstellen des Watchdogs
**Ort:** `voice_server_task()`, `voice_announce.c:305`

Der Task prüft unter Lock, ob eine Durchsage aktiv ist, und stellt den Timer
danach ungeschützt neu (`esp_timer_stop` + `start_once`). Fällt ein Stop
genau dazwischen, bleibt der Timer mit 1 s gestellt. Ein schneller neuer
Start ruft `esp_timer_start_once` auf einen laufenden Timer auf; der Fehler
wird ignoriert, die 3-s-Anlauffrist greift nicht, und die Durchsage wird
nach ~1 s beendet, bevor das Handy-Mikrofon läuft.

Nebenbei: 100 Timer-Stops/-Starts pro Sekunde sind unnötige Last.

**Fix:** Umbau C. Nur noch Zeitstempel (`s_last_packet_us`, `s_started_us`)
schreiben und periodisch prüfen, statt pro Paket den Timer neu zu stellen.

### M2 — Stop-Arbeit im esp_timer-Task
**Ort:** `silence_watchdog_cb()`/`max_duration_cb()`, `voice_announce.c:213`

Das ist bereits im Code kommentiert, aber nicht behoben. Das Restore
blockiert den gemeinsamen esp_timer-Task bis zu 1 s pro Client. Außerdem ist
nicht geprüft, ob `CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584` für
`send_server_settings → client_send_msg → send_all` plus zwei `ESP_LOG*`
reicht. Es ist wieder ein Pfad, der erst im Fehlerfall läuft.

**Fix:** Umbau C. Der Stop läuft dann im `voice_server_task` (PSRAM-Stack,
blockieren erlaubt).

### M3 — Mailbox ohne Vorpuffer, verwirft bei Überlauf die neuesten Samples
**Ort:** `audio_sink_feed_voice()` (`audio_sink.c:869`),
`audio_i2s_feed_voice()` (`audio_i2s.c:667`)

- **Kein Vorpuffer:** Pakete kommen alle 10 ms mit 480 Samples, der Player
  nimmt alle 20 ms 960. Kommt ein Paket etwas zu spät, wird die Hälfte des
  Frames mit Nullen gefüllt (10 ms Lücke, hörbares Knacken). Die Verspätung
  bleibt danach als +10 ms Latenz stehen, bis die 40 ms voll sind. Bei
  WLAN-Jitter im Mesh wird das regelmäßig passieren.
- **Überlauf verwirft Neues:** Ist die Mailbox voll, werden die **neuen**
  Samples verworfen. Das widerspricht „das Frischeste spielen“: Die Latenz
  klebt bei 40 ms, und jeder WLAN-Burst reißt Lücken ins neueste Audio.
  Taktdrift zwischen Handy und ESP (±50 ppm) führt zusätzlich zu einem
  Verlust bzw. Leerlauf etwa alle 200 s.

**Fix:** Erst ausgeben, wenn ~20 ms im Puffer liegen. Bei Überlauf das
Älteste verwerfen. Ob die Kapazität reicht (evtl. 60 ms), entscheidet der
Hörtest in Stufe 3.

### M4 — Lautstärke-/Mute-Änderungen während einer Durchsage gehen verloren
**Ort:** `mute_non_level1_clients()` / `restore_muted_clients()`

Die Durchsage überschreibt das `muted`-Feld, das auch die Steuer-App sieht.
Snapdroid und Co. zeigen die Lautsprecher währenddessen als stumm an, und
eine Lautstärkeänderung während der Durchsage wird beim Restore mit dem
alten Stand überschrieben.

**Fix:** Umbau A.

### M5 — App-Timeout von 1,5 s gegen synchrones Muten auf dem Server
**Ort:** `VoiceAnnounceService.kt:166` (`CONNECT_TIMEOUT_MS` gilt auch als
`soTimeout`)

`Voice.Start` antwortet erst, nachdem alle Nicht-Level-1-Clients ihre
`ServerSettings` bekommen haben. Das dauert bis zu 1 s pro Client, wenn
einer hängt. Die App meldet dann „Server antwortet nicht“ und schließt die
Verbindung. Der Server beendet die gerade gestartete Durchsage daraufhin über
den Disconnect-Hook wieder. Das Ergebnis ist konsistent, aber die Durchsage
schlägt ohne echten Grund fehl.

**Fix:** In der App ein längeres Lese-Timeout für `Voice.Start` (z. B. 5 s),
der Connect-Timeout bleibt kurz. Mit Umbau A muss der Server gar nicht mehr
synchron in `Voice.Start` muten.

---

## Niedrig

### N1 — LED bleibt im Durchsage-Puls hängen
**Ort:** `player_task`, `audio_sink.c:753`

Beim Wechsel VOICE → NONE (Netz während der Durchsage weg) setzt der Player
keinen LED-Zustand, bis `snapclient.c` irgendwann `NO_SERVER` oder `PLAYING`
setzt. Umgekehrt setzt ein Reconnect während der Durchsage `PLAYING`, und
der Puls verschwindet. Beides ist nur kosmetisch.

### N2 — UDP 1706 nimmt Audio von jedem Absender an
Während einer aktiven Durchsage verteilt der Server Pakete von **jedem**
Absender. Port 1705 hat ohnehin keine Authentifizierung (vorbestehend).

**Fix, billig:** Nur Pakete von der Peer-IP der Owner-Verbindung annehmen
(`getpeername(s_owner_fd)`). Die Prüfung stimmt auch hinter NAPT, weil TCP
und UDP dort dieselbe Quell-IP haben.

### N3 — Ablehnung wird als „Method not found“ gemeldet
**Ort:** `snapcontrol.c:471`, `VoiceAnnounceService.kt:178`

„Es läuft bereits eine Durchsage“ kommt als `-32601 Method not found` zurück.
Das ist für andere Controller irreführend. Die App wertet außerdem **jeden**
Fehler als „bereits aktiv“, auch den echten `-32601` einer Firmware ohne
Voice-Unterstützung.

**Fix:** Eigener Code (z. B. `-32000`, „announcement already active“), und
die App unterscheidet die beiden Fälle.

---

## Empfohlene Umbauten

### A — Durchsage-Mute als Serverzustand statt Überschreiben des Client-Mutes
*Löst K1, H1, M4, M5 und den gespeicherten Zustand hinter H2.*

- In `snapserver.c` ein Flag `s_announcement_active`.
- `send_server_settings()` sendet
  `muted = client->muted || (s_announcement_active && !is_level1(client->mac))`.
  `is_level1()` ist `level1_match_rssi()`, also derselbe MAC-Test wie für
  die Verteilerliste.
- `snapserver_set_announcement(bool)` setzt das Flag und schickt allen
  Clients neue `ServerSettings`. Beim Stop wird einfach neu gesendet; es gibt
  nichts zu speichern und nichts wiederherzustellen.
- Der Handshake neuer Clients nutzt automatisch dieselbe Regel (H1). Der
  Re-Poll im `voice_server_task` ruft zusätzlich einmal pro Sekunde
  `snapserver_set_announcement(true)` auf, damit Clients, die die Ebene
  wechseln, erfasst werden. Das sollte nur senden, wenn sich der effektive
  Mute-Zustand eines Clients geändert hat.
- `client->muted` bleibt der Wert aus der Steuer-App (M4).

### B — VOICE als Overlay über der weiterlaufenden Musik-Timeline
*Löst K4.*

Solange VOICE aktiv ist und die Basisquelle NETWORK wäre, ruft der Player
weiter `render_network_frame()` in einen Wegwerf-Puffer auf und gibt nur
stattdessen die Stimme aus. Die Musik läuft also still weiter, genau wie auf
den gemuteten Level-2-Clients. Dadurch bleiben Ring-Füllstand, Timeline und
Regelung unverändert; nach der Durchsage geht es ohne Prebuffer und synchron
mit Level 2 weiter.

Dazu `decide_source()` in „Basisquelle“ und „Voice-Override“ aufteilen. Der
Sonderfall „kein Flush beim Wechsel zu VOICE“ bleibt dabei harmlos.

### C — `voice_server_task` mit Empfangs-Timeout als einziger Zustandsbesitzer
*Löst H3, M1, M2 und nimmt den Timern die Arbeit ab.*

- `recvfrom()` mit `SO_RCVTIMEO` von ~10 ms.
- Bei jedem Durchlauf, mit Paket oder Timeout:
  - Watchdogs über Zeitstempel prüfen (Anlauffrist, 1 s Stille, 3 min Maximum).
  - Solange aktiv und noch nichts vom Handy kam, Stille-Pakete an Level 1
    schicken (H3).
  - Einen angeforderten Stop hier ausführen. RPC und Disconnect-Hook setzen
    nur noch ein Flag (per Task-Notify oder Lock); die eigentliche Arbeit
    läuft hier mit PSRAM-Stack (M2).
- Die esp_timer-Objekte entfallen.

## Reihenfolge

1. **Sofort, jeweils wenige Zeilen:** K2, K3, K1. Ohne K2 scheitert Stufe 2
   schon beim zweiten Testlauf.
2. Umbau A zusammen mit dem Mutex aus H2.
3. Umbau B, vor den Hörtests der Stufe 3, denn K4 verfälscht sonst genau den
   Test, auf dem die Architektur beruht.
4. Umbau C.
5. M3 nach dem ersten Hörtest; N1–N3 bei Gelegenheit.

## Auf Hardware zusätzlich prüfen

- **Interner Heap:** `min_ever` während einer Durchsage mit vielen Clients.
  UDP-pbufs liegen im internen DRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`
  ist aus), dazu kommen N × 100 Pakete/s obendrauf.
- **Empfangsqueue des Servers:** `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` bei
  WLAN-Bursts vom Handy. Verworfene Pakete sind gewollt, aber die Rate ist
  interessant.
- **Server-Lautsprecher:** Er wird erst nach dem Fan-out gefüttert. Wenn
  `sendto()` langsam wird, lohnt es sich, die Reihenfolge zu tauschen.

## Android-App

Abgesehen von M5 und N3 keine Befunde. Es gilt aber: Die App ist ungebaut,
weil hier kein SDK installiert ist. Beim ersten Öffnen in Android Studio mit
Build-Fehlern durch Versionsstände rechnen (AGP 8.5 / Kotlin 1.9 / BOM
2024.06); die angebotenen Updates annehmen.

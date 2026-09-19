/**
 * @file voice_announce.h
 * @brief Low-latency voice announcements ("Durchsagen") over a dedicated UDP
 *        channel, separate from the buffered Snapcast stream on port 1704.
 *
 * The music path exists to survive a dynamic, multi-hop mesh: a large
 * buffer (bufferMs, ~3 s) absorbs rearrangements and jitter invisibly. An
 * announcement wants the opposite trade-off -- latency over smoothness, and
 * a lost/late packet dropped rather than waited for -- so it gets its own
 * path end to end instead of an option bolted onto the Snapcast one:
 *
 *   - UDP, not TCP: a stuck send must never hold up the next packet.
 *   - Raw 48 kHz/16-bit mono PCM, not Opus: no encode/decode latency, and
 *     bandwidth was never the scarce resource here.
 *   - 480-sample (10 ms) packets, not the pipeline's usual 960 (20 ms):
 *     960 samples of PCM plus a header exceeds the 1500 B MTU, and IP
 *     fragmentation would turn one lost fragment into a lost whole chunk --
 *     actively harmful for a "drop, don't wait" channel.
 *   - Delivered only to level-1 clients (direct children of the root's own
 *     AP): not because a hop costs much transit time, but because a hop can
 *     stall for seconds during a mesh rearrangement, which music's buffer
 *     absorbs invisibly and an unbuffered stream cannot absorb at all.
 *
 * Clients that do not receive the announcement (level 2+) must not keep
 * playing music through it -- that would clash acoustically with the
 * announcement in the same room. They are muted instead, through the
 * ServerSettings "muted" field, which already reaches every client at every
 * hop depth reliably (snapserver_set_announcement() in snapserver.h). That
 * mute is added on top of the control app's own setting rather than written
 * into it, so there is nothing to restore afterwards. Level-1 clients are
 * not muted this way -- their output gain applies to every source, the
 * announcement included -- they switch to the announcement locally instead
 * (audio_sink.c). Until the phone's microphone delivers, the server sends
 * them silence, so their music stops at the same moment as everyone else's.
 *
 * Start/stop is signalled over the existing JSON-RPC control channel (port
 * 1705, snapcontrol.c) as two new methods, Voice.Start/Voice.Stop -- not
 * over the UDP channel itself, since losing a start/stop message must not
 * happen the way losing a PCM packet may. Everything that follows from a
 * start or stop -- pushing the mute, finding the level-1 targets, the
 * watchdogs (1 s without audio, 3 s for the first packet, 3 min overall) --
 * runs in the server's own UDP task, never in the JSON-RPC handler.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_ANNOUNCE_PORT 1706

/* Starts the UDP relay + mute bookkeeping. Server role only. Idempotent. */
esp_err_t voice_announce_start(void);

/* Starts the UDP receiver that feeds audio_sink_feed_voice(). Client role
 * only (level-1 clients act on what arrives; level-2+ simply never receive
 * anything, see the header comment above -- both run the same code).
 * Idempotent. */
esp_err_t voice_receive_start(void);

/*
 * JSON-RPC glue for snapcontrol.c's Voice.Start/Voice.Stop. fd identifies
 * the control connection, for ownership (only one announcement at a time)
 * and for voice_announce_on_control_disconnect() below.
 *
 * voice_announce_rpc_start() returns false if another connection already
 * owns an active announcement; snapcontrol.c reports that as a normal
 * result with busy=true, so a client can tell it apart from a firmware that
 * has no Voice.Start at all ("method not found"). Only UDP audio from the
 * owning connection's address is relayed.
 * voice_announce_rpc_stop() always succeeds -- any connection may stop an
 * announcement, since a stuck one is worse than an unauthorized stop.
 */
bool voice_announce_rpc_start(int fd);
bool voice_announce_rpc_stop(int fd);

/*
 * Called from ctrl_conn_task()'s single exit point, right before it closes
 * fd. If fd owns the active announcement, stops it -- the app losing its
 * connection (killed, backgrounded aggressively, network gone) must not
 * leave clients muted and speakers silent indefinitely. A no-op for any fd
 * that doesn't own the current announcement, including when there isn't one.
 */
void voice_announce_on_control_disconnect(int fd);

#ifdef __cplusplus
}
#endif

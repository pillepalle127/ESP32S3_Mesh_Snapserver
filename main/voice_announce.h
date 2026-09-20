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
 *   - Opus at speech bitrates (~25 kbit/s, 16 kHz wideband as the phone
 *     sends it). Raw PCM, which this started with, needed about nine times
 *     the music's airtime per client and congested the shared channel.
 *   - Delivered to the server's own speaker, to its direct clients, and one
 *     hop further (VOICE_RELAY_HOPS in voice_announce.c): each client
 *     forwards to its own children, which the server cannot address itself
 *     because they sit behind their parent's NAPT. The depth is limited
 *     because every hop inherits the tail risk of a rearranging mesh -- a
 *     relay can stall for seconds, which music's buffer absorbs invisibly
 *     and an unbuffered stream cannot absorb at all.
 *
 * No client may keep playing music through an announcement -- that would
 * clash acoustically with it in the same room. Which clients actually hear
 * the announcement depends on the mesh tree, which the server cannot see
 * past the first level, so it does not try: ServerSettings carries a flag
 * of its own ("announcement", snapserver_set_announcement() in
 * snapserver.h), and each of our clients silences its music for as long as
 * the flag is set, playing the announcement if it reaches them
 * (audio_sink.c). The listener's own volume and mute keep their meaning and
 * apply to the announcement as well. A foreign Snapcast client knows no
 * such flag and is muted outright instead; it never receives an
 * announcement anyway. Until the phone's microphone delivers, the server
 * sends silence, so music stops everywhere at the same moment.
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

/* Starts the UDP receiver that feeds audio_sink_feed_voice() and forwards
 * to this node's own children while the packet's hop count allows it.
 * Client role only. Idempotent. */
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

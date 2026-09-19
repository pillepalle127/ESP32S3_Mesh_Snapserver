#!/usr/bin/env python3
"""
Stands in for the Android app while testing voice announcements (stages 1-3
of the voice plan): arms the server over JSON-RPC (TCP 1705), streams a test
tone or a WAV file as raw PCM datagrams (UDP 1706), then disarms.

Wire format, identical to voice_packet_t in main/voice_announce.c: one
datagram per 10 ms, uint32 little-endian sequence number followed by 480
int16 little-endian samples, 48 kHz mono -- 964 bytes.

Examples (laptop joined to the mesh SSID, ideally on the root's own AP):
  # stage 1/3: through the server, which relays to level-1 clients
  python3 tools/voice_test.py --host 192.168.5.1 --seconds 10

  # stage 2: straight at one client, bypassing the server's fan-out
  python3 tools/voice_test.py --host 192.168.5.2 --direct --seconds 10

  # speech instead of a tone (48 kHz, 16-bit, mono WAV)
  python3 tools/voice_test.py --wav durchsage.wav

Stdlib only, no dependencies.
"""
import argparse
import json
import math
import socket
import struct
import sys
import time
import wave

CONTROL_PORT = 1705
VOICE_PORT = 1706
SAMPLE_RATE = 48000
FRAME_SAMPLES = 480  # 10 ms


def rpc_call(sock_file, sock, method, request_id):
    request = {"jsonrpc": "2.0", "id": request_id, "method": method}
    sock.sendall((json.dumps(request) + "\r\n").encode())
    # Skip unsolicited Server.OnUpdate notifications, which carry no id.
    while True:
        line = sock_file.readline()
        if not line:
            raise ConnectionError(f"connection closed while waiting for {method}")
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        if msg.get("id") is not None:
            return msg


def tone_frames(freq, seconds, level):
    amplitude = int(32767 * level)
    total = int(seconds * SAMPLE_RATE)
    n = 0
    while n < total:
        count = min(FRAME_SAMPLES, total - n)
        frame = [int(amplitude * math.sin(2 * math.pi * freq * (n + i) / SAMPLE_RATE))
                 for i in range(count)]
        frame += [0] * (FRAME_SAMPLES - count)
        yield frame
        n += count


def wav_frames(path):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels()) != (SAMPLE_RATE, 2, 1):
            sys.exit(f"{path}: need 48000 Hz, 16-bit, mono "
                     f"(got {w.getframerate()} Hz, {8 * w.getsampwidth()}-bit, "
                     f"{w.getnchannels()} ch)")
        while True:
            raw = w.readframes(FRAME_SAMPLES)
            if not raw:
                return
            frame = list(struct.unpack(f"<{len(raw) // 2}h", raw))
            frame += [0] * (FRAME_SAMPLES - len(frame))
            yield frame


def stream(host, frames, drop_every):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sequence = 0
    sent = dropped = 0
    next_due = time.monotonic()
    for frame in frames:
        packet = struct.pack(f"<I{FRAME_SAMPLES}h", sequence & 0xFFFFFFFF, *frame)
        sequence += 1
        if drop_every and sequence % drop_every == 0:
            dropped += 1  # simulated loss, to see "drop, don't stall" work
        else:
            udp.sendto(packet, (host, VOICE_PORT))
            sent += 1
        # Paced against a monotonic schedule, not sleep(0.01) per packet,
        # so the average rate stays exactly 100 packets/s.
        next_due += FRAME_SAMPLES / SAMPLE_RATE
        delay = next_due - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    udp.close()
    print(f"sent {sent} packets, dropped {dropped} on purpose")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="192.168.5.1")
    parser.add_argument("--direct", action="store_true",
                        help="UDP only, no Voice.Start/Stop (target a client directly)")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--freq", type=float, default=440.0)
    parser.add_argument("--level", type=float, default=0.3, help="tone amplitude 0..1")
    parser.add_argument("--wav", help="48 kHz 16-bit mono WAV instead of a tone")
    parser.add_argument("--drop-every", type=int, default=0,
                        help="skip every Nth packet to simulate loss")
    args = parser.parse_args()

    frames = wav_frames(args.wav) if args.wav else tone_frames(args.freq, args.seconds, args.level)

    if args.direct:
        stream(args.host, frames, args.drop_every)
        return

    ctrl = socket.create_connection((args.host, CONTROL_PORT), timeout=5.0)
    ctrl_file = ctrl.makefile("r", encoding="utf-8", newline="\n")
    try:
        reply = rpc_call(ctrl_file, ctrl, "Voice.Start", 1)
        result = reply.get("result") or {}
        if "error" in reply:
            sys.exit(f"Voice.Start unknown to this server: {reply['error']} "
                     "(firmware without announcements?)")
        if not result.get("active"):
            # No Voice.Stop here: any connection may stop, so sending one
            # would end the announcement that refused us.
            sys.exit(f"Voice.Start refused: {result} (another announcement active)")
        print("armed:", result)
        try:
            stream(args.host, frames, args.drop_every)
        finally:
            try:
                print("disarmed:", rpc_call(ctrl_file, ctrl, "Voice.Stop", 2).get("result"))
            except (OSError, ConnectionError, ValueError) as err:
                print("Voice.Stop failed:", err)
    finally:
        ctrl.close()


if __name__ == "__main__":
    main()

"""A UDP relay between two Slippi players that plays an internet connection:
the same one-way delay, jitter and loss both ways. Each player is given
(by the fake matchmaking server) an address the relay holds for its
opponent; the relay takes each packet there and passes it on, late, to the
other player's real address, from the address that player expects.

    python tools/sweep/net_relay.py A_ADV A_REAL B_ADV B_REAL [--delay MS]
        [--jitter MS] [--loss PCT] [--seed N]

A_ADV, B_ADV: host:port the relay binds (where each player is reached);
A_REAL, B_REAL: where each player really is. A packet that reaches A_ADV is
for A: it leaves from B_ADV (A knows B there) to A_REAL; and the other way.
Order is kept in each direction (jitter delays, it does not reorder), as on
most real paths. Stats every 10 s and at the end, on stdout, with how much
later B's input for each frame reaches the relay than A's (Slippi's pad
messages, read out of the ENet datagrams): B's lateness as A feels it.

--passes FILE records every datagram that carries a pad, a line each: "S A 12.345678 1234"
when A's (its newest pad's frame) reaches the relay, "D A ..." when one is handed to A.
A player sends its pad once for each frame it runs, so this is each player's frames by the
clock and the opponent's newest input it had at each: what match_quality.py scores.

--spikes MS,EVERY_S holds a direction up now and then, as home connections do (Wi-Fi, a busy link):
on average every EVERY_S seconds in each direction (at random), nothing passes for 0.5..1x MS; what was
held then arrives at once, in order.

--suggest A|B|both plays input suggestions (Slippi 3.7.0, message 0x87) to that player, as an opponent
on a GameCube adapter sends them: when the opponent's input changes, a suggestion with it reaches the
player 1-15 ms (--suggest-lead) before the pad of the frame it first applies to, on the same connection
(ENet unsequenced, its group numbers rewritten in order with the real ones), lost as often as anything.
The bots' inputs change on a frame and not between, so each change is one suggestion of the frame's own
pad: a person's controller between frames, read early. --suggest-share sends only that share of them.
"""
import argparse
import ctypes
import os
import sys
import heapq
import random
import select
import socket
import statistics
import struct
import time

# ENet commands (protocol.h): type -> (size before the data, where the data's
# length is), and Slippi's pad message (its newest frame follows the type)
ENET = {1: (8, None), 2: (48, None), 3: (44, None), 4: (8, None), 5: (4, None), 6: (6, 4), 7: (8, 6),
        8: (24, 6), 9: (8, 6), 10: (12, None), 11: (16, None), 12: (24, 6)}
SLIPPI_PAD, SLIPPI_PAD_ACK = 0x80, 0x81
SLIPPI_SUGGESTION = 0x87
PAD_WIRE, SUGGEST_PAD = 9, 8  # (3.7.0) a pad on the wire; a suggestion's inputs, a pad's first 8 bytes


def commands(datagram):
    """[(position, command type, size before the data, data length)] of an uncompressed ENet datagram."""
    if len(datagram) < 2 or datagram[0] & 0x40:
        return []
    pos, out = 4 if datagram[0] & 0x80 else 2, []
    while pos + 4 <= len(datagram):
        cmd = datagram[pos] & 0x0F
        if cmd not in ENET:
            break
        size, length_at = ENET[cmd]
        n = 0
        if length_at is not None:
            if pos + length_at + 2 > len(datagram):
                break
            n = struct.unpack('>H', datagram[pos + length_at:pos + length_at + 2])[0]
        out.append((pos, cmd, size, n))
        pos += size + n
    return out


def pad_messages(datagram):
    """[(command byte, channel, peer header, newest frame, player, [pad per frame, newest first])] of the
    Slippi pad messages in an ENet datagram."""
    out = []
    for pos, cmd, size, n in commands(datagram):
        body = datagram[pos + size:pos + size + n]
        if cmd in (7, 9) and len(body) >= 14 and body[0] == SLIPPI_PAD:
            frame, player = struct.unpack('>iB', body[1:6])
            pads = [body[14 + i:14 + i + PAD_WIRE] for i in range(0, len(body) - 14 - PAD_WIRE + 1, PAD_WIRE)]
            out.append((datagram[pos], datagram[pos + 1], struct.unpack('>H', datagram[:2])[0], frame, player, pads))
    return out


def suggestion_datagram(peer_header, channel, player, target, seq, pad):
    """An ENet datagram with one Slippi input suggestion, unsequenced (its group set when it is sent)."""
    body = struct.pack('>BBiI', SLIPPI_SUGGESTION, player, target, seq) + bytes(pad[:SUGGEST_PAD])
    header = peer_header & ~0xC000  # (no sent time, not compressed)
    return struct.pack('>HBBHHH', header, 0x40 | 9, channel, 0, 0, len(body)) + body


def renumber_unsequenced(datagram, counters):
    """The datagram with each unsequenced command's group the next of its connection's (counters: peer
    header -> next group), so that suggestions put between keep every group new and in order."""
    cmds = [c for c in commands(datagram) if c[1] == 9]
    if not cmds:
        return datagram
    out = bytearray(datagram)
    key = struct.unpack('>H', datagram[:2])[0] & 0x3FFF
    for pos, _, _, _ in cmds:
        if key not in counters:
            counters[key] = struct.unpack('>H', datagram[pos + 4:pos + 6])[0]
        out[pos + 4:pos + 6] = struct.pack('>H', counters[key] & 0xFFFF)
        counters[key] = (counters[key] + 1) & 0xFFFF
    return bytes(out)


def addr(s):
    host, port = s.rsplit(':', 1)
    return host, int(port)


def slippi_frames(datagram, kind=SLIPPI_PAD):
    """The frame of each Slippi message of this kind (a pad's newest, an ack's) in
    an ENet datagram."""
    if len(datagram) < 2 or datagram[0] & 0x40:  # compressed: not read
        return []
    pos, out = 4 if datagram[0] & 0x80 else 2, []  # the sent time is there or not
    while pos + 4 <= len(datagram):
        cmd = datagram[pos] & 0x0F
        if cmd not in ENET:
            break
        size, length_at = ENET[cmd]
        n = 0
        if length_at is not None:
            if pos + length_at + 2 > len(datagram):
                break
            n = struct.unpack('>H', datagram[pos + length_at:pos + length_at + 2])[0]
            body = datagram[pos + size:pos + size + n]
            if cmd in (6, 7, 9) and len(body) >= 5 and body[0] == kind:
                out.append(struct.unpack('>i', body[1:5])[0])
        pos += size + n
    return out


class Direction:
    """Packets arriving on `inbound`, sent from `outbound` to `dest`."""

    def __init__(self, name, inbound, outbound, dest):
        self.name, self.inbound, self.outbound, self.dest = name, inbound, outbound, dest
        self.last_due = 0.0
        self.sent = self.dropped = self.peak = self.send_errors = 0
        self.delays = []
        self.first = {}  # frame -> when its pad first came this way
        self.handed = {}  # frame -> when its pad was handed to the player (the delay done)
        self.turnaround = []  # from that to the player's ack reaching the relay, ms
        self.suggest = False  # (--suggest) suggestions played to this player
        self.groups = {}  # peer header -> the next unsequenced group (when suggesting)
        self.sug_head = {}  # opponent's player index -> newest frame whose pad was looked at
        self.sug_last = {}  # opponent's player index -> the inputs it last had a suggestion or a pad for
        self.sug_seq = 0
        self.suggested = self.sug_skipped = 0
        self.spike_until = self.spike_next = 0.0  # (--spikes) held up until; the next one starts
        self.spikes = 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    for n in ('a_adv', 'a_real', 'b_adv', 'b_real'):
        ap.add_argument(n, type=addr)
    ap.add_argument('--delay', type=float, default=0, help='one-way delay, ms')
    ap.add_argument('--jitter', type=float, default=0, help='extra delay, uniform 0..MS')
    ap.add_argument('--loss', type=float, default=0, help='packets dropped, percent')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--trace', help="each frame's input times, a line a frame: frame, from A, from B (seconds)")
    ap.add_argument('--passes', help='every datagram with a pad, a line each: sent by or delivered to, time, newest frame')
    ap.add_argument('--spikes', help='MS,EVERY_S: now and then a direction is held up for 0.5..1x MS')
    ap.add_argument('--suggest', choices=['A', 'B', 'both'], help='play input suggestions to this player')
    ap.add_argument('--suggest-lead', default='1,15', help='how much earlier than the pad, ms: MIN,MAX (uniform)')
    ap.add_argument('--suggest-share', type=float, default=1.0, help='the share of input changes suggested')
    # (SLIPPWII_RELAY_ARGS: options given to every relay a harness starts, an older harness's too)
    args = ap.parse_args(sys.argv[1:] + os.environ.get('SLIPPWII_RELAY_ARGS', '').split())
    if hasattr(ctypes, 'windll'):  # Windows' timer ticks every 15.6 ms otherwise: delays would be late
        ctypes.windll.winmm.timeBeginPeriod(1)
    rng = random.Random(args.seed)
    sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sa.bind(args.a_adv)
    sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sb.bind(args.b_adv)
    for s in (sa, sb):
        if hasattr(socket, 'SIO_UDP_CONNRESET'):  # Windows: no error from an earlier send's ICMP
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
    to_a = Direction('to A', sa, sb, args.a_real)
    to_b = Direction('to B', sb, sa, args.b_real)
    to_a.suggest = args.suggest in ('A', 'both')
    to_b.suggest = args.suggest in ('B', 'both')
    lead_min, lead_max = (float(x) / 1000 for x in args.suggest_lead.split(','))
    sug_rng = random.Random(args.seed + 1000)  # (its own: the connection's own draws stay as without)
    spike_rng = random.Random(args.seed + 2000)
    spike_ms, spike_every = (float(x) for x in args.spikes.split(',')) if args.spikes else (0, 0)
    for d in (to_a, to_b):
        d.spike_next = time.perf_counter() + spike_rng.expovariate(1 / spike_every) if spike_every else float('inf')
    by_sock = {sa: to_a, sb: to_b}
    queue, seq = [], 0
    print(f'relay: A {args.a_adv} -> {args.a_real}, B {args.b_adv} -> {args.b_real}; delay {args.delay} ms, '
          f'jitter {args.jitter} ms, loss {args.loss}%' + (f'; suggestions to {args.suggest}' if args.suggest else '')
          + (f'; spikes up to {spike_ms:.0f} ms every {spike_every:.0f} s or so' if spike_every else ''), flush=True)
    next_stats = time.perf_counter() + 10
    lateness, lateness_all = [], []
    trace = open(args.trace, 'w', buffering=1) if args.trace else None
    passes = open(args.passes, 'w') if args.passes else None

    def stats(final=False):
        for d in (to_a, to_b):
            avg = sum(d.delays) / len(d.delays) if d.delays else 0
            print(f'relay: {d.name}: {d.sent} sent, {d.dropped} dropped, average delay {avg:.1f} ms, '
                  f'most queued {d.peak}' + (f', {d.suggested} suggestions played ({d.sug_skipped} changes not)' if d.suggest else '')
                  + (f', {d.spikes} spikes' if spike_every else ''),
                  flush=True)
            d.delays = []
        # each frame's input: from B (arriving for A) against from A
        both = [f for f in to_a.first if f in to_b.first]
        for f in both:
            from_b, from_a = to_a.first.pop(f), to_b.first.pop(f)
            lateness.append((from_b - from_a) * 1000)
            if trace:
                trace.write(f'{f} {from_a:.6f} {from_b:.6f}\n')
        for d in (to_a, to_b):  # frames one side never sent (a new game began)
            for f in [f for f in d.first if d.first[f] < time.perf_counter() - 5]:
                del d.first[f]
        lateness_all.extend(lateness)
        for name, v in (('', lateness), (' in all', lateness_all) if final else (None, None)):
            if v and len(v) >= 10:
                q = statistics.quantiles(v, n=10)
                print(f"relay: B's inputs later than A's{name}: median {statistics.median(v):.1f} ms, "
                      f'10% {q[0]:.1f}, 90% {q[-1]:.1f} ({len(v)} frames)', flush=True)
        lateness.clear()
        for name, d in (('A', to_a), ('B', to_b)):
            if len(d.turnaround) >= 10:
                q = statistics.quantiles(d.turnaround, n=10)
                print(f"relay: {name}'s acks: median {statistics.median(d.turnaround):.2f} ms after the pad reached it, "
                      f'10% {q[0]:.2f}, 90% {q[-1]:.2f} ({len(d.turnaround)} acks)', flush=True)
            d.turnaround = []
            d.handed = {f: t for f, t in d.handed.items() if t > time.perf_counter() - 5}

    try:
        while True:
            now = time.perf_counter()
            while queue and queue[0][0] <= now:
                _, _, d, data = heapq.heappop(queue)
                if d.suggest:
                    data = renumber_unsequenced(data, d.groups)
                try:
                    d.outbound.sendto(data, d.dest)
                except OSError as e:  # (a moment without buffers: UDP may lose a packet; the relay must not die)
                    d.send_errors += 1
                    if d.send_errors == 1:
                        print(f'relay: to {d.name}: send failed: {e}', flush=True)
                    continue
                d.sent += 1
                frames = slippi_frames(data)
                for f in frames:
                    d.handed.setdefault(f, time.perf_counter())
                if passes and frames:
                    passes.write(f'D {d.name[-1]} {time.perf_counter():.6f} {max(frames)}\n')
            if now >= next_stats:
                stats()
                if passes:
                    passes.flush()
                next_stats = now + 10
            wait = max(0.0, min(queue[0][0] - now if queue else 0.05, 0.05))
            # select's timeout is coarse on Windows: spin out the last 2 ms
            ready, _, _ = select.select([sa, sb], [], [], wait if wait > 0.002 else 0)
            for s in ready:
                try:
                    data, _ = s.recvfrom(65536)
                except OSError:  # an ICMP port unreachable from an earlier send
                    continue
                d = by_sock[s]
                frames = slippi_frames(data)
                for f in frames:
                    d.first.setdefault(f, time.perf_counter())
                if passes and frames:  # (recorded before the loss: the player did send it)
                    passes.write(f"S {'B' if d is to_a else 'A'} {time.perf_counter():.6f} {max(frames)}\n")
                other = to_b if d is to_a else to_a  # an ack that comes this way answers pads sent that way
                for f in slippi_frames(data, SLIPPI_PAD_ACK):
                    if f in other.handed:
                        other.turnaround.append((time.perf_counter() - other.handed.pop(f)) * 1000)
                if args.loss and rng.random() * 100 < args.loss:
                    d.dropped += 1
                    continue
                delay = (args.delay + rng.random() * args.jitter) / 1000
                if d.suggest:
                    # each new frame whose inputs differ from what this player was last given of that
                    # opponent: a suggestion of them, sent that much before this pad, by the same path
                    for cmd, channel, header, newest, player, pads in pad_messages(data):
                        head = d.sug_head.get(player)
                        if head is not None and newest < head - 600:  # (a new game: its frames start again)
                            head = None
                        for i in range(len(pads) - 1, -1, -1):
                            f = newest - i
                            if head is not None and f <= head:
                                continue
                            pad = bytes(pads[i][:SUGGEST_PAD])
                            if head is not None and pad != d.sug_last.get(player):
                                if sug_rng.random() < args.suggest_share:
                                    d.sug_seq += 1
                                    lead = lead_min + sug_rng.random() * (lead_max - lead_min)
                                    s_delay = (args.delay + sug_rng.random() * args.jitter) / 1000
                                    s_due = max(time.perf_counter() - lead + s_delay, d.last_due)
                                    if not (args.loss and sug_rng.random() * 100 < args.loss):
                                        d.last_due = s_due
                                        seq += 1
                                        heapq.heappush(queue, (s_due, seq, d, suggestion_datagram(
                                            header, channel, player, f, d.sug_seq, pad)))
                                        d.suggested += 1
                                else:
                                    d.sug_skipped += 1
                            d.sug_last[player] = pad
                            head = f
                        if head is not None:
                            d.sug_head[player] = head
                        elif pads:
                            d.sug_head[player], d.sug_last[player] = newest, bytes(pads[0][:SUGGEST_PAD])
                now_t = time.perf_counter()
                if now_t >= d.spike_next:  # (a spike starts: this direction is held up)
                    d.spike_until = now_t + spike_ms / 1000 * (0.5 + 0.5 * spike_rng.random())
                    d.spike_next = now_t + spike_rng.expovariate(1 / spike_every)
                    d.spikes += 1
                due = max(now_t + delay, d.last_due, d.spike_until + delay if now_t < d.spike_until else 0)  # in order
                d.last_due = due
                d.delays.append((due - time.perf_counter()) * 1000)
                seq += 1
                heapq.heappush(queue, (due, seq, d, data))
                d.peak = max(d.peak, sum(1 for q in queue if q[2] is d))
    except KeyboardInterrupt:
        pass
    finally:
        stats(final=True)
        if passes:
            passes.close()


if __name__ == '__main__':
    main()

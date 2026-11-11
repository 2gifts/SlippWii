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
"""
import argparse
import ctypes
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    for n in ('a_adv', 'a_real', 'b_adv', 'b_real'):
        ap.add_argument(n, type=addr)
    ap.add_argument('--delay', type=float, default=0, help='one-way delay, ms')
    ap.add_argument('--jitter', type=float, default=0, help='extra delay, uniform 0..MS')
    ap.add_argument('--loss', type=float, default=0, help='packets dropped, percent')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--trace', help="each frame's input times, a line a frame: frame, from A, from B (seconds)")
    args = ap.parse_args()
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
    by_sock = {sa: to_a, sb: to_b}
    queue, seq = [], 0
    print(f'relay: A {args.a_adv} -> {args.a_real}, B {args.b_adv} -> {args.b_real}; delay {args.delay} ms, '
          f'jitter {args.jitter} ms, loss {args.loss}%', flush=True)
    next_stats = time.perf_counter() + 10
    lateness, lateness_all = [], []
    trace = open(args.trace, 'w', buffering=1) if args.trace else None

    def stats(final=False):
        for d in (to_a, to_b):
            avg = sum(d.delays) / len(d.delays) if d.delays else 0
            print(f'relay: {d.name}: {d.sent} sent, {d.dropped} dropped, average delay {avg:.1f} ms, '
                  f'most queued {d.peak}', flush=True)
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
                try:
                    d.outbound.sendto(data, d.dest)
                except OSError as e:  # (a moment without buffers: UDP may lose a packet; the relay must not die)
                    d.send_errors += 1
                    if d.send_errors == 1:
                        print(f'relay: to {d.name}: send failed: {e}', flush=True)
                    continue
                d.sent += 1
                for f in slippi_frames(data):
                    d.handed.setdefault(f, time.perf_counter())
            if now >= next_stats:
                stats()
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
                for f in slippi_frames(data):
                    d.first.setdefault(f, time.perf_counter())
                other = to_b if d is to_a else to_a  # an ack that comes this way answers pads sent that way
                for f in slippi_frames(data, SLIPPI_PAD_ACK):
                    if f in other.handed:
                        other.turnaround.append((time.perf_counter() - other.handed.pop(f)) * 1000)
                if args.loss and rng.random() * 100 < args.loss:
                    d.dropped += 1
                    continue
                delay = (args.delay + rng.random() * args.jitter) / 1000
                due = max(time.perf_counter() + delay, d.last_due)  # in order
                d.last_due = due
                d.delays.append((due - time.perf_counter()) * 1000)
                seq += 1
                heapq.heappush(queue, (due, seq, d, data))
                d.peak = max(d.peak, sum(1 for q in queue if q[2] is d))
    except KeyboardInterrupt:
        pass
    finally:
        stats(final=True)


if __name__ == '__main__':
    main()

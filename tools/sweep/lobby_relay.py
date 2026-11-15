"""A UDP relay among the players of a Slippi lobby (Teams, Party) that plays an
internet connection between every two of them: the same one-way delay, jitter
and loss on each path. net_relay.py does this for two players; this is the
same for any number, for lobby.py --net.

    python tools/sweep/lobby_relay.py NAME=ADV=REAL[=ADV2] [...]
        [--delay MS] [--jitter MS] [--loss PCT] [--seed N] [--passes FILE]

For each player: ADV, the host:port the relay binds, where the others are told
the player is; REAL, where the player really is. A datagram that reaches X's
ADV is for X; who sent it is known by its source port (every player's REAL
port is its own); it leaves, late, from the sender's ADV to X's REAL, so X
sees it come from where it was told the sender is. Order is kept on each path.

ADV2: a second address for a player on this machine, on its network address,
for the players that are not (a Wii): Windows sends from a loopback address
only to loopback and from a network address only to the network, so a copy of
Dolphin here is known to the other copies at a loopback address (ADV) and to
the Wii at ADV2.

Every 10 s and at the end, on stdout: each path's count, and for each pair how
much later one player's input for a frame reached the relay than the other's
(the first one's lateness as the second feels it) and how soon acks came back.

--passes FILE records every datagram that carries a pad: "S FROM TO time frame"
as it reaches the relay, "D TO FROM time frame" as it is handed over (frame:
its newest pad's), and every ack as it reaches the relay: "A FROM TO time
frame". lobby.py scores each player's game from these by Slippi's match
quality rule (match_quality.py).
"""
import argparse
import ctypes
import heapq
import os
import random
import select
import socket
import statistics
import time

from net_relay import SLIPPI_PAD_ACK, addr, slippi_frames


class Path:
    """Datagrams from one player to another."""

    def __init__(self, src, dst):
        self.src, self.dst = src, dst
        self.last_due = 0.0
        self.sent = self.dropped = self.send_errors = 0
        self.first = {}  # frame -> when its pad first reached the relay on this path
        self.handed = {}  # frame -> when its pad was handed to dst
        self.turnaround = []  # from that to dst's ack reaching the relay, ms
        self.lateness = []  # src's input for a frame after dst's own, ms
        self.turnaround_all, self.lateness_all = [], []  # the whole run's


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('players', nargs='+', metavar='NAME=ADV=REAL')
    ap.add_argument('--delay', type=float, default=0, help='one-way delay, ms')
    ap.add_argument('--jitter', type=float, default=0, help='extra delay, uniform 0..MS')
    ap.add_argument('--loss', type=float, default=0, help='packets dropped, percent')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--passes', help='every datagram with a pad, a line each')
    ap.add_argument('--until', metavar='FILE', help="end, with the whole run's numbers, once this file is there")
    args = ap.parse_args()
    if hasattr(ctypes, 'windll'):  # Windows' timer ticks every 15.6 ms otherwise: delays would be late
        ctypes.windll.winmm.timeBeginPeriod(1)
    rng = random.Random(args.seed)
    names, socks, socks2, real, by_port, by_sock = [], {}, {}, {}, {}, {}

    def bound(where, name):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(addr(where))
        if hasattr(socket, 'SIO_UDP_CONNRESET'):  # Windows: no error from an earlier send's ICMP
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
        by_sock[s] = name
        return s

    for spec in args.players:
        name, adv, where, *adv2 = spec.split('=')
        names.append(name)
        socks[name], real[name] = bound(adv, name), addr(where)
        if adv2:
            socks2[name] = bound(adv2[0], name)
        if real[name][1] in by_port:
            raise SystemExit(f'{name} and {by_port[real[name][1]]} share a port: the relay tells them apart by it')
        by_port[real[name][1]] = name
    paths = {(a, b): Path(a, b) for a in names for b in names if a != b}
    queue, seq, strangers = [], 0, set()
    print('relay: ' + ', '.join(args.players) + f'; delay {args.delay} ms, jitter {args.jitter} ms, loss {args.loss}%',
          flush=True)
    passes = open(args.passes, 'w') if args.passes else None
    next_stats = time.perf_counter() + 10
    next_look = time.perf_counter() + 0.5

    def stats(final=False):
        counted = []
        for (a, b), p in paths.items():
            # a's input for a frame against b's own for it (b's toward a)
            back = paths[(b, a)]
            for f in p.first:
                if f in back.first:
                    p.lateness.append((p.first[f] - back.first[f]) * 1000)
                    counted.append((p, f))
        for p, f in counted:
            del p.first[f]
        now = time.perf_counter()
        for p in paths.values():
            p.first = {f: t for f, t in p.first.items() if t > now - 5}
            p.handed = {f: t for f, t in p.handed.items() if t > now - 5}
        for a in names:
            late = ['%s %s' % (b, f'{statistics.median(paths[(a, b)].lateness):+.1f}' if len(paths[(a, b)].lateness) >= 10 else '-')
                    for b in names if b != a]
            acks = ['%s %s' % (b, f'{statistics.median(paths[(b, a)].turnaround):.1f}' if len(paths[(b, a)].turnaround) >= 10 else '-')
                    for b in names if b != a]
            sent = sum(paths[(a, b)].sent for b in names if b != a)
            dropped = sum(paths[(a, b)].dropped for b in names if b != a)
            print(f"relay: {a}: {sent} sent on, {dropped} dropped; its inputs later than each one's own, ms: "
                  + ', '.join(late) + '; its acks after a pad from each, ms: ' + ', '.join(acks), flush=True)
        for p in paths.values():
            p.lateness_all += p.lateness
            p.turnaround_all += p.turnaround
            p.lateness, p.turnaround = [], []
        if final:
            for a in names:
                late = ['%s %s' % (b, f'{statistics.median(paths[(a, b)].lateness_all):+.1f}' if len(paths[(a, b)].lateness_all) >= 10 else '-')
                        for b in names if b != a]
                acks = ['%s %s' % (b, f'{statistics.median(paths[(b, a)].turnaround_all):.1f}' if len(paths[(b, a)].turnaround_all) >= 10 else '-')
                        for b in names if b != a]
                print(f"relay: in all, {a}: its inputs later than each one's own, ms: " + ', '.join(late)
                      + '; its acks after a pad from each, ms: ' + ', '.join(acks), flush=True)

    try:
        while True:
            now = time.perf_counter()
            while queue and queue[0][0] <= now:
                _, _, p, data = heapq.heappop(queue)
                try:
                    # (to a player elsewhere, from the sender's address on the network)
                    out = socks2.get(p.src) if not real[p.dst][0].startswith('127.') else None
                    (out or socks[p.src]).sendto(data, real[p.dst])
                except OSError as e:  # (a moment without buffers: UDP may lose a packet; the relay must not die)
                    p.send_errors += 1
                    if p.send_errors == 1:
                        print(f'relay: {p.src} to {p.dst}: send failed: {e}', flush=True)
                    continue
                p.sent += 1
                frames = slippi_frames(data)
                t = time.perf_counter()
                for f in frames:
                    p.handed.setdefault(f, t)
                if passes and frames:
                    passes.write(f'D {p.dst} {p.src} {t:.6f} {max(frames)}\n')
            if args.until and now >= next_look:
                next_look = now + 0.5
                if os.path.exists(args.until):
                    break
            if now >= next_stats:
                stats()
                if passes:
                    passes.flush()
                next_stats = now + 10
            wait = max(0.0, min(queue[0][0] - now if queue else 0.05, 0.05))
            # select's timeout is coarse on Windows: spin out the last 2 ms
            ready, _, _ = select.select(list(by_sock), [], [], wait if wait > 0.002 else 0)
            for s in ready:
                try:
                    data, source = s.recvfrom(65536)
                except OSError:  # an ICMP port unreachable from an earlier send
                    continue
                dst, src = by_sock[s], by_port.get(source[1])
                if src is None or src == dst:
                    if source not in strangers:
                        strangers.add(source)
                        print(f'relay: a datagram for {dst} from {source[0]}:{source[1]}, which is no player: dropped', flush=True)
                    continue
                p = paths[(src, dst)]
                t = time.perf_counter()
                frames = slippi_frames(data)
                for f in frames:
                    p.first.setdefault(f, t)
                if passes and frames:  # (recorded before the loss: the player did send it)
                    passes.write(f'S {src} {dst} {t:.6f} {max(frames)}\n')
                back = paths[(dst, src)]  # an ack on this path answers pads handed over on the other
                for f in slippi_frames(data, SLIPPI_PAD_ACK):
                    if passes:
                        passes.write(f'A {src} {dst} {t:.6f} {f}\n')
                    if f in back.handed:
                        back.turnaround.append((t - back.handed.pop(f)) * 1000)
                if args.loss and rng.random() * 100 < args.loss:
                    p.dropped += 1
                    continue
                due = max(t + (args.delay + rng.random() * args.jitter) / 1000, p.last_due)  # in order
                p.last_due = due
                seq += 1
                heapq.heappush(queue, (due, seq, p, data))
    except KeyboardInterrupt:
        pass
    finally:
        stats(final=True)
        if passes:
            passes.close()


if __name__ == '__main__':
    main()

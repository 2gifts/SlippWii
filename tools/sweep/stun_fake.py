"""A STUN server for tests: answers binding requests (RFC 5389) with the address they came from.

    python tools/sweep/stun_fake.py 3478 3479 [--symmetric] [--silent PORT]

A client that queues the way Slippi Dolphin does after 3.6.4 asks two STUN servers where its
netplay socket is seen from (Nintendont/kernel/slippi_online/so_stun.h); the tests point it here
(stun_hosts=) so that it asks nobody on the internet. One UDP port per server.
  --symmetric: each port after the first reports the client's port plus its place in the list, as
               two servers see a router that gives every destination a port of its own
  --silent:    this port takes requests and never answers (a server that is down)
"""
import argparse
import select
import socket
import struct
import time

MAGIC = 0x2112A442


def answer(data, addr, shift=0):
    """The success response to a binding request, or None."""
    if len(data) < 20:
        return None
    kind, length, magic = struct.unpack('>HHI', data[:8])
    if kind != 0x0001 or magic != MAGIC:
        return None
    port = (addr[1] + shift) & 0xFFFF
    ip = struct.unpack('>I', socket.inet_aton(addr[0]))[0]
    attr = struct.pack('>HHBBHI', 0x0020, 8, 0, 0x01, port ^ (MAGIC >> 16), ip ^ MAGIC)
    return struct.pack('>HHI', 0x0101, len(attr), MAGIC) + data[8:20] + attr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ports', type=int, nargs='+')
    ap.add_argument('--symmetric', action='store_true')
    ap.add_argument('--silent', type=int, action='append', default=[])
    args = ap.parse_args()
    socks = []
    for port in args.ports:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('0.0.0.0', port))
        socks.append(s)
    print(f'stun: answering on UDP {", ".join(map(str, args.ports))}' + (' (symmetric)' if args.symmetric else ''), flush=True)
    while True:
        readable, _, _ = select.select(socks, [], [], 1.0)
        for s in readable:
            try:
                data, addr = s.recvfrom(2048)
            except OSError:
                continue  # (an ICMP error from an earlier answer)
            at = socks.index(s)
            port = args.ports[at]
            reply = None if port in args.silent else answer(data, addr, at if args.symmetric else 0)
            print(f'{time.strftime("%H:%M:%S")} stun: {len(data)} bytes from {addr[0]}:{addr[1]} on port {port}: '
                  + ('answered' if reply else 'no answer'), flush=True)
            if reply:
                s.sendto(reply, addr)


if __name__ == '__main__':
    main()

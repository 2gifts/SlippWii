"""Stands in front of a UDP server on this machine: takes datagrams on a port
and passes them to the server on 127.0.0.1, each client from a socket of its
own, and the answers back. For a test server the firewall does not let in
from the network (a new program's incoming traffic is blocked until someone
allows it; Python's is allowed, as the sweeps' relay needs).

    python tools/sweep/udp_front.py PORT BACKEND_PORT
"""
import select
import socket
import sys


def main():
    port, backend = int(sys.argv[1]), ('127.0.0.1', int(sys.argv[2]))
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.bind(('0.0.0.0', port))
    if hasattr(socket, 'SIO_UDP_CONNRESET'):  # Windows: no error from an earlier send's ICMP
        front.ioctl(socket.SIO_UDP_CONNRESET, False)
    by_client, by_sock = {}, {}
    print(f'front: UDP {port} -> {backend[0]}:{backend[1]}', flush=True)
    while True:
        ready, _, _ = select.select([front, *by_sock], [], [], 1.0)
        for s in ready:
            try:
                data, src = s.recvfrom(65536)
            except OSError:
                continue
            if s is front:
                if src not in by_client:
                    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    c.bind(('127.0.0.1', 0))
                    if hasattr(socket, 'SIO_UDP_CONNRESET'):
                        c.ioctl(socket.SIO_UDP_CONNRESET, False)
                    by_client[src], by_sock[c] = c, src
                    print(f'front: a client at {src[0]}:{src[1]}', flush=True)
                by_client[src].sendto(data, backend)
            else:
                front.sendto(data, by_sock[s])


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass

"""PC side of the Slippi-online-on-Wii measurement builds (M1, M1b, M1c).

Run this on the PC, then start the test build of Slippi Nintendont on the Wii
(same network, Slippi Networking on). Every 5 seconds the Wii reports UDP
round-trip times, how long an IOS sendto() takes, how evenly a 60-packet-per-
second stream from this PC arrives (M1c), and how long the game waits for
each per-frame EXI exchange with the Nintendont kernel. Everything is printed
and saved to wii-net-test-<time>.log next to this script.

Discovery works both ways:
- the Wii broadcasts "SLPWII HELLO" to UDP 51450;
- Slippi Nintendont's own SLIP_READY broadcast (UDP 20582) reaches the PC
  too. The test build puts its thread's state in that packet's nickname
  field, which is logged whenever it changes, and the PC answers the Wii
  directly from that packet's address.
The Wii turns the PC's stream on and off with "SLPWII STREAM 1/0"; each
report's "stream=" field repeats it in case that packet is lost.

    python tools/wii_net_test.py

Windows may ask to allow Python through the firewall: allow it, or the Wii's
packets will not arrive.
"""
import re
import select
import socket
import struct
import threading
import time
from pathlib import Path

PC_PORT, WII_PORT, SLIPPI_BROADCAST_PORT = 51450, 51451, 20582
STREAM_HZ = 60


def local_ip_towards(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ip, WII_PORT))
        return s.getsockname()[0]
    finally:
        s.close()


def udp(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', port))
    return s


class Streamer:
    """Sends "SLPWII PUSH <seq>" to the Wii at 60 Hz while enabled."""

    def __init__(self, sock):
        self.sock, self.target, self.on, self.sent = sock, None, False, 0
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        seq, period = 0, 1.0/STREAM_HZ
        next_at = time.perf_counter()
        while True:
            next_at += period
            if self.on and self.target:
                self.sock.sendto(b'SLPWII PUSH ' + struct.pack('>I', seq), (self.target, WII_PORT))
                seq += 1
                self.sent += 1
            delay = next_at - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.perf_counter()


def main():
    log = Path(__file__).resolve().parent/time.strftime('wii-net-test-%Y%m%d-%H%M%S.log')
    sock, beacon = udp(PC_PORT), udp(SLIPPI_BROADCAST_PORT)
    streamer = Streamer(udp(0))
    print(f'Listening on UDP {PC_PORT} and {SLIPPI_BROADCAST_PORT}. Start the test build on the Wii. Log: {log}',
          flush=True)
    wii, pings, addr_msgs, last_status, last_here, pushes_reported = None, 0, 0, None, 0.0, 0
    with log.open('a', encoding='utf-8') as out:
        def note(text):
            line = f'{time.strftime("%H:%M:%S")} {text}'
            print(line, flush=True)
            out.write(line + '\n')
            out.flush()

        def say_here(ip):
            sock.sendto(b'SLPWII HERE ' + socket.inet_aton(local_ip_towards(ip)), (ip, WII_PORT))

        while True:
            ready, _, _ = select.select([sock, beacon], [], [], 1.0)
            if beacon in ready:
                data, (ip, _) = beacon.recvfrom(2048)
                if data.startswith(b'SLIP_READY'):
                    status = data[16:48].split(b'\0')[0].decode(errors='replace')
                    if status != last_status:
                        note(f'SLPWII LOG beacon from {ip}: {status}')
                        last_status = status
                    if status.startswith('OT') and time.monotonic() - last_here > 1.5:
                        say_here(ip)  # answer the Wii directly, in case its broadcast does not reach us
                        last_here = time.monotonic()
            if sock not in ready:
                continue
            data, (ip, _) = sock.recvfrom(2048)
            if data.startswith(b'SLPWII PING '):
                sock.sendto(b'SLPWII PONG ' + data[12:], (ip, WII_PORT))
                pings += 1
                continue
            if data.startswith(b'SLPWII HELLO'):
                say_here(ip)
                if wii != ip:
                    note(f'SLPWII LOG Wii at {ip} found us (HELLO)')
                wii = ip
                continue
            if data.startswith(b'SLPWII ADDR'):
                addr_msgs += 1
                continue
            if data.startswith(b'SLPWII STREAM '):
                streamer.target, streamer.on = ip, data[14:15] == b'1'
                note(f'SLPWII LOG stream {"on" if streamer.on else "off"} to {ip}')
                continue
            text = data.decode(errors='replace')
            if text.startswith('SLPWII REPORT'):
                m = re.search(r'stream=(\d)', text)
                if m:
                    streamer.target, streamer.on = ip, m.group(1) == '1'
                text += (f' | pc pings_echoed={pings} addr_msgs={addr_msgs}'
                         f' pushes_sent={streamer.sent - pushes_reported}')
                pings = addr_msgs = 0
                pushes_reported = streamer.sent
            note(text)


if __name__ == '__main__':
    main()

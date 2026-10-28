"""Check tools/wii_net_test.py against a simulated Wii on this PC (localhost).

    python tools/test_wii_net_test.py
"""
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    proc = subprocess.Popen([sys.executable, '-u', str(HERE/'wii_net_test.py')], stdout=subprocess.PIPE, text=True)
    time.sleep(1.5)
    wii = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    wii.bind(('127.0.0.1', 51451))
    wii.settimeout(3)
    try:
        # 1. Slippi's beacon with the test thread's state: the PC answers HERE directly.
        nick = b'OT wait n1 ipc0a80427 e0 w12'.ljust(32, b'\0')
        wii.sendto(b'SLIP_READY' + b'\x00\x11\x22\x33\x44\x55' + nick, ('127.0.0.1', 20582))
        data, _ = wii.recvfrom(100)
        assert data[:12] == b'SLPWII HERE ' and len(data) == 16, data
        # 2. HELLO, PING/PONG and a report.
        wii.sendto(b'SLPWII HELLO 7f000001', ('127.0.0.1', 51450))
        assert wii.recvfrom(100)[0][:12] == b'SLPWII HERE '
        wii.sendto(b'SLPWII PING ' + struct.pack('>II', 5, 777), ('127.0.0.1', 51450))
        data, _ = wii.recvfrom(100)
        assert data == b'SLPWII PONG ' + struct.pack('>II', 5, 777), data
        wii.sendto(b'SLPWII REPORT 1 ip=7f000001 exi_total=0', ('127.0.0.1', 51450))
        time.sleep(0.5)
        # 3. STREAM 1: about 60 PUSH packets a second until STREAM 0.
        wii.sendto(b'SLPWII STREAM 1', ('127.0.0.1', 51450))
        pushes, end = 0, time.monotonic() + 1.0
        while time.monotonic() < end:
            try:
                data, _ = wii.recvfrom(100)
            except socket.timeout:
                break
            pushes += data.startswith(b'SLPWII PUSH ')
        wii.sendto(b'SLPWII STREAM 0', ('127.0.0.1', 51450))
        assert 50 <= pushes <= 70, pushes
        time.sleep(0.3)
        wii.settimeout(0.3)
        try:
            while wii.recvfrom(100):
                pass
        except socket.timeout:
            pass
        try:
            data = wii.recvfrom(100)[0]
            raise AssertionError(f'stream did not stop: {data!r}')
        except socket.timeout:
            pass
    finally:
        proc.terminate()
    out = proc.communicate(timeout=5)[0]
    assert 'beacon from 127.0.0.1: OT wait n1 ipc0a80427 e0 w12' in out, out
    assert 'found us (HELLO)' in out and 'SLPWII REPORT 1' in out and 'pings_echoed=1' in out, out
    for log in HERE.glob('wii-net-test-*.log'):
        if 'REPORT 1 ip=7f000001' in log.read_text():
            log.unlink()
    print(f'PASS: beacon -> direct HERE, HELLO -> HERE, PING -> PONG, report logged, stream {pushes}/s on and off')


if __name__ == '__main__':
    main()

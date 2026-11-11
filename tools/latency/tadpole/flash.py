"""Puts a firmware on a Tadpole (an RP2040) plugged into this PC, and shows what it prints.

    python tools/latency/tadpole/flash.py [FILE.uf2]      (default: build/latency/tadpole_latency.uf2)

The controller must be in its flashing mode (the PC shows a drive named RPI-RP2): hold Start (or
whatever its firmware asks for) while plugging it in. One already running the latency test firmware
(or any arduino-pico one) needs no button: its serial port, opened at 1200 baud, sends it there.
"""
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def ps(command):
    return subprocess.run(['powershell', '-NoProfile', '-Command', command], capture_output=True, text=True).stdout


def rp2_drive():
    out = ps("(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'RPI-RP2' }).DriveLetter").split()
    return out[0] if out else None


def pico_ports():
    """Serial ports of Raspberry Pi's USB vendor ID (2E8A)."""
    out = ps("Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -match 'VID_2E8A' } | "
             "ForEach-Object { $_.DeviceID }")
    return out.split()


def read_port(port, seconds):
    return ps(f"$p = New-Object System.IO.Ports.SerialPort {port},115200; $p.DtrEnable = $true; $p.ReadTimeout = 500; "
              f"$p.Open(); $end = (Get-Date).AddSeconds({seconds}); while ((Get-Date) -lt $end) {{ "
              "try { $p.ReadLine() } catch [TimeoutException] {} }; $p.Close()")


def main():
    uf2 = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/latency/tadpole_latency.uf2'
    if not uf2.exists():
        sys.exit(f'{uf2} is not there (tools/latency/tadpole/build.py makes it)')
    drive = rp2_drive()
    if not drive:
        for port in pico_ports():
            print(f'{port}: asking for the flashing mode (1200 baud)')
            ps(f"$p = New-Object System.IO.Ports.SerialPort {port},1200; $p.Open(); Start-Sleep -Milliseconds 200; $p.Close()")
        for _ in range(20):
            drive = rp2_drive()
            if drive:
                break
            time.sleep(0.5)
    if not drive:
        sys.exit('no RPI-RP2 drive: unplug the controller, hold Start, and plug it into this PC with a USB cable')
    info = Path(f'{drive}:/INFO_UF2.TXT')
    print(f'{drive}: {info.read_text().strip().splitlines()[-1] if info.exists() else "RPI-RP2"}')
    shutil.copy(uf2, f'{drive}:/{uf2.name}')
    print(f'{uf2.name} copied ({uf2.stat().st_size} bytes); it restarts by itself')
    for _ in range(30):
        time.sleep(0.5)
        ports = pico_ports()
        if ports and not rp2_drive():
            print(f'{ports[0]}:')
            print(read_port(ports[0], 4).strip() or '(nothing printed)')
            return
    print('it did not come back as a serial port (a firmware without one, such as HayBox, shows as a controller)')


if __name__ == '__main__':
    main()

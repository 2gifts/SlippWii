"""Boot a Slippi Nintendont build on the Wii over the network, straight into
Melee: no SD card swap.

The Homebrew Channel accepts programs over TCP 4299 (the wiiload protocol)
while it is on screen. Nintendont takes its whole configuration as argv[1]
(a NIN_CFG, as USB Loader GX passes it); with the auto-boot bit set it skips
its menu and starts the game in that configuration.

    python tools/wii_boot.py                 # loader.dol from the last build
    python tools/wii_boot.py --wii-ip 192.168.1.50 --dol path/to/boot.dol

The configuration starts from a copy of the SD card's /slippwii_nincfg.bin
(tools/wii_boot.py --save-cfg E: stores one in build/).
"""
import argparse
import os
import shutil
import socket
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DOL = ROOT / 'Nintendont/loader/loader.dol'
BASE_CFG = ROOT / 'build/nincfg-base.bin'
DEFAULT_WII = os.environ.get('SLIPPWII_WII_IP')  # or --wii-ip
WIILOAD_PORT = 4299
ARGV0 = b'sd:/apps/Slippi Nintendont C online test/boot.dol'

NIN_MAGIC = 0x01070CF6
NIN_VERSION = 0x0E
NIN_CFG_AUTO_BOOT = 1 << 10


def autoboot_cfg(base):
    """The NIN_CFG with the auto-boot bit set (the game path stays as is)."""
    magic, version, config = struct.unpack('>3I', base[:12])
    if magic != NIN_MAGIC or version != NIN_VERSION:
        raise ValueError(f'not a Nintendont config v{NIN_VERSION:#x} (magic {magic:#x}, version {version:#x})')
    return base[:8] + struct.pack('>I', config | NIN_CFG_AUTO_BOOT) + base[12:]


def wiiload_packet(dol, args):
    """The wiiload 0.5 stream: header, zlib-compressed program, arguments."""
    packed = zlib.compress(dol, 6)
    header = b'HAXX' + bytes([0, 5]) + struct.pack('>H', len(args)) + struct.pack('>II', len(packed), len(dol))
    return header + packed + args


def boot(wii_ip, dol_path, cfg_path, timeout=10):
    dol = Path(dol_path).read_bytes()
    cfg = autoboot_cfg(Path(cfg_path).read_bytes())
    args = ARGV0 + b'\0' + cfg + b'\0'
    data = wiiload_packet(dol, args)
    with socket.create_connection((wii_ip, WIILOAD_PORT), timeout=timeout) as s:
        s.sendall(data)
    return len(dol)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--wii-ip', default=DEFAULT_WII, help="the Wii's address (default: $SLIPPWII_WII_IP)")
    ap.add_argument('--dol', default=str(DEFAULT_DOL))
    ap.add_argument('--cfg', default=str(BASE_CFG), help='NIN_CFG to start from')
    ap.add_argument('--save-cfg', metavar='SD', help='copy SD:/slippwii_nincfg.bin to build/ and exit')
    args = ap.parse_args()
    if args.save_cfg:
        BASE_CFG.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(args.save_cfg.rstrip('\\/') + '/') / 'slippwii_nincfg.bin', BASE_CFG)
        print(f'saved {BASE_CFG}')
        return 0
    if not args.wii_ip:
        ap.error('--wii-ip is needed (or set SLIPPWII_WII_IP)')
    try:
        size = boot(args.wii_ip, args.dol, args.cfg)
    except OSError as e:
        print(f'could not reach the Homebrew Channel at {args.wii_ip}:{WIILOAD_PORT} ({e}).')
        print('Is the Wii on the Homebrew Channel screen, with its network icon lit?')
        return 1
    print(f'sent {Path(args.dol).name} ({size} bytes) to {args.wii_ip}: Melee should start in a few seconds')
    return 0


if __name__ == '__main__':
    sys.exit(main())

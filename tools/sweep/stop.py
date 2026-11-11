"""Stops whatever sweep is running on this PC, and brings the Wii back to the Homebrew Channel.

    python tools/sweep/stop.py [--wii-ip IP]

Stopping the shell that started a sweep (or a batch of them) leaves its run_sweep.py alive: it goes on
waiting, gives up minutes later, and then sends the Wii its exit and kills the helpers of whatever run
came after it. So: every run_sweep.py and batch script first, by process, then the helpers, then the
Wii's exit, once, and one wait for the Homebrew Channel. Never while a build is being sent to it (the
first seconds after "sent the build"): the Homebrew Channel then sits at an error until someone
presses a button on the console.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_sweep  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'))
    ap.add_argument('--batch', default='lat_batch|morning|lag_ab|soak', help='shell scripts that start sweeps: parts of their names')
    args = ap.parse_args()
    for pid, cmd in run_sweep.other_sweeps():
        print(f'stopping run_sweep.py, process {pid}')
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(pid)], capture_output=True)
    for name in ('sh.exe', 'bash.exe'):
        for pid, _, cmd in run_sweep.processes(name):
            if any(part in cmd for part in args.batch.split('|')):
                print(f'stopping {name}, process {pid}: {cmd[-60:]}')
                subprocess.run(['taskkill', '/F', '/T', '/PID', str(pid)], capture_output=True)
    time.sleep(1)
    left = run_sweep.other_sweeps()
    if left:
        sys.exit(f'still running: {left}')
    run_sweep.kill_leftovers()
    if run_sweep.LOCK.exists():
        run_sweep.LOCK.unlink()
    if args.wii_ip:
        run_sweep.send_exit(args.wii_ip)
        print('the Homebrew Channel answers after', run_sweep.wait_hbc(args.wii_ip, 60), 's (None: it does not)')


if __name__ == '__main__':
    main()

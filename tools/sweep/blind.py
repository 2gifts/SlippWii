"""A blind test: which feels better on the Wii, rollback or delay based netcode, over the same
connection? A person plays the PC's bot (run_sweep.py --human) with versions A and B, in a random
order, and says which they prefer; only then does the key say which was which.

    python tools/sweep/blind.py prepare --wii-ip IP --net 30,5 [--delay 3] [--plays 4]
    python tools/sweep/blind.py next     (sends the next version to the Wii; plays until `stop`)
    python tools/sweep/blind.py stop     (from another shell: ends this play, the Wii goes back to the HBC)
    python tools/sweep/blind.py reveal   (the key, and where each play's logs are)

--delay: the delay based version's input delay in frames; rollback keeps Slippi's default of 2. The
builds have no delay buttons on the CSS, which would show the version and change it. Each play keeps
the logs a sweep does, in build/sweep/blind/<session>/play-N. Nothing here prints the key before
`reveal`, and a play's directory is named by its number, not its version.
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parents[1] / 'build/sweep/blind'
SWEEP = [sys.executable, '-u', str(HERE / 'run_sweep.py')]


def session():
    """The latest session's directory and state."""
    dirs = sorted(OUT.iterdir()) if OUT.exists() else []
    if not dirs:
        sys.exit('no session yet: run prepare first')
    return dirs[-1], json.loads((dirs[-1] / 'session.json').read_text())


def save(run, state):
    (run / 'session.json').write_text(json.dumps(state))


def prepare(args):
    run = OUT / time.strftime('%Y%m%d-%H%M%S')
    run.mkdir(parents=True)
    pick = random.SystemRandom()
    names = ['rollback', 'delay']
    pick.shuffle(names)
    key = dict(zip('AB', names))
    # as many of each, shuffled: neither the first nor the last play says anything
    order = list('AB' * (args.plays // 2))
    pick.shuffle(order)
    for label, name in key.items():
        extra = ['--kernel', 'rollback=0', '--kernel', f'delay={args.delay}'] if name == 'delay' else []
        print(f'building version {label} (a minute or two)...', flush=True)
        subprocess.run([*SWEEP, '--wii-ip', args.wii_ip, '--rollback', '0', '--human', '--net', args.net,
                        '--rounds', str(args.rounds), '--seed', str(args.seed),
                        '--no-delay-buttons', '--build-to', str(run / f'version-{label}'), *extra],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    save(run, {'key': key, 'order': order, 'next': 0, 'net': args.net, 'wii_ip': args.wii_ip,
               'rounds': args.rounds, 'delay': args.delay})
    print(f'ready: {len(order)} plays over {args.net} (one-way ms). `blind.py next` for the first.')


def next_play(_):
    run, state = session()
    i = state['next']
    if i >= len(state['order']):
        sys.exit('all plays done: run reveal')
    label = state['order'][i]
    play = run / f'play-{i + 1}'
    shutil.copytree(run / f'version-{label}', play)
    state['next'] = i + 1
    save(run, state)
    print(f'play {i + 1} of {len(state["order"])}: version {label}', flush=True)
    code = subprocess.run([*SWEEP, '--wii-ip', state['wii_ip'], '--rollback', '0', '--human', '--net', state['net'],
                           '--rounds', str(state['rounds']), '--skip-build', str(play)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    failure = play / 'failure.txt'
    print(f'play {i + 1} ended' + (f': {failure.read_text().strip()}' if failure.exists() else f' (exit {code})'))


def stop(_):
    run, state = session()
    if not state['next']:
        sys.exit('no play has started')
    (run / f'play-{state["next"]}' / 'stop').touch()
    print('stopping: the Wii goes back to the Homebrew Channel')


def reveal(_):
    run, state = session()
    for label, name in state['key'].items():
        print(f'version {label}: {name}' + (f' ({state["delay"]} frames of input delay)' if name == 'delay' else ''))
    for i, label in enumerate(state['order'][:state['next']], 1):
        print(f'play {i}: version {label} ({state["key"][label]}), logs in {run / f"play-{i}"}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--net', required=True, metavar='DELAY[,JITTER[,LOSS]]')
    p.add_argument('--delay', type=int, default=2, help="the delay based version's input delay, in frames")
    p.add_argument('--plays', type=int, default=4, help='how many plays in all, half of each version')
    p.add_argument('--rounds', type=int, default=20, help='most games a play')
    p.add_argument('--seed', type=int, default=40, help="the bot's inputs (at most 126)")
    p.add_argument('--wii-ip', default=os.environ.get('SLIPPWII_WII_IP'), required=not os.environ.get('SLIPPWII_WII_IP'))
    sub.add_parser('next')
    sub.add_parser('stop')
    sub.add_parser('reveal')
    args = ap.parse_args()
    {'prepare': prepare, 'next': next_play, 'stop': stop, 'reveal': reveal}[args.cmd](args)


if __name__ == '__main__':
    main()

# Building SlippWii

The build runs on Windows 10/11 from **Git Bash**, with **Python 3.14** (the toolchain fetcher uses
its `compression.zstd`).

```sh
python tools/fetch_devkitpro.py      # a portable devkitPPC r47.1, devkitARM r66 and libogc 2.13.0 in .toolchain/
sh tools/build.sh                    # builds Nintendont: dist/sd/apps/SlippWii/boot.dol
```

`make` can't handle spaces in paths, so `build.sh` maps the repository to drive `W:` (`subst`). It
builds with `-j1`: the upstream Makefile can compile `MeleeCodes.c` before the Gecko headers it
includes have been generated.

## The parts

| Path | What |
|---|---|
| `Nintendont/` | Slippi Nintendont with SlippWii's changes; the online client is `kernel/slippi_online/` (see its README) |
| `Nintendont/kernel/gecko/g_online_boot.bin` | Codes installed in Melee: Slippi's bootloader, the crash reporter and the delay buttons. Rebuild with `python tools/build_online_boot.py` (committed; `--stall`, `--heartbeat` and `--autoplay` make diagnostic and test builds) |
| `tools/css_delay/` | The delay buttons on the character select screen (C, built position independent by `tools/pic_code.py`) |
| `tools/ppc/` | The crash reporter and diagnostics (PowerPC assembly) |
| `Nintendont/loader/source/slippwii.c`, `vcdiff.c` | Before Melee starts: the account check, and Slippi's menu patches applied to the disc (first start) |
| `release/` | What goes on the SD card besides the app: the Homebrew Channel icon and `meta.xml`, the account-copy script |
| `tools/slippi_files.py` | Slippi's pinned files; `netplay.gct`, and a PC-side patcher for development |
| `tools/make_release.py` | The release zip (the SD card's files, with the manifest of what the Wii must make) |
| `tools/online_selftest.py`, `tools/sweep/`, `tools/wii_mirror.py`, `tools/slp_compare.py` | Test tools |

## Tests

```sh
cd Nintendont/kernel/slippi_online && sh tests/run_tests.sh      # unit tests (needs a host gcc)
python tools/online_selftest.py pc --game                       # the client vs a fake Melee and opponent
```

`online_selftest.py pc` needs a fake matchmaking server and peer (`slippi_fake_mm.exe`,
`slippi_fake_peer.exe`, from the Melee 3DS port's Slippi tools); point `--tools` at them.

## On a Wii

Leave the SD card in the Wii, open the Homebrew Channel, and send a build over the network:

```sh
export SLIPPWII_WII_IP=192.168.1.50    # your Wii's address
python tools/online_selftest.py watch --boot    # sends boot.dol and records the Wii's log
```

The log only goes to your PC (`log_host` in `SD:/slippwii/config.ini`, which `online_selftest.py
setup --real` writes). Release builds log nothing.

## A release

1. Bump `SLIPPWII_VERSION` in `Nintendont/common/include/Metadata.h` and `<version>` in
   `release/apps/SlippWii/meta.xml`, and add a `CHANGELOG.md` entry.
2. `python tools/build_online_boot.py`, `sh tools/build.sh`, then commit.
3. `python tools/make_release.py X.Y.Z --iso Melee.iso` makes `dist/SlippWii-vX.Y.Z.zip` (the disc
   gives the patched files' CRC-32s for the manifest; nothing else of it is kept). Attach the zip to a
   GitHub release tagged `vX.Y.Z`.

`python tools/test_vcdiff_c.py --iso Melee.iso` checks the loader's patch decoder against the
Python one (needs a host `gcc`).

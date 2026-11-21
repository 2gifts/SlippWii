# Already tried

Ideas that come up often, or that were tried and measured, with what was found. If you want to
reopen one, bring a measurement that answers the finding. Details are in the linked docs and in the
commit history.

## Input delay and timing

**"Use Nintendont's PDF + 1/2F lag reduction online."**
The target is the tournament console, and "PDF + 1/2F" is *faster* than one. Slippi's online
codeset, which SlippWii runs as Slippi Dolphin does, already has the polling drift fix, and
SlippWii's online timing was built and measured to match the tournament console at 1 delay frame
([INPUT_DELAY.md](INPUT_DELAY.md)). An independent tester's numbers, on a CRT with a light-sensor
adapter, before that work (1.6.1): plain Nintendont offline about 57 ms; SlippWii at 1 delay frame
about 53 ms; with PDF + 1/2F about 52 ms. After the 1.8.0 timing work, the same tester found
SlippWii at 1 delay frame "perfectly in line" with their offline results. Offline, the setting stays
the player's, as in Slippi Nintendont.

**"Make offline faster too" / "turn the VB flag on offline."**
Offline stays what Slippi Nintendont gives: vanilla Melee plus the player's settings. That is the
reference online is measured against. A fix once brought back the VB part of the polling drift fix
offline, which made offline half a frame faster than Slippi Nintendont's; it was undone on purpose.

**"Read the controller more often" / "add polls so the Wii sends suggestions sooner."**
Measured 2026-11-20: the Wii suggesting 95% of input changes instead of 60% cut the PC player's
rollbacks from 1317 to 1139 a minute at 30 ms one way, but it needs extra controller polls, and the
game's controller reads must not change. Not taken.

**"Start a game a refresh behind when the connection is heavy, to be safe."**
Tried 2026-11-20 for the 85-105 ms ping band with heavy characters: it removed most slow frames, but
it made one game play a refresh later than the next for reasons the player can't see. Reverted. A
fixed, stated rule by ping would be the only acceptable form.

**"Wait out frames when the Wii gets ahead."**
Slippi Dolphin catches up too, and the Wii ends up behind. Removed (2026-11-08).

**Nudging the video timing by shortening every line.**
The picture was lost on the capture card. Ending a frame one line early holds it, and is what's used
(sparingly) now.

## Rollback cost on the Wii

The cost of a rollback pass is at its floor on this hardware; see [ROLLBACK.md](ROLLBACK.md). Each
of these was built and measured:

- **Skipping savestate pages whose contents didn't change**: about half are identical, but a load
  still restores every page written since, so nothing is saved.
- **Checksum-skipping identical pages at capture**: the checksum costs what the skip saves.
- **CPU copies instead of DMA** for restores: DMA is the fastest (11.5 µs a page; CPU 33).
- **Pipelined DMA and CPU restores**: slower; they share the bus.
- **Newest captures or the page table in MEM1**: not enough free MEM1 in a match; the gain was
  0.1-0.3 ms a pass.
- **Skipping animation work during a resimulation**: desyncs; hitboxes and platforms follow
  animations.
- **Excluding GX copy destinations from the savestate**: no measurable gain.

**"Rollback in Teams and Party."**
With three opponents one rollback pass takes about 20 ms, longer than a frame (16.7 ms), and the
engine's store holds too few captures. Teams and Party wait for inputs instead, with the delay set
by ping ([FOUR_PLAYERS.md](FOUR_PLAYERS.md)). This needs a pass several times faster, not tuning.

## Network

- **Holding acks back to share a datagram with the next pad** (4-8 ms): worse; the Wii's inputs
  reached the PC 3-4 ms later (2026-11-20). Acks go out at once.
- **Sending acks ahead of queued pads**: almost never reorders anything; no gain.
- **Two sender threads**: IOS serializes sends; no gain.
- **Smaller pad packets**: IOS's send cost is per datagram, not per byte; no gain.
- **The network thread above or below IOS's**: no gain in reply time.
- **Wi-Fi**: far too slow and jittery for online play. SlippWii is wired only, and checks.

## Loading

**Unpaced disc reads** (as fast as the card allows): one load in 27 crashed. Reads are paced (2 ms
seek, 60 MB/s), which cut the PC's halts at a game's start from about 7 to under 1.

## Scope

- **Ranked**: Slippi's protected competitive mode; left out.
- **New modes, menus or settings** beyond Slippi's own: out of scope ([CONTRIBUTING.md](../CONTRIBUTING.md)).

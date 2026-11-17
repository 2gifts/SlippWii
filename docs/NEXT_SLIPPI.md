# Following Slippi's next version

2026-11-16. Where SlippWii stands against the Slippi Dolphin that comes after 3.6.4: what changes,
what was built for it, what was proven and how, and the steps for the day Slippi releases it.
Branch `tls-kernel`. **That day was 2026-11-17: Slippi 3.7.0. The switch is turned, and SlippWii
1.7.0 speaks 3.7.0 only (the next section). The rest of this page is as it was written the day
before, when every build still spoke 3.6.4.**

## 2026-11-17: Slippi 3.7.0

Slippi published 3.7.0 at 16:23 UTC, and from then on its matchmaking turned 3.6.4 away: every
SlippWii showed *Exit* and *Update* only, by its own rule. The steps below, as they went:

1. **The release against what was built.** 3.7.0 is the development source this was built from
   (`7333463b`) and three commits: the version number; chat taken under a lock, a message's player
   checked to be one of the others; input suggestions off when a bot is in the game. Nothing a
   console speaks differently. The codeset and the 16 game files at the tag (`07b2d287`) are the
   pinned ones, hash for hash. (The number is 3.7.0, not the 3.6.5 guessed here.)
2. **The pins**: `NEXT_COMMIT` is the tag's commit; the tests' Dolphin is the release itself
   (`FM-Slippi-3.7.0-Win.zip`, sha256 `5bf5e9d4...`, unpacked into `build/next/dolphin-3.7.0`).
3. **The number**: `SO_APP_VERSION_DEFAULT` "3.7.0". 4. The switch, the boot codes, the build.
5. **Tests against the released Dolphin**, through the test matchmaking only: one opponent at
   71 ms 4 of 4 replays identical, with rollbacks forced 3 of 3, at 100 ms with 1% loss 6 of 6,
   from port 2 2 of 2; Teams 9 of 9 replays, Teams at 63 ms 9 of 9, Party 3 of 3, a Dolphin closed
   in mid-game 3 of 3; a player's kernel (`test_control=0`) back at the Homebrew Channel by the
   menu's Exit in 4 s; unit tests 727 checks. Taken from 3.7.0 as well: a chat message is accepted
   only from a player who is in the game (`on_chat`).
6. **First contact with Slippi's service** is the one step no test here does: by hand, before the
   release is published. What public records say of it beforehand: `matchmaking.slippi.gg` is an
   alias of `ghs.googlehosted.com`, and its certificate (logged 2026-10-12) is issued by Google
   Trust Services' WR3, under GTS Root R1: among the kernel's six roots, and the chain's shape the
   handshake was timed with.

## Where it stands

Slippi's public development source (project-slippi/Ishiiruka, branch `slippi`, commit `7333463b`
of 2026-11-02; version string `3.6.5-dev.1`) is not compatible with 3.6.4 in four places: how a
client finds a match, what two clients send each other in a game, what the game's own code
expects from the client, and two game files that are now filled in from the player's disc. When
that version is released and Slippi's server turns 3.6.4 away, every SlippWii stops queueing (by
its own rule, since 1.6.0) until a SlippWii that speaks the new version is installed.

All of it is built, and a Wii plays Slippi's own development Dolphin with it:

| Piece of the next version | State |
|---|---|
| Matchmaking: a WebSocket over TLS, after a STUN lookup | built; proven on the Wii |
| Netplay packets: 9-byte pads, drop reports, input suggestions | built; proven on the Wii against the development Dolphin |
| EXI device: larger `0xB0` and `0xB3` answers, predicted inputs, random bits | built; proven the same way |
| Game codes: new RNG seeding, larger input code, stage fixes; the rollback engine on them | built; proven, with rollbacks forced |
| Game files whose assets are links into the player's disc | built in the loader and the tools; byte-identical to Slippi's own tool |
| Four players: a player leaving, Party as a stock game | built; proven on the Wii with three development Dolphins. With a PC mirroring the console as well, one game in three collapsed on the heaviest stage until the replay stream was sent in pieces ("Not proven, and open") |
| The match-quality rule | unchanged: the kernel's rule is Slippi's current source, line for line |
| A release of it: one switch, the files, an update over an installed 1.6.x | built; a trial release made and tested |

What is not proven is what only Slippi's own service can show (at the end).

## The switch, and the day Slippi releases

`SO_SLIPPI_NEXT` in `Nintendont/kernel/slippi_online/so_config.h` says which Slippi a checkout
builds for: `0`, Slippi 3.6.4; `1`, the version after it. It is the only switch:

- the kernel's defaults follow it: the version it reports, and the matchmaking service
  (`mm.slippi.gg` by ENet, or `wss://matchmaking.slippi.gg/v1/queue`);
- `tools/build_online_boot.py` builds the boot codes for that version (the rollback engine is
  built against one version's codes);
- `tools/make_release.py` ships that version's codes and files;
- the test harness (`tools/sweep`) runs a switched checkout against the development Dolphin only.

One way to queue and one netcode in a release, never two: against Slippi's server neither the
version nor the service is a setting (`so_config_finish`). At run time the kernel goes by the
version it reports and by `mm_url`, which is how a test build of an unswitched checkout speaks
the next version (`run_sweep.py --next`).

When Slippi releases:

1. **Read the release against what was built.** Everything here was read from commit `7333463b`
   (Ishiiruka), `d0ce23a` (slippi-ssbm-asm) and `934f373` (slippi-rust-extensions). Diff the
   release against those: `SlippiNetplay.cpp/.h`, `SlippiPad`, `EXI_DeviceSlippi.cpp`,
   `SlippiExiTypes.h`, `SlippiMatchmaking`; the crates `matchmaking`, `match-quality`, `hsd`;
   `Online/` and `Common/` of the codes. What changed after those commits must be built first.
2. **Move the pins**: in `tools/slippi_files.py` the source (`NEXT_COMMIT`), the codeset's and
   each game file's SHA-256 and the two linked files' results (`NEXT_LINKED`;
   `tools/test_hsd_links.py --slippi-hsd` checks them against Slippi's own tool); the checkout
   `slippi-ssbm-asm-next` at the release's commit of the codes; the released Dolphin unpacked
   into `build/next/dolphin-dev`.
3. **Set the number**: `SO_APP_VERSION_DEFAULT` for the switched build (now `"3.6.5"`, a guess).
4. `python tools/slippi_version.py next`, `python tools/build_online_boot.py`,
   `sh tools/build.sh`.
5. **Test** (below, "How to run the tests"): the unit tests, the PC runs, the loader's file tests
   with the old and the new release's zips, the sweeps and lobbies against the released Dolphin, a
   soak, and one run without the tests' way back (`test_control=0`).
6. **First contact with Slippi's service**, with the user's own account, by hand: the one thing no
   test here does.
7. `tools/make_release.py`, and publish on the user's word. Installed copies are told by "Check
   for Updates"; the loader makes the new version's files on the first start (tested both ways).

The code for 3.6.4 (ENet matchmaking, 8-byte pads, the old layouts) stays until that release is
out and can then be removed.

## What changes after 3.6.4

Read from the source at the commits above, against tag `v3.6.4`. "Both" means two players' clients
must agree, or the game desyncs or the packets are misread.

**Matchmaking:**
- The ENet service (`mm.slippi.gg:43113`) is gone from the client, with no switch. The new one is
  `wss://matchmaking.slippi.gg/v1/queue`: JSON text messages `join`, `joined` (ticket id and
  token), `status`, `match` (the old `get-ticket-resp` under another type), `error` (with
  `latestVersion`), `ping`, `cancel` and `resume` (slippi-rust-extensions, `matchmaking/src`).
- Timeouts: 5 s to connect, 10 s for the handshake, 3 attempts a second apart, the join
  acknowledged within 5 s, a `ping` after 5 s of silence, lost after 15 s, a lost ticket taken up
  again (`resume`) for 15 s.
- The service never sees the netplay UDP socket, so before joining the client asks STUN servers
  where that socket is seen from: `stun.l.google.com:19302`, `stun1.l.google.com:19302`,
  `stun.cloudflare.com:3478`, `global.stun.twilio.com:3478`, in that order until two have answered
  (3 requests each, 500 ms apart), and one request every 15 s while it waits in the queue. The
  join carries the port a server saw, the LAN address, both as "candidates", and the NAT's kind
  (`cone` when both servers saw the same address, `symmetric`, or `unknown`). No answer is not
  fatal.
- Slippi's hosts' certificates come from Google Trust Services ("WR3", under GTS Root R1; public
  certificate logs, 2026-11-15), for 90 days each, with a new key each time.

**Netplay:**
- A pad on the wire is 9 bytes, not 8: byte 8 is that frame's "RNG bits" (**both**). No packet
  carries a version, so only matchmaking keeps the two versions apart.
- `0x88`, a player's report that another has dropped, with the last inputs it has of it, 64 at
  most (**both**, in games of more than two). 300 ms after a player is lost, the newest input
  anyone had of it is its last; that input is repeated and 16 frames later its character leaves
  the game, on the same frame for everyone. Pads from a player that has left are discarded and it
  cannot connect again.
- `0x87`, an input suggestion: may be ignored, and is.
- ENet's packet throttle is turned off on every connection.

**EXI device:**
- `0xB0`: the request is 27 bytes (the input's source port; "is leaving", which disconnects). The
  answer is 559 bytes: after the pads, the frame each opponent leaves the game on (32-bit), a block
  of predicted inputs (each opponent's newest pad, seven times), and a random byte for the local
  player's next RNG bits.
- `0xB3`: the answer is 1030 bytes (rank information and "is a bot"); Party is a stock game of 8
  minutes like every other mode (**both**).

**Game codes:** 1,368 bytes more.
- The per-frame RNG seed goes through a mixing function and takes the "seed bits" the players'
  pads carry (**both**: it is the game's simulation).
- The code that sends and predicts inputs (`TriggerSendInput`, at `80376A28`) grew by a third: the
  RNG bits, and a mark on a pad the game is rolling back over. Its data block grew at its end only,
  so nothing the rollback engine reads moved.
- Two stage resets (Castle's wind, Kongo's log bridge) (**both**).
- `slpCSS.dat` is now `SlippiCSS_gui.dat`.

**Game files:** `GameSetup_gui.dat` and `SlippiCSS_gui.dat` no longer contain Nintendo's assets.
They hold links (366 and 14) of the form `slp:<disc file>:<root>/<path>` that Dolphin resolves from
the player's disc when it loads them (slippi-rust-extensions, `hsd`).

**Unchanged:** game reporting, the user service, the connect phase of a game, chat, the match-set
flow of Ranked, and the match-quality rule (SlippWii's was written from this same source).

## What was built

### Matchmaking over TLS

`mm_url` chooses it (`wss://host[:port]/path`, or `ws://` for a test server). In
`Nintendont/kernel/slippi_online`:

| File | What |
|---|---|
| `so_tcp.c` | one TCP connection through IOS, non-blocking, so every wait has a timeout and can be called off |
| `so_tls.c`, `so_tls_roots.inc` | TLS 1.2 with BearSSL 0.6 (the loader's updater uses the same library): ECDHE, RSA or ECDSA certificates, AES-GCM or ChaCha20-Poly1305 |
| `so_ws.c`, `so_ws_frame.c` | the WebSocket: the HTTP upgrade, masked text frames, pings answered |
| `so_stun.c`, `so_stun_io.c` | STUN binding requests through the session's UDP socket |
| `so_mm_ws.c` | the session: a worker thread that connects, joins, waits and takes a lost ticket up again |
| `so_mm_msg.c`, `so_mm.c` | the messages (byte for byte what Dolphin's client writes), and one interface for both services |

- **A worker thread.** A handshake is about a second of arithmetic on the Wii's ARM processor, and
  IOS's TCP calls take time, so none of it runs on a thread the game or the netcode waits for: the
  worker's priority (0x30) is below the kernel's main thread (0x50) and the netcode's threads, and
  it has a 16 KB stack on the online heap. The connection is closed as soon as the match arrives:
  nothing of it runs during a game.
- **The server is checked.** Its chain must end at one of six roots compiled in (Google Trust
  Services' four and Let's Encrypt's two: `tools/make_tls_roots.py` takes them from Mozilla's list
  by fingerprint) and its certificate must name the host. If Slippi moves to another authority,
  matchmaking fails with "Slippi server certificate not trusted" until a SlippWii update adds it;
  the loader's updater does not depend on that list.
- **A certificate's dates are not checked.** A Wii's clock is often wrong by years, and a console
  that cannot play because its clock battery died is worse than one that accepts a certificate
  past its date. (BearSSL 0.6 fails a chain when it has no clock; `tools/build_bearssl.py` changes
  that one place, on a copy of the pinned source, for the kernel's and the PC's builds.)
- **The play key** is in the join message only: built by the network thread, wiped once the
  service has it, never logged.
- **Room in the kernel.** Its code and data regions were both full. The megabyte the loader keeps
  for the kernel had 64 KB unused at its end, so the data and the stacks now sit 64 KB higher and
  the code region is that much larger (`kernel.ld`). With everything here built, 5.4 KB of code and
  1.9 KB of data are free.
- **The kernel's `memcpy` returns the end of the destination**, where C says the start
  (`utils_asm.S`). The kernel's own code is compiled so that the compiler never uses the value;
  BearSSL at first was not, kept the server's public key at the returned address, and failed every
  handshake at the server's signature, on a Wii only. BearSSL for the kernel is now compiled the
  kernel's way, and `tools/check_kernel_memcpy.py` (run by `tools/build.sh`) fails a build in
  which anything uses that value.

### Netplay and the EXI device

The kernel speaks the next version when the version it reports is over 3.6.4 (`so_config_next`):

- `so_pads.c`: a pad is kept as 9 bytes and written to the wire as 8 or 9 (`so_pad_wire`); 3.6.4's
  ninth byte is 0. The drop report's contents and what is taken from another's report
  (`so_drop_report_encode`, `so_drop_report_apply`), and a lost player's last input held
  (`so_remote_pads_repeat`).
- `so_netplay.c`: `0x87` ignored; `0x88` sent and taken as Dolphin does (`process_drops`,
  `on_player_drop`): the 300 ms, the frame the character leaves on, pads of players that have left
  discarded, their late connections refused; the throttle off.
- `so_exi.c`: `0xB0` and `0xB3` in both layouts, by the version; the predicted inputs block; the
  frames opponents leave on; "is leaving"; Party as a stock game. The random byte for the RNG bits
  comes from the TLS client's generator (`so_tls_random`): it must only be unpredictable to the
  player, and it is the same value for everyone once sent.

### The codes and the rollback engine

`tools/build_online_boot.py --next` (or a switched checkout) builds the engine against the next
codes: `tools/rollback/slippi_offsets.py` reads its offsets from the checkout
`slippi-ssbm-asm-next`. Two things changed for it:

- The pad the engine sends ahead of the game's own poll (`rb_early_pad`) carries the RNG bits the
  device handed the game for that frame.
- The engine's block of shared memory moved up within its region (the larger `0xB0` answer needed
  the room): the same addresses on both sides (`T_*` in `rollback.c`, `ENGINE_*` in `so_exi.c`),
  for both versions.

Rollback itself did not change: with rollbacks forced on every frame (`--rollback 2`), the Wii's
replays of games against the development Dolphin are identical to Dolphin's.

### The linked game files

`Nintendont/loader/source/hsd_links.c` resolves the links on the Wii from the player's disc, as
the loader already makes the patched menu files: a release ships `patches/<name>.link` (Slippi's
file, with no Nintendo data in it) and the manifest says what must come out (`link`, with the
size and CRC-32). `tools/hsd_links.py` is the same in Python, for the tools. Both give exactly the
bytes of Slippi's own `slippi-hsd resolve` for the two files (380 links), and each link carries a
hash of its asset, so a disc that is not the original is refused.

### A release

`tools/slippi_files.py release_files` puts a release's `slippwii/` folder together for either
version (it reproduces 1.6.1's files exactly for 3.6.4), and `tools/make_release.py` follows the
switch. `--trial DIR` makes a release's zip without signing anything, for tests.

"Check for Updates" writes a release over the installed files and deletes nothing. The loader
makes the files again when the manifest is another one, so a card goes from one version's files to
the other's on the first start; files only the old version had stay behind, unused
(`tools/test_loader_slippwii.py --zip-other`).

## What was proven

Nothing here contacts Slippi or the public STUN servers: the opponent is Slippi's own development
Dolphin (its GitHub Actions build of commit `7333463b`, "3.6.5-dev.1"), pointed at a test server
on this PC. A game counts as proven when the Wii's replay is identical, frame for frame, to the
replay of a Dolphin in the same game.

### Matchmaking on the Wii (2026-11-15)

One wired Wii, the test server on a PC on the same LAN, Melee at the online character select
screen while the worker runs.

| Certificate chain | Suite | TCP connect | TLS handshake | Longest socket call | Result |
|---|---|---|---|---|---|
| RSA 2048 under RSA 2048 under RSA 4096 (the shape of Slippi's hosts), plus the test root's signature | ECDHE-RSA-AES256-GCM | 13 ms | 817 ms | 63 us | matched; 2 games, both replays identical to Dolphin's |
| EC P-256 under EC P-384 | ECDHE-ECDSA-CHACHA20-POLY1305 | 14 ms | 1186 ms | 12 us | matched; 1 game, identical |

The worker used 6,076 of its 16,384 bytes of stack.

### Games against the development Dolphin

The Wii against Slippi's development Dolphin unless a row says otherwise; "identical" is every
frame of the Wii's replay against the Dolphin's.

| What was run | Result |
|---|---|
| Two development Dolphins through the test server (the rig's own check) | matched and played; replays identical |
| One opponent, the game waiting for inputs (no rollback) | identical |
| One opponent with rollback, a rollback forced on every frame | identical; 2,336 rollbacks in 2 games |
| The same with the game's files made on the Wii by the loader (patches and links) | identical |
| The controller in port 2, rollbacks forced | 2 games, identical (the bug below) |
| A build switched to the next version (`SO_SLIPPI_NEXT 1`, as a release will be) | 3 games with 3,816 rollbacks, identical; Teams, 6 of 6 replays |
| **One opponent, an hour**: 45 ms and 60 ms simulated connections (jitter, 1% loss on one) | **60 games, all identical**; 30 more before the review's fixes |
| Teams, 15 games in 5 lobbies, two of them over a simulated connection | 45 of 45 replays identical to the first Dolphin's |
| Teams on Yoshi's Story, the Wii as Ice Climbers (the heaviest scene the bots reach), a PC mirroring the console, the replay stream in pieces (as it is now) | 35 of 35 games played through, all replays identical |
| The same with no PC mirroring the console, or the stream held for the game | 24 of 24 games played through: the Dolphins' replays identical, the Wii's checksums agreeing with them (its replay does not reach the PC in these) |
| The same with the replay stream as it was, a send a frame | 20 of 60 games collapsed (below); the 40 that finished identical |
| Teams, a Dolphin closed in mid-game | 4 of 5 runs: the three that play on identical to the end, the same freeze as Dolphin's own (about 7.4 s); 1 run failed (below) |
| Party, the Wii's player quits (L+R+A+Start) | 2 runs: the Wii leaves alone, every Dolphin takes its character out from the same frame and plays on; a third collapsed (below) |
| **Slippi 3.6.4 with the same build** (what a release does today) | one opponent with rollbacks forced, 3 of 3 identical; Teams, 6 of 6 |
| The same builds once more on the last commit | 3.6.4: 2 of 2; next: 2 of 2 with rollbacks forced, Teams 6 of 6 |
| **Slippi 3.6.4 from this branch, a long soak** (2026-11-16, 8 hours) | one opponent over four simulated connections (one-way delays of 15 to 60 ms with jitter, 1% loss on the slowest, which is a 135 ms ping): **320 games, all identical**; Teams and Party (a PC mirroring with the stream in pieces, on a LAN and at 63 ms; Party with no mirror): 17 lobbies, 51 games, all identical, none collapsed |
| The branch against 1.6.1's own source, by turns, the same games and inputs (2026-11-17) | at 135 ms with 1% loss, 5 runs each; with every frame rolled back 3 frames, 2 runs each: 140 games, all identical. What the PC player gets is the same from both. The Wii's own picture is level within what the runs can tell: refreshes with no new frame 21.6% against 1.6.1's 20.7% (one run to the next moves 1.5; game by game with each game's rollback depth taken out, +0.05 with a standard error of 0.7), and 58.8% against 58.2% (standard error 1.2). The kernel's own timings are the same, and the game's side is the same code but for three addresses |
| **A player's kernel** (`test_control=0`, no test rescues), 3.6.4 and next: boot, the Online Play menu, Exit | both back at the Homebrew Channel by themselves in 4 s |

### What the tests missed, and what found it

An independent read of the game-side code against Slippi's source (2026-11-16) found what no run had:

- **"Is leaving" was read from the byte before it**, which is the controller's port. From port 1
  that byte is 0, and the test bot only ever played from port 1: every run passed. From port 2, 3
  or 4 every game on the next version would have ended at its first frame. Fixed, proven from
  port 2 on the Wii, and the cause closed: `tools/check_exi_layout.py`, run with every build,
  holds the kernel's 16 offsets and sizes of these buffers to the numbers in Slippi's own source,
  for both versions. `run_sweep.py --wii-port N` moves the bot.
- Smaller ones, all fixed: the order of two steps when the console disconnects (as Dolphin's), a
  report written for a player who left by choice, a 9-byte pad copied into an 8-byte buffer in two
  test paths, Slippi's hosts recognised in any letter case, and two places where 3.6.4's
  behaviour had moved by a byte of a log line.
- And while switching the checkout back and forth: the committed boot codes still held the
  rollback engine from before its shared memory moved. Test builds always rebuild them, so only
  `tools/build.sh` had the old ones, and `make_release.py` would have refused them.

### On the PC (the same code built for Windows)

`python tools/online_selftest.py pc --next-mm ...`, about 15 s a run, each ending in a game of
synthetic inputs against the 3DS fork's test opponent: without TLS; with it, an RSA chain and an
EC chain; a server that offers one suite only (ChaCha20 with RSA and with ECDSA, AES-128-GCM); a
server that offers TLS 1.3 as well; a root the client does not have (refused, "certificate not
trusted"); a certificate for another name (refused); a connection cut 3 s after the join (the
ticket taken up again, then matched); STUN servers that report different ports (`symmetric`) and
that do not answer (`unknown`, still matched).

Unit tests (`tests/run_tests.sh`, 727 checks, run as a build for each version): the messages,
frames and STUN, the join compared with slippi-rust-extensions' own example; 9-byte and 8-byte
pads; the drop report and what is taken from one; the switch's defaults, and that a test's server
is never Slippi's in either build.

`tools/test_hsd_links.py`: the links resolved by the loader's C, by the Python and by Slippi's
tool, all the same; a disc without the files, another disc's files and damaged files refused.
`tools/test_loader_slippwii.py`: the loader's file making on a PC, with a trial release of the
next version, including an update over 1.6.1's files and back.

## How to run the tests

- `tests/pc/pc_lobby_mm.c --ws-port N`: the test matchmaking server speaks both services and
  pairs their players. `--latest V` turns older versions away as Slippi's does; `--ws-drop-after S`
  cuts each WebSocket once.
- `tools/sweep/tls_front.py`: TLS in front of it, with certificates under a root made for the run,
  which a test build trusts by its settings (`test_tls_root`; never honoured against a slippi.gg
  address). Python listens because Windows Firewall lets its traffic in.
- `tools/sweep/stun_fake.py`: the STUN servers.
- The development Dolphin: unpacked into `build/next/dolphin-dev`. A test copy of it
  (`tools/sweep/dolphin_copy.py`) takes its matchmaking address from `SLIPPI_MM_URL`, and every
  Slippi and STUN name in it is patched to one that does not resolve, or to this PC.
- `python tools/sweep/push_next_files.py --wii-ip IP`: puts the next version's release files on
  the Wii's card in `/slippwii-next`, beside the player's own, and has the loader make them.
- `python tools/sweep/run_sweep.py --wii-ip IP --next [--rollback 2] [--net 30,5,2]`: one opponent.
  `--next-mm` alone: the Wii queues the new way against Slippi Dolphin 3.6.4.
- `python tools/sweep/lobby.py --mode teams|party --next --wii-ip IP [--drop 3,20 --rounds 1]`:
  four players; without `--wii-ip`, four Dolphins (the reference). `--stages 8` puts every game on
  one stage, `--teams 1,1,0,0` sets the teams, `--no-mirror` leaves the Wii's replay stream off,
  `--kernel multi_stream=1` sends it a frame at a time as before, `--kernel multi_stream=0` holds
  it for the game with the PC connected (the Wii's replay is then not among those compared; its
  own checksum report says whether it agreed), `--wii-old-mm` has the Wii queue the old way into
  the same next-version game.
  Party ends when the Wii's bot has quit and every Dolphin has taken its character out.
- `run_sweep.py --wii-port N`: the Wii's bot plays from another controller port.
- Two commits against each other: build each once (`run_sweep.py --build-to DIR ...`, the older
  one from a clean checkout of it), then run them by turns from copies of those directories
  (`--skip-build DIR`) with one `--seed` and `--shift`. Soaks are not comparable game for game
  (`soak.py` gives every run another plan, and the stage of the heaviest game moves a run's
  numbers more than a build does), and on a rollback connection the soak summary's "long
  (6+ frames)" stalls are the profiler counting a rollback's frames, not time lost.
- `python tools/check_exi_layout.py` (in every build): the kernel's buffer layouts against Slippi's.
- `--kernel test_logfile_dump=2` on the next start: the newest report on the Wii's card, read back
  into the run's `wii.log` (`card>` and `card+` lines), when a console's log to the PC had stopped.

## Not proven, and open

- **Four players while a PC mirrors the console: games collapsed with the replay stream as it
  was; it now goes out in pieces, and one thing about that is read, not seen.** The stream is
  Slippi Nintendont's own (`SlippiNetwork.c`, TCP port 51441): a player has it only while a PC
  mirrors the console, and the test rig always had it. It was one send a frame. In a collapse the
  Wii stops answering pings in mid-game and the stream stalls, its game runs at about half speed
  for some 20 s, and by then every connection has timed out. The console's network comes back by
  itself (after up to 14 minutes), or at once when the player exits. It is not a desync: every
  frame the replays share is the same, and the Wii's own reports (read back from its card,
  `test_logfile_dump=2`) have its checksums agreeing.
  - Counted over every four-player game on Yoshi's Story with the Wii as Ice Climbers since
    2026-11-14 (the batches of 2026-11-16 run by turns, so that each way met the same hour):

    | The stream to the PC | Collapsed | Frames the Wii waited for inputs in a 40 s game | Ping between the Wii and its opponents |
    |---|---|---|---|
    | Next version, a send a frame (as it was) | 20 of 60 | 108 | 77 ms |
    | Next version, one send in 100 ms (`multi_stream=100`, the default now) | 0 of 19 | 6 | 18 ms |
    | Next version, one send in 50 ms; in 250 ms | 0 of 10; 0 of 6 | 12; 15 | 25 ms; 25 ms |
    | Next version, held for the game (`multi_stream=0`) | 0 of 13 | 0 | 9 ms |
    | Next version, no PC connected | 0 of 11 | 0 | 8 ms |
    | 3.6.4, a send a frame | 0 of 17 | 25 | 67 ms |
    | 3.6.4, one send in 100 ms | 0 of 2 | 0 and 11 | 14 and 16 ms |
    | 3.6.4, held for the game | 0 of 1 | 0 | 8 ms |
    | 3.6.4, no PC connected | 0 of 15 | 0 | 8 ms |

    (medians of the games that finished). On the other stages the next version with a send a
    frame collapsed in 0 of 29 games; 3.6.4 with four players, in 0 of 233 on any stage. So a
    send a frame costs both versions on that stage, and the next version falls over where 3.6.4
    slows.
  - Ruled out: the test Wii's cable (looked at; the router answers throughout while the Wii does
    not); the bots' play (one seed in every batch); the stage's Shy Guys and the size of a frame
    (1 to 3 items alive at the collapses, 11 in games that finished); the TLS matchmaking session
    (the Wii queueing the old way into the same game, `lobby.py --wii-old-mm`: 3 of 12 collapsed,
    against 5 of 12 by turns).
  - What is seen of the cause: the Wii's report of a collapse has its sends at their usual speed
    (2.8 ms a datagram) while it waits 400 frames and more for inputs. What starves is what it
    receives: the opponents' inputs and pings, the acknowledgements of its own stream. The stream
    was one TCP send a frame from a thread of its own, beside three opponents' datagrams, all
    through IOS's one network module. What costs is the number of sends, not the bytes: the same
    data in one send every 100 ms leaves about a twentieth of the waiting (50 ms and 250 ms are
    both worse than 100). What inside IOS gives way, and why the next version's game (which waits four
    times as many frames with a send a frame) tips it over, is not known.
  - The fix (`multi_stream` in `so_config.c`, used in `SlippiNetwork.c`): in a game with more
    than one opponent the stream goes out as one send in 100 ms. The PC gets the whole game while
    it runs, and the Wii's replay from it is Dolphin's frame for frame in all 37 such games. A
    game against one opponent, offline play and the replay file on the card
    (`SlippiFileWriter.c`) are not touched. What is left of the cost: 6 waited frames in a 40 s
    game and 10 ms on the ping the opponents read, against none without a mirroring PC; in two of
    the 30 games at 50 or 100 ms the Wii once waited 7 frames in a row.
  - Two other settings, for `config.ini`: `multi_stream=0` holds the stream for such a game (the
    PC stays connected by the keep-alives, the game is passed over whole, and afterwards the
    stream goes on from where the buffer is, said to the client as an overflow is: proven in 14
    games, and the way back with every other game held in a run against one opponent). It costs
    nothing and shows nothing: the mirroring PC has no picture of a Teams or Party game. And
    `multi_stream=1` is a send a frame, as it was.
  - Read, not seen: what a mirroring Slippi Dolphin shows of a game that arrives 6 frames at a
    time. By its source (`prepareFrameData`) it waits for a frame that is not there and, in its
    default mode, never runs fast after the start: the picture should be as before, about 100 ms
    later. With the Launcher's "real-time mode" on it runs fast whenever it is more than 2 frames
    behind, so there the picture of a Teams or Party game may judder. The test rig's mirror is a
    script; nobody has looked at Slippi Launcher's.
- **The real service has never been contacted.** Its TLS settings are inferred from who issues its
  certificates (Google's front ends take TLS 1.2 with the suites here). A first contact is
  Slippi's to allow, or the release day's.
- **The release may differ from the development source.** Everything was read from one commit;
  step 1 above is there for that.
- **A client name.** The WebSocket's upgrade request could carry one (a `User-Agent`); nothing is
  sent until Slippi says where they want it.
- **The seed of the handshake's random numbers** is gathered from timing (the timer at the moments
  of the player's actions and of each network answer) and the session's generator. It is not a
  hardware random source; what it protects is the play key in transit, which today's matchmaking
  sends unencrypted.
- **Wi-Fi, a second console, real routers:** not looked at for the new path (STUN's answers
  matter there; on the LAN they are the console's own address).
- **A late drop report.** When a player's connection is lost to one opponent well before the
  others notice, reports can arrive after the 300 ms; Dolphin has the same window, and the
  checksums then say so. Not provoked here: the tests' lost player is lost to all at once.

## Only Slippi can say

- When the next version is released and under what number; whether the old and the new
  matchmaking run side by side for a while.
- The TLS settings of the real service, and whether a console client may connect to it at all.
- Whether it wants a client name, and where.
- How it uses the candidates and the NAT's kind, and what it tells an opponent when STUN failed.
- Whether a test service (or `matchmaking-dev`) can be used.

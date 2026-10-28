# Slippi online on a real Wii: feasibility and plan

*Research of 2026-10-05. Sources: Slippi Nintendont (branch `slippi`, v1.13.1), slippi-ssbm-asm, Slippi Dolphin (`EXI_DeviceSlippi.cpp`, `SlippiMatchmaking.cpp`, `SlippiSavestate.cpp`), and our 3DS Slippi client (`slippi-3ds`).*

## Verdict

**Feasible.** The recommended route is to **extend Slippi Nintendont**:
- Implement Slippi Dolphin's *online* EXI commands in Nintendont's ARM kernel, with **lockstep instead of rollback**.
- Run Slippi's own, unmodified online game code (the "Slippi Online" codeset) on the Wii.

The PC opponent runs ordinary Slippi Dolphin and keeps its rollback, exactly as with the 3DS beta.

A custom ISO alone cannot work. Melee has no network code, and in GameCube mode the game cannot reach the network. Only Nintendont's ARM kernel can, through IOS. A pre-patched ISO is optional, as one way to deliver Slippi's menu files (see M4).

Nobody has shipped this yet:
- Slippi feedback items #107 ("Play online on console") and #581 ("Rollback on Wii") are open.
- Fizzi called *rollback* on the Wii "incredibly difficult… might not be possible".
- This plan avoids rollback on the Wii altogether.

## Why lockstep makes it possible

Slippi's online ASM (`Online/Core/TriggerSendInput.asm`) sends command `0xB0` every frame and branches on the device's reply:

| Reply | Game does |
|---|---|
| 1 NORMAL | advances with the remote pad the device returned |
| **2 SKIP** | **does not advance this frame**; asks again at the next poll |
| 3 DISCONNECTED | ends the match |
| 4 ADVANCE | runs an extra frame (catch-up) |

The game only predicts inputs, captures savestates (`0xB1`) and rolls back (`0xB2`) when the device returns a frame *without* the opponent's real input. A device that answers **SKIP until the confirmed remote pad has arrived** never triggers prediction, savestates or rollback.

This matters because Dolphin's savestates are about 8 MB each, 7 deep (about 57 MB). They are impossible on a Wii, and with lockstep they are never requested.

- **Same approach as the 3DS beta.** The 3DS client works this way and stays in sync with Slippi Dolphin, and the PC tolerates a peer that never rolls back.
- **No deadlock.** The PC runs up to 7 frames ahead of the last input it has from the Wii, so it always produces the pad the Wii waits for.
- **Better than the 3DS:**
  - **Same CPU family.** The Wii runs the original PowerPC game code (the 3DS port is a recompile for ARM), so floating-point desyncs are far less likely. This is still to be confirmed; console replays can't prove it because Slippi playback re-syncs positions every frame.
  - **Same refresh rate.** The Wii runs at 59.94 Hz, like Dolphin, so there are no dropped frames for time sync.
  - **Same inputs.** GameCube controllers plug straight in.

## What already exists in Slippi Nintendont

- **Slippi EXI device emulation.** The game's EXI DMA/Imm/Select calls are patched to a mailbox, and the PPC spins until the kernel answers. Today it is write-only (replay recording), but Fizzi's `hack/console-playback` branch already answers per-frame EXI **reads** from the kernel. That is the mechanism online play needs.
- **Networking in the kernel** (`kernel/net.c`, a libogc port over IOS sockets): TCP and **UDP** (the console-mirroring broadcast already uses UDP), poll, local IP, and threads.
- **Code injection:** prebuilt Gecko binaries (`kernel/gecko/`) and a codehandler.
- **Config and files:** a loader UI on the PPC, FatFS on SD/USB, and a config file.

## The work

| # | Milestone | Main tasks |
|---|---|---|
| **M0** | Build and baseline | Build Slippi Nintendont (Docker image `nikhilnarayana/devkitpro-slippi` or devkitPro), and boot it on the user's Wii unchanged. |
| **M1** | Measure the two unknowns | A test build with **(a)** UDP round-trip time Wii ↔ PC (Wi-Fi and USB LAN) and **(b)** EXI request/response latency, using a tiny Gecko test code that does a per-frame read like `0xB0`. Results go to a log on SD/USB. Note: the kernel's TCP `sendto` was measured at about 10 ms in a source comment. |
| **M2** | Kernel networking | Extend `net.c` (port from libogc / FIX94's `bbatest` branch): `sendto` with a destination, `recvfrom` with a source, non-blocking I/O, DNS. Port **ENet** with an IOS backend (we wrote a 3DS one) and our **matchmaking + P2P + JSON** C code from the 3DS client. A network thread pre-computes each frame's answer, so the EXI handler never blocks on the network. |
| **M3** | Online EXI device | Answer the Direct-flow commands: |
| | | `B0` inputs (lockstep: SKIP until the remote pad arrives), `B3` match state (port `prepareOnlineMatchState`, about 962 bytes), `B4` find opponent, `B5` selections, `B9` user/login (from `user.json`), `BA` cleanup, `BC` seed, `D5` delay, `E1`/`E2` premade text, `C3` chat strings, `BE` code suggestions. `B1`/`B2`/`BD`/ranked commands become no-ops. |
| | | It must also keep parsing (and may discard) the recording stream. |
| **M4** | Codes and files | **Codes:** the online codeset is about 58 KB (Nintendont's Gecko area is about 40 KB), so use Slippi's **bootloader** route (`D3`/`D4`, the kernel serves the GCT) or patch the DOL. Build it from `netplay.json` for console, **without the two music-mute codes**: on PC, Dolphin plays the music instead; the Wii should let the game play it. |
| | | **Files:** serve `D1`/`D2` file loads (slpCSS.dat, SlippiCSS.dat, GameSetup*.dat, MxDb/MxScn) and the patched menus from SD/USB. A PC tool (like our 3DS `slippi_files.py`) prepares them from the user's own disc. |
| **M5** | Play | Direct match on LAN, then over the internet. Desync detection uses the per-frame checksum the ASM already sends in `B0`. Compare the PC's replays. |
| **M6** | Ship | Loader options (Slippi Online on/off, delay), `user.json` setup, docs, packaging. |

## Risks (in order)

1. **Wii network latency.** The IOS UDP round-trip time is unmeasured. If it is high, it eats the delay budget and the Wii stalls more. M1 answers this first; a USB LAN adapter will likely be recommended.
2. **EXI answer latency.** `0xB0` runs inside the VI retrace callback with interrupts off. The kernel must answer within a fraction of a millisecond, and **never wait for the network there**: if the input hasn't arrived, it answers SKIP at once.
3. **The ARM is shared.** IOS's Wi-Fi/IP stack, USB and disc I/O, replay writing and our ENet all run on one ARM core at 243 MHz. CPU use is unprofiled.
4. **Kernel memory.** The heap size is unknown. The 3 MB replay ring buffer can be shrunk to make room.
5. **Testing needs real hardware.** Dolphin cannot emulate Nintendont's ARM kernel, so every iteration is: build → copy to SD → test on the user's Wii → logs back. Expect more round trips than the 3DS (no emulator).
6. **Wii/Dolphin determinism** is likely but unproven; M5 measures it.
7. **The Slippi team.** Use Direct only, never Ranked or Unranked queues, and give them a heads-up before release (as for the 3DS beta).

## Feel compared with Slippi on a PC

The same as the 3DS beta: no rollback on the Wii, so a late input means a brief stall instead of a rollback. On LAN and good connections, delay 2 should rarely stall. On higher ping, a bigger delay helps. The PC side keeps rollback.

## What the user needs for testing

- A Wii (or a Wii U in vWii mode) with the Homebrew Channel, and Slippi Nintendont working today.
- SD card or USB drive, a GameCube controller, and the NTSC 1.02 ISO.
- Preferably a **USB LAN adapter** to compare with Wi-Fi.
- A PC with Slippi Dolphin, and a second Slippi account for the Wii.

## M1 results (hardware, 2026-10-05, Wi-Fi, Versus vs CPU, ~7,000 in-match frames)

| Measurement | Result |
|---|---|
| Game stall per frame for one write + read with the kernel (like command 0xB0) | **~1.4 ms** (write 0.15 ms, read 1.25 ms; max 2 ms, one 7 ms outlier). **0 slow frames.** About 1 ms of it is Nintendont's emulated EXI interrupt delay (`EXI_IRQ_DEFAULT`, 1900 Starlet ticks), which can be made instant for our device. |
| Wii timebase | 60.75 MHz (1,012,500 ticks per frame) |
| UDP round trip, best case (Wi-Fi) | **4.6–4.9 ms** |
| One blocking IOS `sendto()` from the kernel | **~16 ms** median (23 ms p95, up to 150 ms) |
| Round trip with one thread doing send + poll + receive at 60 Hz | Climbs to 0.4–4 s: the thread cannot keep up, so replies queue in IOS |
| `sendto`/`recvfrom` with addresses (new in net.c) | Work: `addr_send` returns bytes sent, `addr_recv=1` |

**Conclusions:**
- The game side is fine.
- The network path is fast, but each blocking IOS socket call takes about a frame. One thread doing blocking calls cannot keep up at 60 packets per second.
- The client must use asynchronous IOS calls (`IOS_IoctlvAsync` with a completion queue, as in FIX94's `bbatest` branch) and a dedicated receive thread.
- M1b measures that design.

## M1b results (hardware, 2026-10-05, Wi-Fi)

- **EXI** with an instant interrupt for our device: **0.39 ms per frame** (write 0.15 ms, read 0.24 ms). 0 slow frames in ~6,700.
- **Blocking sends at 60 Hz, with a separate receive thread:** `sendto` takes 17–18 ms; round trip p50 30–66 ms (min 4.7 ms).
- **Async sends at 60 Hz:**
  - issuing a send takes 0.04 ms;
  - IOS completes each one after 90–135 ms (p50), with all 8 slots full and some sends dropped;
  - completions run at about 52 per second, and the round trip grows to seconds (backlog).
- **Conclusion:** on Wi-Fi, IOS sends about 50–55 UDP packets per second, about 17–18 ms each, whether called blocking or async. One input packet per frame each way (60/s) does not fit.
- **Options:**
  - send every second frame (Slippi packets carry several frames of input);
  - try the USB LAN adapter, since the per-packet cost is likely the Wi-Fi driver.
- **M1c** measures receive capacity (PC → Wii at 60/s) while the Wii sends at 30/s and 15/s.

## M1c results (hardware, 2026-10-05, Wi-Fi; phases ran in menus and during a match)

| Phase | Result |
|---|---|
| Receive only, PC streams 60/s | All received; gap p50 16.6 ms, p95 24 ms |
| Wii sends 15/s + receives 60/s | All received (gap p95 ~39 ms); round trip p50 ~35 ms, p95 50-130 ms; `sendto` ~30 ms |
| Wii sends 30/s + receives 60/s | Round trip p50 150-240 ms: saturated |
| Wii sends 30/s only | Round trip p50 ~18 ms, p95 ~30 ms; `sendto` ~17 ms |
| **Stall** | When the second match loaded, the Wii received **nothing for ~57 s** (no errors) while its sends kept working and the PC echoed everything. Shorter stalls (1-8 s) also appear in M1 and M1b. |

**Verdict for Wi-Fi:** IOS manages about 50-90 packets per second in total, and receiving stalls for seconds at times. Lockstep needs about 60 packets each way per second, steadily, so Wi-Fi does not look playable.

**Next:** the same M1c build over the USB LAN adapter (Wii Settings: Wired Connection). If wired is clean, continue with M2. If not, consider networking from the PowerPC side (libogc) instead of the Nintendont kernel.

## M1c over the USB LAN adapter (hardware, 2026-10-26; two match loads)

| Phase | Wired result |
|---|---|
| Wii sends 30/s + receives 60/s | Round trip p50 11.5-12.5 ms, p95 13 ms; no loss |
| Wii sends 30/s only | Round trip p50 8.3 ms, p95 9-12 ms |
| Receive only, 60/s | Gap p50 16.9 ms, p95 17.3 ms; all received |
| `sendto` | 8-11 ms (blocking) |
| Stalls | None: the worst gap is ~0.2 s. The 20 s gaps are the stream-off phase. |
| EXI per frame | Read 0.23 ms; 0 slow frames in 7,000+ |

**Verdict: feasible over the LAN adapter.** One-way latency of about 6 ms fits well inside Slippi's 2-frame delay. Wi-Fi is not viable, so the beta requires a LAN adapter, as Slippi already recommends for console mirroring.

**Still to measure, in M2:** 60 sends per second plus 60 receives per second (the real per-frame load). If sends are too slow at 8-11 ms each, send every second frame with two frames of input per packet.

## M2: the network client on hardware (2026-10-26)

`Nintendont/kernel/slippi_online/` (see its README) implements Slippi's
matchmaking and peer-to-peer netplay in the kernel. Hardware self-test
against the 3DS fork's fake matchmaking server and Dolphin-like peer
(dummy accounts, LAN, USB adapter):

| | Result |
| --- | --- |
| Matchmaking (create-ticket, queue, match) | works |
| P2P connect, duplicate-connection handling, selections | works |
| 3600-frame lockstep match | 60.05 s, 1 wait, 0 pad mismatches |
| Ping (Wii / PC peer) | 3.2 ms / 3.8 ms |
| Peer (Dolphin-like) | 3602 pads verified, 0 rollbacks, 0 time-sync halts |
| IOS sendto | ~1 ms average |

What it took (each found on hardware):
- **Freeze at matchmaking start.** `DIinit()` zeroes the Triforce DIMM region
  0x12B80000-0x12E80000 at the DI_INIT boot stage for every game. The
  client's heap (0x12E00000) was built earlier, so its first allocation walked
  a zero-size header for ever at high priority and starved the kernel (music
  looped, game froze). The client now starts after `SlippiMemoryInit`; heap
  walks validate headers.
- **Slow networking.** The kernel's main loop (priority 0x50, 20 µs sleeps)
  starves IOS's network module: round trips 39 ms, sendto up to 11 ms. With
  the main thread at 0x30: 2.8 ms and 0.9 ms. The client lowers it while
  online and restores it afterwards (`online_main_priority`). The main loop
  still runs ~8,400 passes/s during play.
- Asynchronous IOS socket requests turned out not to be the problem, but the
  client now uses blocking sends from a sender thread (as Nintendont does).
- `make -j8` races on the generated gecko headers; builds use one job.

Workflow: the SD card stays in the Wii; `tools/online_selftest.py run --boot`
sends each build to the Homebrew Channel (`tools/wii_boot.py`, wiiload) with
Nintendont's config set to auto-boot, serves the test plan, and records the
Wii's state from Slippi's LAN broadcast and its log.

Next: M3, the online EXI commands, so Melee and Slippi's online codeset drive
the client.

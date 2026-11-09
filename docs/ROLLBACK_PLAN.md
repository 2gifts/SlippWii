# Rollback: goal, baseline, tests, code review and plan

Proposal, 2026-11-06. Nothing here is decided until you approve the goal (section 1) and the decisions in section 7. No game code changed for this review. Findings come from five independent reviews, today's PC vs PC runs and 467 archived test runs. I checked the most important claims against the code myself.

**Terms.** A *refresh* is one 16.7 ms screen update. A *repeat* is a frame shown for two refreshes, and a *jump* is a frame skipped. *Ping* is the round trip on the wire. A *rollback* re-runs frames with the real input. Connections are written `30,5`, meaning 30 ms each way with up to 5 ms of jitter, so about a 60-70 ms ping.

## The short version

1. **Don't start over.** The foundation is sound: games stay in sync frame for frame, and the savestate core and network protocol code are good. Three parts need replacing: frame timing, the game's per-frame wait on the Wii's ARM processor, and the time sync logic. Test code also has to leave the player's path.
2. **Most of the stutter you felt isn't rollback being slow.** Of 29 repeats in your 92 seconds of play, 24 look like Slippi's own "skip a frame when no buffer is free" rule firing by mistake. Two independent reviews found this, and I confirmed the code. My early controller read made it common. A real fix has not been run yet.
3. **We've been measuring the wrong things.** Nothing measures the actual picture, the actual input lag or the sound. The bots change inputs far less than a person does. PC rollbacks ran about 3 times higher against you (939 a minute) than against the bot (314-332). Several "all clear" results can't fail when they should.
4. **The goal looks reachable, but that is not proven.** The analysis says zero repeats with lag close to PC is plausible up to about 160 ms ping. That comes from a cost model, not a test.
5. **The order changes:** agree the numbers, build tools that can't lie, then fix. You shouldn't need to test by hand until the tools say a build has passed.

## 1. The goal

> At Slippi's standard 2 frames of input delay, up to 160 ms ping, a Wii player's game looks and responds like Slippi on PC, and a PC player can't tell from the game that the opponent is on a Wii.

| Connection band | Ping | Status |
|---|---|---|
| A: regional | up to 66 ms | Must meet every target |
| B: out of state | 66-100 ms | Must meet every target |
| C: long distance | 100-160 ms | Target, confirmed or revised after phase 4 |
| D: very far | over 160 ms | Stays playable and stable; the Wii may raise its own delay (decision 5) |

These targets are proposals for you to adjust. "PC vs PC" is measured today unless noted. "Wii today" comes from the current build with the bot unless noted.

### The Wii player

| What's measured | PC vs PC | Wii today | Target |
|---|---|---|---|
| Repeats and jumps in play | No frame over 30 ms in 71,900 frames (about 20 min); longest 29 ms | 35 ms ping: 0 in 2 games. 70 ms ping: 3-17 per 35 s. **You at 70 ms: 29 in 92 s.** 100 ms ping: 22-27 per 36 s | **0 in 30 min per stage group, on the real video output, with person-like inputs** |
| Button to light, mid-screen | About 57 ms at 144 Hz, 52 at 300 Hz (published, not your setup). Console on CRT: 56-62 ms | **About 67 ms, estimated, never measured** | **60 ms or less on a CRT**; final limit set after measuring your PC on the same rig |
| Lag steadiness | No drift by design | Unknown | Steady through a game |
| Visible corrections (how far characters snap) | 60,8 with 1% loss: 4.5 a minute over 5 game units | Not measured for the Wii player | No more than PC at the same ping |
| Sound gaps, doubled or lost effects | Not measured | Stand-in measure only | No more than PC |
| Stadium changes, GAME!, pause, start of game | Not measured | A hitch at each Stadium change; extra repeats on GAME! | No hitch; same length as PC |

### The PC player, against a Wii vs against a PC

| What's measured | PC vs PC | Wii today | Target |
|---|---|---|---|
| Reply time to an input packet | 0.05 ms | **5.1 ms** (10-90%: 3.5-7.2) | 3 ms or less |
| Extra input lateness, as one-way delay | 0 | About 2.5 ms from reply time alone; more at high ping | **2 ms or less** (decision 1) |
| Rollback depth (frames re-run) | 45,5: 1.1. 60,8,1: 2.2 | 45,5: 1.7. 60,8,1: 2.9 (older build) | Within 5% at every delay |
| Rollback rate | 30,5: 231 a minute. 45,5: 662 | 30,5: 243-314. 45,5: 587 | Within a measured margin of PC vs PC |
| Visible snaps over 5 game units | 60,8,1: 4.5 a minute | **14.0 a minute** (3 games, so a warning sign rather than a verdict) | At most 10% above PC |
| Freezes during play | 0 | 0 | 0 |
| Wait at game start | **1-3 frames** | **30-37 frames** | At most 5 frames above PC |
| Evenness of input arrival, 1%-99% | 14.6-18.5 ms | 12.5-20.6 ms | Within 1 ms of PC |
| Forced clock corrections, "poor connection" warning | None seen | PC's 2-frame jump in 4 of 20 games (older build); warning risk | None |

### Reliability

| What's measured | Target | What it proves |
|---|---|---|
| Desyncs, crashes, hangs, disconnects | 0 in 500 games | Under 0.6% of games, at 95% confidence |
| Black screens and boot hangs | 0 in 200 boots, cold and warm | Under 1.5% of boots |
| Long session | 8 hours with no drift or memory growth | Stability |
| Final check | All of the above on the exact build players get | Not just a test build |

"Zero" can never be proven outright. Zero events in N only shows the true rate is under about 3/N. That's why 30 clean minutes means "at most 1 per 10 minutes". An optional 15-minute session with injected hitches would show what you can actually perceive, and could tighten this.

## 2. Why Slippi's rollback works, and the PC baseline

You had two parts right, and there is more to it.

**1. The picture never hitches. This is the big one.** Re-run frames are never drawn, and they cost a PC so little that frame timing barely moves. Even at 150 ms ping, frames stayed within 9-24 ms, with no frame over 30 ms. On a Wii, a frame that misses its refresh costs a full extra 16.7 ms, which is exactly the hitch you see.

**2. Input delay hides most of the network.** Two frames hide about 66 ms of ping with no rollbacks at all. Each extra 33 ms of ping adds about one re-run frame. Slippi's FAQ recommends 2 frames up to 130 ms.

**3. "2 frames feels like normal" is roughly right, for a specific reason.** Slippi's FAQ says it removes about 1.5 frames of Melee's visual delay, so adding 2 frames lands very close to a CRT. A 120 Hz+ monitor removes about another half frame. Part of that saving appears to come from how Dolphin presents frames, which a Wii can't copy because its video output has fixed timing.

**4. "Inputs are still sent during lag" is correct, with limits.**
- Reading and sending the input runs in the screen refresh interrupt, so it continues even when the game engine falls behind.
- Every packet repeats all unconfirmed inputs, so a lost packet is covered by the next one.
- During a freeze at the rollback limit, packets continue but only resend unconfirmed inputs.
- During a rollback, a new read waits until the rollback ends. Our Wii reads and sends the input on time anyway, which is better than PC here.

**5. Clocks stay in step without visible corrections.** Dolphin nudges its speed by up to +1% or -0.5%. It only holds frames back in the first 2 seconds, and only jumps ahead if it falls over 26 ms behind. A Wii can't nudge its speed the same way, and this is the source of several PC-side gaps.

**6. Almost any input change causes a rollback.** Prediction just repeats the last input, and stick values must match exactly. A person changes input on 51-63% of frames and the bot on 18-20%. So at 70 ms ping against a person, rollbacks hit about 43% of frames, sometimes for seconds at a time.

**7. Smaller things that add up:** a 7-frame rollback limit; sound effects that aren't replayed on re-runs; a code that removes Fountain of Dreams' effects so it runs faster; a stick deadzone that stops resting noise from causing rollbacks.

### Measured today: PC vs PC, same bot, 3 games per connection

| Connection | Rollbacks a minute | Frames re-run | Deepest | Freezes in play | Start wait (frames) | Frame time, 1%-99% | Longest frame |
|---|---|---|---|---|---|---|---|
| 5,1 | 0 | - | - | 0 | 0.7 | 14-19 ms | 29 ms |
| 15,3 | 0 | - | - | 0 | 1.0 | 14-19 ms | 25 ms |
| 30,5 | 231 | 1.0 | 1 | 0 | 1.7 | 13-20 ms | 23 ms |
| 45,5 | 662 | 1.1 | 2 | 0 | 1.7 | 13-20 ms | 28 ms |
| 60,8 with 1% loss | 637 | 2.2 | 4 | 0 | 2.7 | 10-23 ms | 27 ms |
| 75,8 | 669 | 3.0 | 4 | 0 | 3.0 | 9-24 ms | 29 ms |

Three cautions:
- **Rates are very sensitive near a frame boundary.** In older runs, 25,5 gave 0 and 35,5 gave 659, so a 1 ms bias matters there.
- **Rates level off above 33 ms** because of the bot's input rate. Above that, depth and snap size are the useful numbers.
- **These runs share one PC.** A second machine would give two real clocks.

**Still has to be measured on your setup:** real button-to-light on your PC, and the bot-independent PC vs PC numbers with person-like inputs.

### What players say

Regional pings are about 15-30 ms, out of state 50-70 ms, and players tolerate up to 60-90 ms if it's stable. Spikes bother them most. On the Wii specifically, the November 2 r/SSBM thread on the delay-based build is blunt: the top comment objects to random unranked opponents being matched against it. Fizzi said delay-based play "somewhat nullifies the benefit of the other client having rollback logic", and also that he has always wanted Slippi on Wii. That's the bar for acceptance, and it makes the PC player's numbers as important as yours.

## 3. What's actually causing the stutter and lag

| # | Cause | Evidence | Fix size |
|---|---|---|---|
| 1 | **Slippi's frame-skip rule fires by mistake.** When a frame has to wait for a free buffer, the next frame's drawing is skipped. On PC, a patch bypasses this; on a Wii that patch hangs the console, so we removed it and the rule stayed live. Reading the controller early makes fast frames finish early and wait, so the rule fires again and again. | 24 of your 29 repeats; the same pattern on GAME! (25 extra); explains why reading up to 12 ms early gave hundreds of repeats | Small, unproven |
| 2 | **Rollback frames are slow at depth 2 and up.** A save costs 2.1-2.5 ms per predicted frame, a load 3.6-4.3 ms. That's worst-case 15 ms at 1 predicted frame, 19 at 2 and 23 at 3. | Most of what remains at 100 ms | Medium |
| 3 | **Interrupts stay off for up to 7-8 ms** during a rollback, so the screen refresh can be handled late. If it's late enough, a frame could show a refresh late with no record of it. | Slippi's code plus our logs; effect on screen unknown | Medium |
| 4 | **The game waits for the ARM with interrupts off**, about 0.8-0.9 ms every frame, plus input waits of up to 5 ms inside the frame. | Performance logs | Medium |
| 5 | **Visible corrections.** Two of your five marks had no dropped frames at all, just rollbacks. | Your 58 s and 85 s marks | Compare with PC first |

**Input lag is worse than I told you, but better than the old build.** The controller is sampled about 8 ms before the screen refresh, whatever time the game reads it. My code that moved this sampling is undone by the game on every frame. So:

| Build | Button to light, mid-screen on a zero-lag display (estimated) |
|---|---|
| Old rollback with the third buffer | About 83 ms |
| Current rollback, and delay at 2 frames | About 67 ms |
| Slippi PC at 144 Hz, for comparison (published) | About 57 ms |
| With fresh controller sampling and a read at the refresh | About 59 ms |

The last row looks like the cheapest remaining win: fresher controller sampling could save about 7 ms on any build, including delay. It still needs a real test.

**How far the Wii can go, by ping (cost model, not yet tested):**

| Ping | Frames still predicted | Today | With planned work |
|---|---|---|---|
| Up to 66 ms | 0 | Clean | Clean, with less lag |
| 70 ms | 0-1 | Fits, once cause 1 is fixed | About 0 repeats |
| 100 ms | 1-2 | Heavy moments miss | About 0, with one save per rollback |
| 130 ms | 2-3 | Fails in bursts | About 0, with further savings |
| 160 ms | 3 | No | At the sustained limit |
| 190-220 ms | 4-5 | No | Needs the Wii to raise its own delay |

## 4. Testing: what we can't see today

**Most important findings:**
- **"Shown" doesn't mean on screen.** The recorder sees what the game handed to the video hardware, not what the TV received.
- **The crouching bot is probably a test tool bug.** In 6 of 9 hand-tested sessions, the bot held one input for the whole first game. So some first-game data is suspect.
- **False passes:** a desync can exit as a pass when replays aren't mirrored; "0 stalls" prints in builds that don't measure stalls; a cut-off replay still counts as matching; freezes after a game ends count as "in play".
- **Results from 2 games prove little.** "0 repeats" in 2 games only rules out more than 78% of games having one.
- **Tests are slow and flaky.** A run takes about 4.7 minutes. 137 of 467 runs (29%) produced no game.

### What a player notices, and how we'd measure it

| Player notices | Side | How | Exists today? |
|---|---|---|---|
| Real repeats, jumps, tearing | Both | Video capture, reading the in-game timer's digits, which change every frame | No |
| Button to light | Both | Controller rig plus light sensor, same rig on Wii and PC | No |
| Fresh controller readings, lost quick presses | Both | Same rig, recording real controller polls and random-phase presses | No |
| Frame timing | PC | Dolphin's own log (working now), plus PresentMon | Partly |
| Late screen updates | Wii | Log the exact line when the video registers are written | Partly |
| Snap size, wrong-action flashes | Both | From replay data | Prototype |
| Rollback rate, depth, bursts | Both | Replays | Yes, without bursts |
| Freezes, speed changes, warnings | PC | Dolphin log | Mostly; report miscounts |
| Input arrival, reply time, each side's ping | Network | Relay data | Data exists, not reported |
| Clicks, doubled or lost sound effects | Both | Audio capture and detector | Stand-in only |
| Game start wait, countdown, GAME!, pause | Both | Capture and logs | Hand-corrected |
| Match found to play, stage loads, results to next game | Both | Logs and capture | Partly |
| Menus and character screen | Both | Capture with real inputs | No |
| Stadium changes, every stage and character | Both | Coverage matrix weighted to real play | Skewed |
| Boot success: cold, warm, after exit | Wii | Supervisor statistics | Anecdotal |
| Desync, disconnect, crash rates | Both | Pass/fail gate | Partly |
| Long sessions, memory, drift, heat | Both | Overnight runs on the player's configuration | Partly |
| Wii as host, port swaps | Both | Run both ways | Never run |
| Burst loss, spikes, uneven routes | Network | Better relay | Random loss only |
| Real internet | Both | Relay on a small rented server, plus real matches | Manual |
| The exact player build | Wii | Release build with rig | Never |
| Other setups: second Wii, USB adapters, replays on, rumble, 480i vs 480p | Both | Occasional matrix | No |

### What to build, in order

| # | Build | Why | Size | Needs |
|---|---|---|---|---|
| 1 | Fix the bot bug; flag runs with abnormal input rates | First games are suspect | Small | - |
| 2 | Fix the false passes and mislabels | A pass must mean a pass | Small | - |
| 3 | Metrics report per game, also run over the 467 archived runs | New truth with no new runs | Small | - |
| 4 | Keep PC frame timing logs; PC baseline library | First hard PC cadence numbers | Small | - |
| 5 | Phone clip and digit decoder | First on-screen truth | Small | 1 min of your time; 240 fps phone |
| 6 | Person-like inputs: real replays and Phillip as opponent | Real rollback load | Medium | - |
| 7 | Test supervisor: no rebuild or reboot between runs, one run at a time, hard deadlines | About twice the games per hour, fewer dead runs | Medium | - |
| 8 | Capture stick pipeline, checked against the recorder | Stutter is priority one | Medium | About $20 |
| 9 | Controller rig on Wii and PC | The only true answer on lag; tests the real controller path | Medium-large | About $20 |
| 10 | Relay: true timing, spikes, burst loss | The relay is itself a measuring tool | Medium | - |
| 11 | Second-PC baseline and ping sweep report | A credible parity number | Medium | A second PC |
| 12 | Replay-exact test: a replay's inputs must reproduce that replay | Whole-system check on real game states | Medium | - |
| 13 | Audio detector; real-internet tests | Remaining gaps | Medium | About $5 a month |

### How much testing proves a claim

- **Zero-event claims:** 30 clean minutes per group for "at most 1 per 10 minutes", and 3 hours for "per hour".
- **PC parity:** run the same inputs across about 6 delays, 10 games each per side, then report how many ms the Wii's curve is shifted from PC's. That's about 120 games and should resolve about 0.6 ms (estimate). Single-point comparisons at 30,5 would need 230-300 games per side for a 10% difference.
- **Every run gets one of three results:** invalid, fail or pass. Too little data never passes.
- **Every instrument has to go red** on an injected fault, or we don't trust it.
- **Fix what confounds results:** stage, matchup, input load, PC load, run order, host and port.

## 5. Code review

**Would I keep it all as is?** No. **Would I start over?** Also no: the hardest-won parts are right and a rewrite would relearn them. **Are fundamental changes needed?** Yes, three:

1. **One owner for frame timing.** Today, timing is spread across about 700 lines of reactive code: the early read, frame hold, tracker and input wait.
2. **The game never waits for the ARM during a frame.** Shared memory replaces that round trip, and the kernel's main loop can stop running at high priority 2,000 times a second.
3. **Rollback work can't break the display deadline.** Today every frame's deadline is set around the slowest possible work.

| Part | Verdict | Why |
|---|---|---|
| Savestate core: write tracking, version store, memory copies | Keep and clean | Correct on hardware, with hard-won cache fixes |
| Network library, input buffers, wire protocol, matchmaking, file handling | Keep | Match Slippi for the current version, and are tested |
| Replay ring, MEM2 answer path, early input send, recorder, test relay | Keep and extend | Right direction |
| Frame timing: early read, hold, tracker, input wait | **Rewrite** | Reactive, and built partly on a misdiagnosis |
| Per-frame kernel path | **Rewrite** | Root of the wait and of reply-time problems |
| Time sync: four controllers | **Rewrite as one** | They interfere; one wrong limit explains the deeper PC rollbacks |
| `rollback.c` (2,570 lines) and `so_exi.c` (2,100 lines) | **Split** | Too much in one file; test code on hot paths |
| Network client and sockets | Refactor | Locks on the game's path; a full queue can silently lose an input |
| Kernel self-test, diagnostics, spare sender, async probe | **Delete from player builds** | Hundreds of lines of test code in a nearly full kernel |
| Alarm relinking, disc-load sync | Delete after one check | Logs suggest they never do anything |
| Hand-kept offsets across C, assembly and kernel | Replace with generated ones | Fragile |
| Build and tests | Extend | Timing logic has no off-console test; a small simulator reproduced the main stutter |

### Hazards found (most serious first)

| Hazard | What could happen | Confidence |
|---|---|---|
| Upstream Slippi has changed format (next version, unreleased): input packets go from 8 to 9 bytes; new secure matchmaking | **The Wii may stop pairing the day it ships** | Source read by two reviewers; effect untested |
| Lost capture when memory is short | Game silently goes wrong instead of ending | Code certain, rare |
| Full send queue drops an input | PC gets wrong inputs; silent desync after a long network stall | Code verified, rare |
| A controller read can nest inside another during the input wait | Desync or freeze, the same class as a bug fixed earlier | Path verified, unlikely |
| Teams and Party always write "not Teams" (also in released 1.3.0) | Mismatched matches, likely desync | Code verified, not run |
| Game-ending logic keyed to the text of a log line (`so_exi.c:1923`); network call without its lock | Rare bad ending | Code verified |
| A log flush while holding a lock on a first desync | Roughly a 300 ms freeze | Verified |

The reviews also confirmed what's right: games stay in sync, and the core rollback mechanism is the right one for this hardware.

## 6. The plan

Each phase ends with a measured result. A hand test from you only happens after the tools say a build passed.

| Phase | Work | Done when | Your time |
|---|---|---|---|
| **0. Agree and clean the tools** | Approve section 1. Build items 1-5. Run one bot test of the frame-skip fix, to confirm or kill the main theory. | Goal doc committed; false passes fixed; first scorecard; theory settled | Review; 1-minute phone clip |
| **1. Tools that can't lie** | Items 6-9. Baseline library. Scorecard for PC vs PC, current rollback and delay. | One overnight command gives a scorecard; every instrument goes red on an injected fault | Plug in hardware |
| **2. The picture** (biggest stutter) | Single timing module with an off-console test. Correct the frame-skip rule. Let refreshes through during rollback. Fresh controller sampling. Choose read timing from data. Close the nesting and lost-capture hazards. | Bands A and B: 0 repeats in 30 min per stage group on capture; lag 60 ms or less | **First hand test** |
| **3. The PC player and the network path** | Take the game's per-frame wait off the ARM. Lower the kernel's load. One time sync. Fix the known errors: the sync limit, redundant resends, ENet's packet throttling. Reduce the start wait. | Bands A and B: all PC targets met; reply time 3 ms or less | None |
| **4. Deep rollbacks** | One save per rollback; save during idle time; tighter memory for particles; hold back small corrections when needed; handle Band D. | Band C targets met, or revised with data | Short hand test |
| **5. Clean and document** | Finish splitting files; remove what's left; docs describe the design rather than its history; independent code review. | Fresh review finds no accretion; docs match code | None |
| **6. Release** | Support the next Slippi version; Teams and Party working or hidden; reliability numbers on the player build; real matches; private beta. | Every target met on the release build | Beta, real matches |

**Main risks:**
- **Slippi's next version.** Timing isn't in our control, which is why I'd talk to the Slippi team early.
- **The cost model could be off** by about one predicted frame, moving the band limits.
- **Corrections you can see** may turn out to matter more than repeats.
- **The Wii's network stack may have a floor** of about 2.5-3 ms reply time.
- **One unexplained black screen** remains.

## 7. Decisions for you

1. **"No worse for the PC player": same wire or same displayed ping?** The Wii's network stack seems to add about 1.5 ms each way that may not go lower. I propose allowing up to 2 ms one way, about the same as a slightly longer internet route, and documenting it.
2. **Do the goal numbers look right?** Especially 160 ms for Band C, and 60 ms button to light.
3. **Hardware:** are you OK with about $40 for a capture stick and controller rig? I also need to know: which display does your Wii use (CRT, or HDMI adapter to a monitor)? Does your phone record 240 fps? Do you have a second PC on your network?
4. **The public 1.3.0 build:** the community's complaint is about strangers being matched against delay-based play. Options: keep as is, or limit it to Direct until rollback ships. I'd also hide Teams and Party now. I'd limit it, because of how acceptance is going, but it's your call.
5. **Over about 190 ms ping:** may the Wii raise its own input delay automatically? I'd say yes: smooth with more delay beats stutter.
6. **Talking to the Slippi team:** Fizzi has said he's wanted this. Would you contact him about the next protocol version and what would make a Wii client acceptable? I'd do this early, and I can draft the message for your approval.

## 8. Things I told you that were wrong

- **"52-56 ms total lag at 2 frames."** That left out controller sampling age and screen scan-out. The estimate is about 67 ms, and it's still unmeasured.
- **"Rollback is 5.5 ms behind delay."** They're probably about equal, because the sampling doesn't move.
- **"Lag falls to 18.7 ms at low ping."** The read moves, but the controller sample doesn't, so that saves nothing real.
- **"Reading the controller more than about 5.5-7 ms early can't work."** I treated that as a limit, and wrote it into the code's comments. The frame-skip rule was behind it.
- **The guard fix "may be behind your stutter bursts."** The bug was real, but it was not the main cause.
- **"Shrinking the code didn't fix the black screens, so size wasn't the cause."** Six of the seven hangs had boot code about 4 KB past its limit, and the last 24 boots have been clean. One hang at a smaller size is still unexplained.
- **"0, 0 repeated frames."** That was from two games and from a recorder that can't see the screen.

## 9. Progress, 2026-11-06

Measured with person-like bot inputs against the scorecard (`tools/sweep/scorecard.py`), on a capture of the video output.

| Connection | Repeats a minute | Note |
|---|---|---|
| 15,3 | 0 (1.7 min, before the last fixes) | Short run |
| 30,5 | 0.3-0.9 (was about 14) | 20 of 20 games matched frame by frame |
| 45,5 | 3-20 | A 2-frame rollback costs about 18 ms; a refresh is 16.7 ms |
| 60,8,1 | about 200 (not rerun since the 8 ms read) | Phase 4 |

Done: the skip rule, video through loads and captures, a line-early nudge in place of whole-frame advances, fresh controller sampling, the read 5.5 to 8 ms early during deep rollbacks, frames held by when their poll came, no resend after a pad sent ahead, no input wait on a main-thread poll, ENet's throttle off (no measured effect).

Tried and dropped:
- **Shortening every line** to nudge: a capture card lost the picture.
- **Skipping unchanged pages:** half of captured pages are byte-identical, but a load must restore everything written since the last capture, so a load gains nothing.
- **Kernel thread priority and a second sender:** no change to reply time. `sendto` takes 1.7 ms in IOS, and IOS runs them one at a time.
- **A read 10 ms early:** a lost savestate, fatal.

Open:
- Reply time is about 5 ms against a 3 ms target, and may have a floor near 4 ms. Inputs reach the PC about 2 ms later than a PC's would, at the agreed limit.
- PC rollbacks a minute run about 30% over PC vs PC at 30,5, with the same depth. Not explained.
- The PC's start wait is about 28 frames against 0.
- About 8 odd-step pairs a minute on video follow nudges, with no repeat in the record. Probably the card's sampling; needs a look at a real display.
- Fresh sampling's lag saving is by calculation only.
- Whether to raise delay to 3 frames at about 90 ms ping and up: 0.6 repeats a minute against about 20 in one test.

## 10. Progress, 2026-11-07 and 08

Judged on the capture card (`build/oddmatch.py`: every off-rhythm step matched to the trace's events), 6 games a row unless noted. Commits on `rollback-stutter`, not pushed.

| Connection | Before (Nov 7 morning) | Now |
|---|---|---|
| 30,5 | ~15 odd steps a minute on video (12.4 min soak), 0.2 repeats/min | 0 repeats, 0 late retraces; ~1 nudge pair per 2 min, mostly in a game's first 10 s |
| 45,5 | 24-86 odd steps per 3.8 min, 3-20 repeats/min | 0.3-1.6 repeats/min, 2-4 nudge pairs per 6 games |
| 60,5 | | 32 repeats/min: depth 2-3 every frame, passes of 3-4 loops (19-25 ms) |

What did it, in order:
- **Input wait capped** (c2f7ff3) and **Melee's own frame-buffer race** in `HSD_VICopyXFBAsync` (bdc51f2): the late retraces and the 30 ms repeats.
- **Background saves** (161e626, 790e56f; on by ping from 80 ms): a capture's copies go on while the game runs, pages write-protected until copied.
- **Nudges proven to be the video's hitches** (26e5bb5): with the pad read moved instead and no frame ever ending early, 0 pairs in 6 games against 31-38. The time sync goes by when the pad is sent, not by the picture, so the Wii now stays level by reading the pad earlier, a line a frame, up to 8 ms; frames end early only when it is over 3 ms behind.
- **The start's time sync** (7ff1001): a whole frame waited out from 10-16 ms ahead left the Wii up to 6 ms behind, which then took frames ending early. Now only frames that leave it ahead.
- **Kernel loop** (350e8a5): only the engine-timed quick window, so IOS's network module gets more time (`sendto` 2.16 to 2.01 ms).
- **Staying level from the countdown** (eaad2a6): the pad read moves from frame 60, by the read alone until 120, while it is still 2 ms early and up to 6 ms of a start's offset fit.
- **A wider band** (ac9c0e8): start 2.5 ms behind, stop 1.5 (was 1 and 0.3). Dolphin's speed-up is proportional to how far behind it reads itself; with the Wii stopping nearly level, Dolphin sat inside its own noise, ran ~70 ppm fast on average and the Wii followed it all game. 12 games each, same seeds: 30 ms capture odd steps 10 to 1, 45 ms nudge pairs 2 to 0, the PC's lateness and rollbacks no worse.

Measured and dropped:
- Freely nudging in the countdown (frames 60-120): the pairs just move to right after GO.
- No extra read time in deep rollbacks (`rollback_poll_deep_us=0`): more repeats in heavy scenes (Samus and Marth on Fountain of Dreams). More (9.5 ms) at 60 ms: worse; 60 ms is limited by the average pass, not the deadline.
- Leaving the GPU's copy textures out of savestates: they are only 256x256 shadows. The pages written whole every frame are the HSD object pool (`GObj_RunProcs` marks every process each frame; animation updates): real state.
- The profiler's idle time in rollbacks is the game waiting for the retrace after the pass: nothing to win.
- Network thread below IOS's network module, and acks held to share our pad's datagram: no gain that survived a second seed.
- Waiting out frames when far ahead after an opponent's hitch (tested by freezing the PC's Dolphin 300 ms every 12 s, `build/pc_stall.py`): Dolphin catches up as well, the Wii ends behind, and it got worse (whole-frame advances, more frames ending early).

What the PC player gets: at the time sync's balance the Wii's inputs reach the PC late by about half of (the Wii's receive latency + its pad's wait to be sent + `sendto`), 1.1-2.8 ms (relay, varies run to run). Acks and pads collide every frame (the PC's pad lands at about our poll), each waiting ~0.8 ms for the other's `sendto` (1.45 ms for an ack, 1.9 ms for a pad: IOS spends ~5 us a byte). PC rollbacks 1390-1780 a minute at 30,5 against 1008 PC vs PC.

Open:
- **60 ms one way and up** needs far cheaper rollback loops, or the PC player's delay. Not solvable by timing.
- **The PC's start wait** (19-38 frames a game): the Wii's load reads ~4.7 MB in 64 KB pieces from the SD card (14 ms each plus ~8 ms between), ~0.9 s. The disc cache is only ~3.4 MB beside the 16 MB rollback scratch and empties when full, so every match rereads everything. A prefetch of the opponent's character files during stage select, with a ring cache, could roughly halve it.
- **The opponent stalling** (a PC hitch of 76-485 ms, ~1 test game in 30): the Wii waits at the 7-frame limit, then ~15 frames of slow-motion deep rollbacks.
- **Reply time** 4.6-5.3 ms (target 3): the floor looks like IOS.

## 11. Progress, 2026-11-08 (day)

Commits on `rollback-stutter`, not pushed.

**The PC's wait at each game's start** (8e035c2). The load's critical part, after both players pick, read ~1.1 s from the SD card: the opponent's character (its main file, costume, animations, effects), the stage, both characters' and the stage's sound banks, and a dozen files every match reads. This console's own character the game already loads on the CSS. Between games the rollback engine's 16 MB scratch is idle, so `so_prefetch.c` fills it in a low-priority thread and `ISO.c` serves the game's reads from it: the opponent's pick as soon as it arrives, the last game's characters, all legal stages and their banks, and this console's bank (from the game's own CSS load of its character). The kernel closes it when the engine asks to start a game, before answering, and opens it after the game is over and the engine has reported. File lists come from Melee's DOL (`tools/gen_prefetch_tables.py`). Melee rounds reads up to 32 bytes, so each copy runs 32 bytes past its file.

| 30 ms one way, PC halts before the Wii's first input | per game | mean |
|---|---|---|
| Before (seed 21) | 27, 16, 26, 24 | 23 |
| Prefetch (seed 21, 4 runs) | 11-24, 0, 5-8, 1-6 | 4-9 |
| Prefetch (seed 31, 12 games) | 23, 0, 6, 5, 7, 18, 15, 0, 2, 4, 5, 11 | 8 |

The first game after boot keeps ~20: its drive is idle during the load, so it is something else (not found). PC vs PC is 0.

**A Wii that loads first** used to wait in lockstep at frame 1 for the PC's first pad, starting a one-way latency behind; Dolphin only clears offsets over 10 ms, and the pad read's lead ran out, so it sat ~3.6 ms behind all game, with late retraces. The frozen frames' pads are known (Slippi clears both players' below frame 78), so it now runs on them as a PC does and the time sync levels it.

**Games ending by stocks** (4deb4b9). At the game's report the kernel reset its numbers, which turned rollback off for the ~100 frames GAME! still runs. In lockstep the Wii's pad frames skipped 1-6 (Dolphin: "packet too small to read pad buffer", then it stops reading ours) and the PC halted at its rollback limit for up to 1.6 s after most such games, back to at least last night's runs. Now the game goes on rolling back until the engine has left it. 6 games of 90 s, all ending by stocks: PC halts in play 68-95 to 0. Real games mostly end by stocks, so the PC player saw this nearly every game.

Validation, 30 ms one way, seed 31, 12 games with the capture card: 0 odd steps, 0 repeats, 0 late retraces, 12 of 12 matching; the Wii's inputs reach the PC 2.0-2.4 ms late (as before).
45 ms one way, seed 21, 12 games: 1.4 repeats a minute, all in rollback overruns (earlier runs 0.8-1.2), 3.5 late retraces a minute (earlier 4.6-10.9), 12 of 12 matching, PC halts in play 0, start halts 6 a game.

**Rollback cost, measured** (a benchmark at the game's first frame, every page still equal to its copy), per 4 KB page:

| Step | us |
|---|---|
| MEM1 to the locked cache (DMA) | 2.8 |
| Locked cache to MEM2 | 2.6 |
| MEM2 to the locked cache | 8.8 |
| Locked cache to MEM1 | 2.7 |
| dcbst / dcbi of its 128 lines | 1.1 / 0.7 |

A load is 11.5-13 us a page (the MEM2 read), a capture ~5.4 (in background-save mode nearly every page is "hot" and copied at once). A 3-loop pass at 45 ms spends ~8 ms of ~17 copying ~265 pages a frame; the rest is Melee's own animation code (RB_TRACE's profiler, mapped with melee-3ds's symbols: HSD FObj/AObj/TObj/MObj/JObj/PObj/RObj interpreters, sin/cos), which drives hitboxes and platforms and cannot be skipped. Dropped: the DMA bringing pages in while the CPU puts each out (17.8 us a page: the castouts share the bus with the DMA). Left: the 64 KB page table read from MEM2 at each scan (~0.14 ms, 4 a pass) could live in MEM1 if 64 KB there were free.

**PC parity, tried and dropped (same day):** pad packets cut to the newest 4 pads, with every unacked pad again once one looked lost (older than a round trip and a frame). 91 bytes against 113, but `sendto` 2025 against 2027 us, the Wii's inputs at the PC 2.4 against 2.2 ms late, PC rollbacks the same: IOS's send costs a datagram, not a byte. (The earlier "~5 us a byte" was a misreading.) The Wii's lateness at the PC stays ~2-3 ms.

**Late retraces at VI line ~136.** At 45 ms most "late retraces" in the record sit at lines 128-143, the pad read's line. Yet they come 16.64 ms after the last retrace, and the capture card shows few of them (3 of 27 at 45 ms; 0 in a 30 ms run of 12 that had 2 games starting on the frozen pads). Probes showed no retrace waiting at a load, capture or held behind a pad read. Not explained; the capture card is the judge, not this count.

**The first game's wait, and most of the rest: the read pace** (evening). Slippi Nintendont paces every read, even one served from memory (the prefetch, the DI cache), like a drive; online it was 10 ms a seek and 15 MB/s. The kernel now counts the paced commands (`game drive: ... paced N commands (S seeks), KB, ms`): ~35 seeks and 12-15 MB between games, ~1.4 s at the old pace. Now 2 ms a seek and 60 MB/s (`ReadSpeed_Quicker`; tests: `test_read_pace`, `test_read_seek_ms`). Unpaced reads crashed ~1 stage load in 27 (8dd58eb: a return through a main-stack frame full of frame buffer data, in `gm_Scene_Vs_OnEnter`'s load; cause not understood), so this is paced still, and judged on loads: 108 games at 2 ms (seeds 21, 31, 41, 51, 61), no crash, no hang, every checksum matching.

| 30 ms one way, PC halts before the Wii's first input | total | per game |
|---|---|---|
| 10 ms seek, 15 MB/s (seed 21, 12 games) | 85 | 7.1 (first game 21) |
| 10 ms, 60 MB/s | 50 | 4.2 (18) |
| 2 ms, 60 MB/s | 5 | 0.4 (0) |
| 2 ms, 60 MB/s, seeds 31/41/51/61 (24 games each) | 23/18/6/24 | 0.2-1.0 (0-11) |

The Wii now often loads first; it runs the frozen frames on known pads and waits up to 13 frames before GO (Slippi's own early-side wait). PC rollbacks a minute 1442-1547 (were 1619-1729), the Wii's inputs 1.1-1.7 ms late at the PC (were 1.7-2.5). Capture card, seed 51, 24 games: 1 odd step (a line-136 late retrace in game 17), 1 repeat (a rollback). The record's line-136 late retraces rose (32-78 a minute, in the games the Wii starts ahead), but not on screen: same seed, capture card, 24 games each, old pace 2 off-rhythm steps in 15.3 min (one rollback repeat; late retraces 0.7/min, PC start halts 5.8 a game, PC rollbacks 1642/min), new pace 1 in 15.5 min (the line-136 retrace; 32/min, 0.2, 1547). The rest of what the scorecard first counted (2 and 3) were the capture card's own slips: one picture's hold about 180 s in and then every 8001 pictures, in every capture, whatever is playing; decode_marker.py now lists them apart. Dolphin wrote 21 replays of 24 in one run (all 24 checksums matching): a harness issue.

**What's left on screen at 30 ms, and the pass cost's floor** (evening). The seed-51 run's one real step (game 18, Bowser against Pichu) was a rollback repeat after a PC hitch: Dolphin took 522 ms for frames 1650-1680, the Wii ended 22 ms (1.4 frames) ahead, and for the ~5 s Dolphin took to catch up at +0.46% every pass ran 3 loops at 15-17 ms; two hit 19.7 ms. A PC Slippi opponent would skip a frame there (over 10 ms ahead, every 30 frames); for the Wii that is a certain repeat against a possible one, so it stays as it is.

Tried for a cheaper pass, measured on the Wii and dropped:
- Copies of the newest captures in MEM1 (a load from MEM1 would be ~5.5 us a page, not 11.5-13): Melee's OS heaps leave ~340 KB and ~336 KB free in a match (heaps of 56 KB, 512 KB, 10 KB, 5.9 MB), far short of the 2+ MB the newest captures need.
- The page table in MEM1: a TLB miss costs ~30 ns with the table in the cache, ~100 ns from MEM2 (one word of each of 5632 pages: 4.0 ticks a page through the BATs, 5.8 through the table cached, 9.9 uncached). A MEM1 table would win perhaps 0.1-0.3 ms a pass, from a heap the game may need.

**Late evening, 2026-11-08.** 30 ms one way, seed 71, 24 games with the capture card: 0 repeats, 0 off-rhythm steps in 15.5 min (2 card slips), 24 of 24 matching. With seed 51 that is 1 step (a PC hitch) in 31 min.

45 ms: the PC player rolls back deeper than against a PC (1.50-1.55 frames a rollback against 1.15; snaps over 5 units 1.8-2.1 a minute against 0.2). Depth follows the Wii's lateness steeply (2.9 ms late: 76% of the PC's rollbacks 2 deep; 0.8-1.4 ms: ~47%; PC vs PC 15%). The send timing's own jitter is small (frame to frame sd 1.2-1.8 ms against the PC's 0.9). By Dolphin's formula (the remote's send time = arrival - ping/2, ping from our acks), the balance leaves the Wii's pads half the ack's delay behind. Acks wait ~0.8 ms for the sender, but behind a pad already in IOS's sendto: sending acks ahead of queued pads moved 11-18 a game and nothing else (lateness 1.4/1.2 ms against 1.0/1.4 FIFO, 2 seeds x 12 games). Dropped; the floor is IOS's ~1.5-2 ms a datagram.

Three sweeps sat on the CSS for good after a game (the Wii running). The test bot's fault: it locked in by the door's icon, which a hovered character shows too, so after an A that missed it pressed START forever. It now also needs the picked flag. The kernel's test diag logs the menus' state every 10 s.

Seed 81, 30 ms, 21 games before that stall: the 23-repeat burst was a PC hitch (Dolphin 866 ms for 30 frames; the Wii held at Slippi's 7-frame limit, as a PC opponent would). Besides it, 2 repeats, both in the first 2 s of play while the pad read moves (frames 85-125), one after retraces handled at VI line 232.

**Night, 2026-11-08: beta hardening.**
- A hand test froze as Direct opened, every boot of a build without the bot: the background saves' state lives in the MEM2 scratch, which outlives reboots and is never cleared, and the engine fed it from the online CSS on (8bdfbeb). An audit found the engine also used the scratch when the kernel had given it to the disc cache or a big memory card (5906123): the engine now asks the kernel once a boot (0xD9 state 3) and touches nothing until yes; the kernel clears the engine's words at boot. Test knobs: `test_scratch_junk`, `test_no_scratch`. Soak with both, delay buttons on: 30 ms 24/24, 45 ms 18/18 match, no PC halts in play.
- Beta session through real matchmaking (LAN, the user's own PC, 4.6 ms): game 1 on Pokemon Stadium had 20 checksum mismatches from frame 4763 while a fighter flew off the left edge, ending as they grabbed the ledge (positions snapped); no rollbacks ran in that game. Slippi's ASM only ends a game when the position sum differs by more than 1, and the game went on, so this was within Slippi's tolerance. Its 321 ms freeze was our own report inside the poll (9932427). Idle Stadium games through 22 transformation loads match frame by frame, so the stage's geometry agrees; open: compare the Wii's own replay of that game with the PC's.
- Bot games end by stocks at ~50 s, before Stadium's first transformation (~62 s): the sweeps never covered one until `--idle-rounds` with `--timer-s`.
- Outside review (a second model, read-only): the PC's lateness is set by the ack's turnaround T (lateness = T/2 - 0.25 ms at Dolphin's balance), so Wii-side timing cannot beat it; only a faster ack can. Reply time is 3.7-6 ms median, sendto ~1.5 of it: the rest is the receive ring, the network thread's wake-up and ENet. Hence the fast ack (the receive thread acks a pad packet as it arrives, the same ENet datagram, before the send queue; config `fast_ack`).

- A crash in hsdAllocMemPiece (a free list holding 0x7C0802A6, `mflr r0`: another page's code) about a second into games at 45 ms, 2 of ~6 no-mirror games: the background saves (on from 80 ms ping) refilled their four locked-cache buffers with up to 10 DMA operations queued, against the 4-operation rule every other copy keeps (3f63301). The verify build cannot show it any more: retraces let through a capture or load (video_through) change bytes it checks, so both the old and the fixed build stopped on a false "rb VERIFY" within minutes.
- The pad is read ~8 ms before the retrace by each game's end, at 30 ms as at 45 (`earl` 7.4-8.0 ms, the base 5.5): sync_lead never comes back down. 2.5 ms of the Wii player's input lag, to stay level with the PC.

- Fast ack measured (8 games each, no mirror): 45,5 lateness 2.1 -> 1.5 ms, frames re-run 1.59 -> 1.52, reply 4.9 -> 4.4 ms; 30,5 1.8 -> 1.6 ms, PC rollbacks 1592 -> 1522 a minute. The rest of the reply is IOS's. A review found it could ack a datagram the ring then dropped (the sender never resends acked frames: a game stuck for good) and race the network thread's teardown; fixed in 473510b (ack only what is queued and taken, at most 8 frames past the stored head, under the group lock).
- Capping the early read at 6.5 ms (`rollback_sync_most_us`): at 30 ms the read came 1.35 ms later (the Wii player's lag down by that) but the PC saw the Wii's pads 2.0 -> 2.9 ms late and rolled back 1615 -> 1789 a minute; at 45 ms no change (the deep-rollback lead holds it at 8 ms). Default kept.

- **The Stadium crash, found (e255733).** Fighting bots on Pokemon Stadium with rollbacks running (`--plain-bot --lras-s 150`; bots that KO end games before the first transformation, idle ones never roll back) panicked in lbarchive.c:73 at a transformation: the pad's renewal, which rollback moves to a line interrupt mid-frame, sends 0xB0 between the main thread's 0xD2 write and its read, and each write cleared the answer waiting, so the game parsed a stale buffer. The kernel now sets a main-thread answer aside across 0xB0/0xE0 and gives it back to a read of its length; small answers (the ping display's 8 bytes, 1-byte ones) were being lost the same way. 12/12 Stadium games then matched frame by frame.

- Capture card, all fixes in (e255733, trace build, no mirror, 12 games each): 30 ms 1 off-rhythm step in 7.8 min; 45 ms 12 in 7.7 min (1.6 a minute, the top of the earlier 0.1-1.6; the same games with the old 10-deep queue: also 12, so the safe queue costs nothing on screen). The record's ~46 "repeats" a game are the paused frame after the bot's L+R+A+Start quit, not play.
- Final soaks, player configuration (no mirror, delay buttons, junk in the scratch at boot): 45 ms 48/48, 30 ms 48/48, 60,8,1 12/12; Stadium with fighting bots 30 ms 12/12 and 45 ms 12/12 matching frame by frame. No crash, no PC halt in play.

## Sources and evidence

**Slippi source:** Ishiiruka `slippi` branch at 7333463 (`SlippiNetplay.cpp`, `EXI_DeviceSlippi.cpp`, `SlippiSavestate.cpp`), latest release v3.6.4, `slippi-ssbm-asm`.

**Published:**
- [Notebookcheck on Dolphin's input lag tests](https://www.notebookcheck.net/Dolphin-input-lag-update-makes-emulator-faster-than-Nintendo-GameCube.1194079.0.html) (console 62 ms)
- [Kadano's oscilloscope data](https://kadano.net/SSBM/inputlag/)
- [Slippi Launcher FAQ](https://github.com/project-slippi/slippi-launcher/blob/main/FAQ.md)
- [r/SSBM on the Wii build](https://www.reddit.com/r/SSBM/comments/1xlmsyn/slippi_unranked_and_direct_codes_working_on_wii/)
- [r/SSBM on ping tolerance](https://www.reddit.com/r/SSBM/comments/1tux5co/)
- [r/SSBM lag reading at 300 Hz](https://www.reddit.com/r/SSBM/comments/1sg0lp9/)

**PC figures from community sources are secondary:** smashboards.com and dolphin-emu.org blocked direct reading.

**Local runs:**
- PC vs PC baseline: `20261106-112938` to `-114401`
- Your game: `20261106-104022`
- Wii at 30 ms and 70 ms ping: `20261105-172504` and `20261106-105057`
- Wii at 100 ms ping: `20261105-173703`

**Code checked directly:**
- The frame-skip rule: `slippi-ssbm-asm/Binary/LagReduction/PD+VB.bin`, disassembled at `0x80376200` and `0x801A5018` with the game's own code from the disc
- Sampling reset: Melee's `vi.c` and `pad.c`
- Time sync limit: `so_exi.c`, `fine_sync`

### Hand test, 2026-11-06 (30,5)

- Input delay felt very low; no picture blips; a couple of small stutters; PC still feels smoother.
- The 10 s GAME screen is RB_TRACE's dump (`tr_report`), test builds only: about 2 s without it.
- Capture: 42 odd-step pairs in 3.6 min against 2 repeats in the record: 35 after a nudge stops, 5 at a retrace handled at line 23-31 (not in the record), 2 in the record. The 4 s the scorecard skipped hid a repeat at GO in every game: it now counts from frame 84.
- Tried: a steady nudge, a line every few frames instead of bursts. The card read 550 odd steps in 3.7 min (about 85 with bursts): set aside. It says nothing yet about a TV.
- PC runs about 0.03% fast against a Wii (0.005% against a PC), and its offset reads wider. Probably the Wii's slow reply inflating the half ping in its offset (not tested). That's about half the nudging the Wii needs.
- Open: whether nudges show on a real TV. A capture card can't say; a 240 fps clip of the marker can.

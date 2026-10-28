# M3 spec: Slippi's online EXI device, for the Nintendont kernel

What the Nintendont ARM kernel must do to stand in for Slippi Dolphin's EXI device when Melee runs Slippi's online codeset ("Slippi Online", `netplay.json`). It describes behaviour and byte layouts only; it contains no implementation.

Written 2026-10-26 from local, read-only sources. Every layout cites `file:line` so it can be checked.

## 0. Sources, versions and path shorthands

| Shorthand | Path | Revision |
|---|---|---|
| `D/` | `Source/Core/Core/` of [project-slippi/dolphin](https://github.com/project-slippi/dolphin) (mainline Slippi Dolphin) | git `5a1d426` "chore: update asm codeset", 2026-10-23 (shallow, 1 commit) |
| `R/` | [project-slippi/slippi-rust-extensions](https://github.com/project-slippi/slippi-rust-extensions) | as of 2026-10 |
| `I/` | `Source/Core/Core/` of [project-slippi/Ishiiruka](https://github.com/project-slippi/Ishiiruka) (Slippi/ and NetPlayProto.h only) | v3.6.4 |
| `A/` | [project-slippi/slippi-ssbm-asm](https://github.com/project-slippi/slippi-ssbm-asm) | `fcf47f1` (2026-05-01), `git describe` = `app-v1.1.6-1244-gfcf47f1` |
| `N/` | `Nintendont/kernel/` in this repository | as it was in 2026-10 |


Everything is big-endian unless stated otherwise. "Payload" means the bytes after the command byte.

### 0.1 Which codeset matches this Dolphin, and is there a version handshake?

- Dolphin bundles the generated codeset as `Data/Sys/GameSettings/Netplay/GALE01r2.ini` (sparse checkout; read it with `git show HEAD:...`).
- It differs from `A/Output/Netplay/GALE01r2.ini` (built from `fcf47f1`) in 74 diff lines, all recording/UI:
  - replay version `lis r3,0x0313; ori r3,r3,0x0100` (3.19.1) locally vs `...0x0000` (3.19.0) in Dolphin;
  - `SendFountainInfo`, `InitInGame`, `ControlAllPanels`, `InitializeInGameDelay`.
- Every EXI size immediate is identical in both, which pins the layouts in this spec: `li r4,26` (TXB), `li r4,297` / `li r3,297` (RXB), `li r4,962` (MSRB), `li r4,369` / `li r3,369` (RGB), `li r4,33` (SSRB) and `li r4,42` (OSB).
- **So `A/` at `fcf47f1` is the right ASM for this Dolphin.** The Dolphin bundle is slightly older, around `a14195e` "build codes" (2026-04-29).
- **There is no version handshake over EXI.** No command reports Dolphin's version to the ASM, and the ASM never checks one. The only version-related output is `0xB9`'s `app_state = 2` ("update required"), which Dolphin sets when the `latestVersion` in `user.json` is newer than its own semver (`D/HW/EXI/EXI_DeviceSlippi.cpp:3002-3010`).
- The codeset and device are matched only because Dolphin ships them together. The kernel therefore targets the layouts below and should return `app_state = 1` (section 3, `0xB9`).
- The netplay version the matchmaking server sees (`app_version`, 3.6.4 in `so_config`) is separate. It never crosses EXI.

## 1. The transfer model

### 1.1 PPC side: one helper does every transfer

`FN_EXITransferBuffer` at 0x800055f0 (`A/Common/EXITransferBuffer/EXITransferBuffer.asm:1-116`) takes `r3` = buffer, `r4` = length and `r5` = mode (0 = read, 1 = write; `A/Common/Common.s:520-521`).

1. `OSDisableInterrupts` (:36). The whole exchange runs with interrupts off.
2. Write only: `dcbf` every 32-byte block while the offset is below the length, then `sync`/`isync` (:45-52).
3. `EXIAttach(STG_EXIIndex)`, `EXILock`, `EXISelect(chan, dev 0, freq 5)` (:58-68).
4. `EXIDma(chan, buf, len, mode, NULL)`, then `EXISync` (:72-80).
5. `EXIDeselect`, `EXIUnlock`, `EXIDetach` (:84-93).
6. Read only: `dcbi` the same range (:100-107).
7. Restore interrupts.

The online codeset is built with `-defsym STG_EXIIndex=1`, so it uses **slot B, channel 1, device 0** (`A/build-netplay.bat`). The console builds use 0 or 1 (`A/build-console-core.bat`).

**A query is a write followed at once by a read, inside one interrupts-off window.** The device must have the answer ready when the write is acknowledged, or at the latest when the read arrives. Commands without an answer are a single write.

Every online command is issued as **its own DMA write, one command per write**. The recording messages (`0x35`/`0x36`, frame data) can pack several commands into one write.

### 1.2 Dolphin side: `DMAWrite` / `DMARead` and `m_read_queue`

`CEXISlippi::DMAWrite(addr, size)` (`D/HW/EXI/EXI_DeviceSlippi.cpp:3355-3581`):

- If byte 0 is `0x35` (`CMD_RECEIVE_COMMANDS`), `configureCommands` reads `payload[0]` = length, then triplets `(cmd u8, size u16)` into `payload_sizes` (:244-253, :3375-3388). Parsing continues after it.
- `0x3E` (`CMD_MENU_FRAME`) is forwarded whole and returns (:3390-3395).
- Otherwise it loops `while (buf_loc < size)` (:3404-3580):
  - `byte = mem[buf_loc]`;
  - if `byte` is not in `payload_sizes`, it logs "Invalid command byte" and **returns, dropping the rest** (:3407-3413);
  - otherwise it dispatches through `switch(byte)` with `&mem[buf_loc+1]` as the payload, or `&mem[buf_loc]` for the `SlippiExiTypes` structs that include the command byte;
  - then `buf_loc += payload_sizes[byte] + 1`.
- Unknown-to-the-switch commands fall to `default:`, which writes them to the replay file, the spectator stream and the Rust reporter (:3569-3575).
- **Dolphin reads the payload straight from emulated RAM.** It never checks that `1 + payload` fits in the DMA length. One online command depends on that: `0xC2` writes length 1 but carries a payload byte (section 3).

`CEXISlippi::DMARead(addr, size)` (:3583-3602):

- If `m_read_queue` is empty, it logs "DMARead: Empty" and **writes nothing**: the PPC buffer keeps whatever it held.
- Otherwise `m_read_queue.resize(size, 0)`, then `size` bytes are copied to `addr`. The answer is zero-padded or truncated to the read length.
- **Reading does not consume the queue.** It stays until the next command that clears and rebuilds it. Every "prepare*" handler starts with `m_read_queue.clear()`; write-only handlers leave it alone.

### 1.3 The payload-size table, and the `0x35` mechanism

Dolphin's built-in sizes are in `payload_sizes` (`D/HW/EXI/EXI_DeviceSlippi.h:138-194`). The ASM's `0x35` message only registers recording commands (`A/Recording/SendGameInfo.asm:64-141`, `A/Recording/Recording.s:16-45`), so **every online command's size comes from Dolphin's fixed table**. The kernel should hard-code the table in section 2.

The `0x35` message is sent once per game start, packed with `0x36` game info: length `MESSAGE_DESCRIPTIONS_PAYLOAD_LENGTH + 1 + GAME_INFO_PAYLOAD_LENGTH + 1` (`A/Recording/SendGameInfo.asm:627`). Nintendont's existing replay path already handles that stream.

### 1.4 What this means for the kernel

1. **Dispatch on the first byte of each write.** Online commands never share a DMA with others.
2. **Read the payload from RAM using the table size, not the DMA length.** Invalidate at least `max(len, 1 + payload)` bytes, rounded up to 32, before reading. `0xC2` (length 1, payload 1) needs this, and so do `0xD1`/`0xD2`, whose length is `strlen + 2` while the table says 0x40.
3. **Prepare the answer at write time, as Dolphin does.** Keep one response buffer, the equivalent of `m_read_queue`. On a read:
   - copy `min(len, resp_len)` bytes and zero-fill the rest of `len`;
   - flush the ARM data cache over `[ptr, ptr + len)` before acknowledging (the PPC `dcbi`s it afterwards);
   - if no answer was ever prepared, write nothing (Dolphin behaviour), or zeros (safer);
   - do not consume the buffer: a repeated read returns the same bytes.
4. **Keep online traffic out of the replay ring.**
   - The Nintendont DMA handler sends every mode-1 DMA to `SlippiMemoryWrite` (`N/EXI.c:805-809` slot A, `:841-845` slot B).
   - Online commands (`0xB0`-`0xC4`, `0xD0`-`0xD8`, `0xE1`-`0xE5`) must not go there. The consumer parses that stream with the `0x35` sizes and would desync on an unknown byte.
   - Dolphin itself only forwards recording commands, plus `0xE5`; see section 3.
5. **Answer `0xB0` in well under a millisecond.**
   - It runs from the `HSD_PadRenewRawStatus` hook (0x80376a28, `A/Online/Core/TriggerSendInput.asm:2`) with interrupts off, every frame.
   - M1b measured 0.39 ms per frame for write plus read once the EXI interrupt is instant (`docs/research/FEASIBILITY.md:104`).
   - Never wait for the network inside the handler. Take the client lock briefly, read what has arrived, decide, and answer.
6. **Nintendont's DMA mailbox length is 16 bits.**
   - The patched `EXIDMA` stub builds `0x12000000 | chan<<20 | mode<<16 | len` with a plain `or` (`N/asm/EXIDMA.S:31-38`).
   - The kernel decodes `len = command & 0xFFFF` (`N/EXI.c:797`).
   - A transfer of 64 KiB or more corrupts the mode and channel fields and truncates the length. This only affects the big reads, `0xD2` and `0xD4` (section 6, surprise 1).
7. **Route the slot.**
   - The online code uses channel 1, device 0. `N/EXI.c` sends channel-1 DMAs to the Slippi path only when `!slippi_use_port_a` and `GCNCARD_ENABLE_SLOT_B` is built (`N/EXI.c:838-871`).
   - The online device must answer channel 1, device 0, both reads and writes.
   - `EXISelect` presence (`N/EXI.c:734-737`) only matters if something probes; `FN_EXITransferBuffer` ignores the return values.

## 2. Command table

Each row gives the write the ASM issues, how many bytes Dolphin consumes, and the read that follows.

| Cmd | Name | ASM write length | Dolphin payload size (`EXI_DeviceSlippi.h`) | Answer: PPC read length | Issued from |
|---|---|---|---|---|---|
| B0 | ONLINE_INPUTS | 26 (`TXB_SIZE`) | 25 (:154) | **297** (`RXB_SIZE`) | `A/Online/Core/TriggerSendInput.asm:176-247` |
| B1 | CAPTURE_SAVESTATE | 33 (`SSRB_SIZE`) | 32 (:155) | none | `A/Online/Static/SaveState.asm:57-66` |
| B2 | LOAD_SAVESTATE | 33 | 32 (:156) | none | `A/Online/Static/LoadState.asm:115-124` |
| B3 | GET_MATCH_STATE | 1 | 0 (:157) | **962** (`MSRB_SIZE`) | `A/Online/Static/LoadMatchState.asm:33-46` |
| B4 | FIND_OPPONENT | 20 (`FMTB_SIZE`) | 19 (:158) | none | `A/Online/Menus/CSS/HandleInputsOnCSS.asm:346-376` |
| B5 | SET_MATCH_SELECTIONS | 10 (`PSTB_SIZE`) | 9 (:159) | none | `HandleInputsOnCSS.asm:401-481` |
| B6 | OPEN_LOGIN | 1 | 0 (:161) | none | `A/Online/Menus/TitleMenu/OnMenuPrep.asm:339,373-379` |
| B7 | LOGOUT | 1 | 0 (:162) | none | `OnMenuPrep.asm:818-824` |
| B8 | UPDATE | 1 | 0 (:163) | none | `OnMenuPrep.asm:357,373-379` |
| B9 | GET_ONLINE_STATUS | 1 | 0 (:164) | **42** (`OSB_SIZE`) | `A/Online/Static/UserDisplayFunctions.asm:414-427` |
| BA | CLEANUP_CONNECTION | 1 | 0 (:165) | none | `HandleInputsOnCSS.asm:539-560`; `A/Online/Slippi Online Scene/main.asm:712-730`; `A/Online/Menus/TitleMenu/OnMenuLoad.asm:82-89` |
| BB | SEND_CHAT_MESSAGE | 2 (`CMTB_SIZE`) | 2 (:160) | none | `slpCSS.dat` code (not in `A/`); layout `A/Online/Online.s:382-384` |
| BC | GET_NEW_SEED | 1 | 0 (:166) | **4** | `main.asm:840-860` |
| BD | REPORT_GAME | 369 (`RGB_SIZE`) | 368 (:167) | none | `A/Online/Core/InitOnlinePlay.asm:349-453` |
| BE | FETCH_CODE_SUGGESTION | 32 (`ACTXB_SIZE`) | 31 (:168) | **30** (`ACRXB_SIZE`) | `A/Online/Menus/CSS/TextEntryScreen/CheckTriggersAndZ.asm:129-163` |
| BF | OVERWRITE_SELECTIONS | 15 | 14 (:169-170) | none | `GameSetup.dat` (ranked; not in `A/`) |
| C0 | GP_COMPLETE_STEP | 6 | 5 (:171) | none | `GameSetup.dat` |
| C1 | GP_FETCH_STEP | 2 | 1 (:172) | **6** | `GameSetup.dat` |
| C2 | REPORT_SET_COMPLETE | **1** (2 bytes stored) | 1 (:173-174) | none | `main.asm:564-582` |
| C3 | GET_PLAYER_SETTINGS | 1 | 0 (:175) | **3264** | `.dat` code (not in `A/`) |
| C4 | REPORT_MATCH_STATUS_UPDATE | 2 | 1 (:176-177) | none | `InitOnlinePlay.asm:176-184` |
| D0 | LOG_MESSAGE | 128 | 0xFFFF ("by itself", :180) | none | `logf` macro, `A/Common/Common.s:238-277` |
| D1 | FILE_LENGTH | strlen+2 | 0x40 (:181) | **4** | `A/Online/Core/EXIFileLoad/GetFileSize.asm:25-44`, `TransferFile.asm:29-48` |
| D2 | FILE_LOAD | strlen+2 | 0x40 (:182) | **file length** | `TransferFile.asm:62-81` |
| D3 | GCT_LENGTH | 1 | 0 (:183) | **4** | `A/Bootloader/main.asm:42-53` |
| D4 | GCT_LOAD | 5 | 4 (:184) | **GCT length rounded up to 32** | `A/Bootloader/main.asm:78-90` |
| D5 | GET_DELAY | 1 | 0 (:185) | **2** | `A/Common/UseInGameDelay/InitializeInGameDelay.asm:105-118` |
| D6 | PLAY_MUSIC | 9 | 8 (:186) | none | `A/Online/Core/Music/StartSong.asm:53-66` |
| D7 | STOP_MUSIC | 1 | 0 (:187) | none | `Music/Stop.asm:28-35` |
| D8 | CHANGE_MUSIC_VOLUME | 2 | 1 (:188-189) | none | `Music/VolumeChange.asm:53-63` |
| E1 | PREMADE_TEXT_LENGTH | 3 | 2 (:190) | **4** | `A/Online/Static/LoadPremadeText.asm:38,66-81` |
| E2 | PREMADE_TEXT_LOAD | 3 | 2 (:191) | **E1 length** | `LoadPremadeText.asm:54,66-81` |
| E3 | GET_RANK | 1 (assumed) | 0 (:192) | **16** (Dolphin layout) | `.dat` code (not in `A/`) |
| E4 | FETCH_RANK | 1 (assumed) | 0 (:193) | none | `.dat` code |
| E5 | GET_RANK_VISIBILITY | ? | 0 (:194) | none prepared | no handler in Dolphin |

Command enums: `D/HW/EXI/EXI_DeviceSlippi.h:47-113`; ASM constants: `A/Common/Common.s:524-561`. Note that `A/Common/Common.s:542` names `0xBF` `CONST_SlippiCmdNameEntryAutoComplete`, but no ASM sends it; Dolphin's `0xBF` is `OVERWRITE_SELECTIONS`.

Some commands come from the `.dat` code of `slpCSS.dat`, `GameSetup.dat` and the other Slippi game files: `0xBB`, `0xBF`, `0xC0`, `0xC1`, `0xC3`, `0xE3` and `0xE4`. That source is not available locally. Their layouts here come from Dolphin's packed structs (`D/Slippi/SlippiExiTypes.h`, `#pragma pack(1)`), which define what Dolphin parses and returns.

## 3. Per-command specification

The device keeps per-session state. Dolphin's fields and what they become in the kernel:

| Dolphin | Meaning | Kernel |
|---|---|---|
| `local_selections` | `SlippiPlayerSelections` | `SoSelections` |
| `last_search` | `{mode, connect_code}` | own field |
| `recent_mm_result` | `{id, players, stages, items}` | `so_client_match()` |
| `forced_error` | string | own field |
| `allowed_stages`, `stage_pool` | stage lists | own fields |
| `overwrite_selections` | ranked overwrites | own field |
| `m_local_player_idx` = 0, `m_remote_player_idx` = 1 | ports for this lobby | own fields |
| static `online_match_block[0x138]` | **must persist across calls**: see `0xB3` | own buffer |
| `is_play_session_active` | Rust reporter session | not needed |
| skip/advance counters | time sync | own fields |
| `matchmaking` | `SlippiMatchmaking` | `so_mm` / `so_client` |
| `slippi_netplay` | `SlippiNetplayClient` | `so_np_*` |
| `user` | `user.json` | `so_client_user()` |

Fields are declared at `D/HW/EXI/EXI_DeviceSlippi.h:294-351`.

---

### 0xB0 ONLINE_INPUTS (essential)

**Request.** 26 bytes, `TXB` (`A/Online/Online.s:287-293`; filled at `TriggerSendInput.asm:176-217`).

| Offset | Size | Field |
|---|---|---|
| 0x00 | u8 | 0xB0 |
| 0x01 | s32 | `frame`: `ODB_FRAME`. **1 on the first in-game frame** (`InitOnlinePlay.asm:53-55`); +1 after every non-SKIP answer (`TriggerSendInput.asm:972-974`). |
| 0x05 | s32 | `finalized_frame`: `ODB_STABLE_FINALIZED_FRAME` (:185-186) |
| 0x09 | u32 | `finalized_frame_checksum`: the local checksum recorded for that frame (:190-207). If none is found, the byte is left over from the previous frame. |
| 0x0D | u8 | `delay`: `ODB_DELAY_FRAMES`, from MSRB delay clamped to 1..15 (`InitOnlinePlay.asm:224-239`) |
| 0x0E | 12 | local pad report (layout `A/Online/Online.s:96-112`): buttons, buttons, stick X/Y, C-stick X/Y, L/R analog, 2 unknown, status (+0x0A), pad |

The ASM zeroes all pads before `START_SYNC_FRAME - delay`, where `START_SYNC_FRAME = 78` (`Online.s:58-63`, `TriggerSendInput.asm:85-93`). It also clamps sticks within ±2 of rest to 0 (:100-130).

**Dolphin's algorithm** (`handleOnlineInputs`, `D/HW/EXI/EXI_DeviceSlippi.cpp:1180-1264`):

1. `m_read_queue.clear()`. Parse `frame`, `finalized_frame`, `checksum`, `delay` and `inputs = payload+13` (:1184-1188).
2. If `frame == 1` (:1190-1224):
   - reset the savestate pools, `stall_frame_counts`, perf debt, and the skip/advance state (`frames_to_skip`, `is_currently_skipping`, `frames_to_advance`, `is_currently_advancing`, `fall_behind_counter`, `fall_far_behind_counter`);
   - `local_selections.Reset()`;
   - `slippi_netplay->StartSlippiGame()` (`D/Slippi/SlippiNetplay.cpp:1212-1238`: clears the local pad queue, timings, acks, desync recovery, the game-prep queue and `match_info`, i.e. **the remote selections**).
3. If disconnected (no netplay object, or status not `CONNECTED`, :1171-1178), push `3` and return (:1226-1243). The read is then `03 00 00 ...` zero-filled to 297.
4. `DropOldRemoteInputs(finalized_frame)`: per remote, pop the oldest pads while `size > 1 && back.frame < finalized_frame` (:1246; `SlippiNetplay.cpp:1529-1538`).
5. `should_skip = shouldSkipOnlineFrame(frame, finalized_frame)` (:1248; rules below).
6. If skipping: `SendSlippiPad(nullptr)`, which resends un-acked pads only and **does not send this frame's input** (:1249-1253).
   Otherwise: `handlePoorMatchPerformance(frame)` (ranked only, :1266-1271), then `handleSendInputs` (:1598-1614):
   - on `frame == 1`, first send `delay` empty pads for frames `1..delay` (`SlippiPad(i)`: 12 zero bytes, checksum frame 0, checksum 0; `D/Slippi/SlippiPad.cpp:30-36`);
   - then send `SlippiPad(frame + delay, checksum_frame = finalized_frame, checksum, inputs)`.
   - Only the first 8 pad bytes travel (`SLIPPI_PAD_DATA_SIZE`, `SlippiPad.h:5-6`; `SlippiNetplay.cpp:1314`).
7. `prepareOpponentInputs(frame, should_skip)` builds the answer (:1634-1798).

**Return code** (`prepareOpponentInputs`, :1638-1657). The values match the ASM's `RESP_*` (`A/Online/Online.s:563-566`). The `FRAME_RESP_*` enum at `EXI_DeviceSlippi.h:115-121` (0..3) is legacy and **unused**; ignore it.

| Value | ASM name | When Dolphin returns it | ASM effect (`TriggerSendInput.asm:254-286`) |
|---|---|---|---|
| 1 | `RESP_NORMAL` | default | Use this frame's inputs and advance |
| 2 | `RESP_SKIP` | `should_skip` | Return to the parent's epilogue (0x80376cec): **no input this poll, frame not advanced, the same `frame` is sent next poll**. Exception: once `ODB_IS_GAME_OVER`, SKIP is treated as NORMAL (:268-274). |
| 3 | `RESP_DISCONNECTED` | status != `CONNECTED` (checked after skip) | Sets `ODB_IS_DISCONNECTED`, then continues as NORMAL. The engine loop shows "disconnected" and ends the game (`StartEngineLoop.asm` ~:290-313). |
| 4 | `RESP_ADVANCE` | `shouldAdvanceOnlineFrame(frame)` | NORMAL, plus `ODB_IS_FRAME_ADVANCE = 1`. `ForceInputRefetchOnAdvance.asm` (hook 0x80019614) calls `RenewInputs_Prefunction` again, so the engine runs an extra frame, and polls `0xB0` for `frame+1`, in the same VI. |

**Response.** 297 bytes, `RXB` (`A/Online/Online.s:295-302`; built at `EXI_DeviceSlippi.cpp:1657-1793`). `ROLLBACK_MAX_FRAMES = 7`, `SLIPPI_REMOTE_PLAYER_MAX = 3` (`D/Slippi/SlippiNetplay.h:34-40`), `SLIPPI_PAD_FULL_SIZE = 12`.

| Offset | Size | Field | Dolphin source |
|---|---|---|---|
| 0x000 | u8 | result (1/2/3/4) | :1657 |
| 0x001 | u8 | remote player count (`matchmaking->RemotePlayerCount()` = players − 1; 1 in 1v1) | :1659-1660; `SlippiMatchmaking.cpp:808-814` |
| 0x002 | 3 × {s32 frame, u32 checksum} | per remote `i`: `checksum_frame`, `checksum` from that player's **latest received pad packet** (`remote_checksums[i]`, `SlippiNetplay.cpp:364-368`). With an empty queue it is 0/0 (`SlippiNetplay.cpp:1481-1491`). Unused slots are 0/0. A disconnected remote (multi-player) gets the values of the connected remote with the highest `latest_frame`. | :1711-1736 |
| 0x01A | 3 × s32 | per remote: `min(latest_frame, frame)`. Unused slots: `frame`. Disconnected remotes: `latest_frame_from_opps` (also clamped). | :1743-1766 |
| 0x026 | s32 | minimum of the three values above | :1768-1769 |
| 0x02A | 3 × 7 × 12 | per remote `i` at `0x02A + i*0x54`: up to 7 pads, **newest first**, starting at the pad for frame `min(latest, frame)`, zero-padded to 84 bytes. Pad `k` is for frame `RXB_OPNT_FRAME_NUMS[i] − k`. Each remote pad is the 8 received bytes plus 4 zero bytes (status byte 0x0A = 0 = connected). | :1772-1787; `SlippiNetplay.cpp:1474-1527`, `SlippiPad.cpp:38-42` |
| 0x126 | 3 × u8 | `should_despawn[i]`: 1 when a remote is disconnected and `frame >= roundup30(lastInputFrame + 2*7 + 2)` | :1685-1709, :1789-1793 |
| 0x129 | | end (297) | |

How `GetSlippiRemotePad(i, 7)` (`SlippiNetplay.cpp:1474-1527`) fills `data`:

- It walks the remote queue **from oldest to newest**, prepending each pad, so `data` ends up newest first.
- It stops after 7 pads. `data` therefore holds the **7 oldest** queued pads, and `latest_frame` is the newest of those.
- The queue is trimmed to the finalized frame each call (step 4), so in steady state that is roughly `finalized .. finalized+6`.
- `offset = max(0, (latest − frame) * 12)` then skips the pads newer than `frame` (:1749-1750).

**How the ASM uses it** (`TriggerSendInput.asm:735-902`):

- For remote `c` (walking ports and skipping `ODB_LOCAL_PLAYER_INDEX`), `idx = RXB_OPNT_FRAME_NUMS[c] − frame`.
- If `idx >= 0` (only `idx == 0` is possible after Dolphin's clamp), it copies pad `idx` into that port. That is a real input.
- If `idx < 0`, it predicts:
  - from frame `START_SYNC_FRAME − delay` on, and while not game over, it records a prediction and sets `ODB_SAVESTATE_IS_PREDICTING` and `ODB_PLAYER_SAVESTATE_FRAME`;
  - this later causes `0xB1` captures (`StartEngineLoop.asm:635-655`) and possibly a rollback with `0xB2` (`TriggerSendInput.asm:573-602`, `StartEngineLoop.asm:318-393`);
  - it uses pad 0 as the stale input (:861-862).
- The checksums at 0x002 are compared with local ones for frames ≤ the stable finalized frame and above 84 (`StartEngineLoop.asm:490-611`):
  - the low 16 bits are the integer sum of fighter position floats; a difference greater than ±1 is a **hard desync**: error SFX, message, and `FN_END_GAME`;
  - a difference in the high 16 bits (XOR-folded action state and positions, `StartEngineLoop.asm:75-145`) is a **soft desync**: warning text only.
- `RXB_SMALLEST_LATEST_FRAME` caps the finalized frame (:664-668).

**Dolphin's skip rule** (`shouldSkipOnlineFrame`, `EXI_DeviceSlippi.cpp:1362-1474`):

1. If the connection status is `FAILED` or `DISCONNECTED`, return false (:1364-1373).
2. **Rollback limit.** For each remote (skip disconnected ones):
   - if `latest_remote_frame − finalized >= frame − finalized − 7`, i.e. `latest >= frame − 7`, it has enough input;
   - otherwise `stall_frame_counts[i]++` and skip. After `> 420` stalled frames (7 s) it calls `ForceDisconnectPlayer` (:1382-1421).
3. **Time-sync skip** (:1423-1460) only runs when `frame % 30 == 0` (`SLIPPI_ONLINE_LOCKSTEP_INTERVAL`, `SlippiNetplay.h:35-36`), `!is_currently_skipping` and `frame <= 120`:
   - `offset = CalcTimeOffsetUs()`;
   - if `offset > 10000` µs (t1; the t2 = 2×16683 + 10000 branch is unreachable because of `frame <= 120`):
     - `frames_to_skip = min(((offset − 10000) / 16683) + 1, 5)`;
     - `is_currently_skipping = true`.
4. If `frames_to_skip > 0`, decrement it and return true. Otherwise clear `is_currently_skipping` and return false (:1463-1473).

**Dolphin's advance rule** (`shouldAdvanceOnlineFrame`, :1476-1596). It is evaluated only when not skipping and connected.

1. If every remote is a bot (`opponentRunahead`, :1616-1632), return false.
2. On `frame % 30 == 0` (:1497-1579):
   - set the emulation speed: no change for offsets in (−250, 8000) µs; when behind, speed up by up to +1% (full at 3 frames, 3×16683 µs); when ahead, slow down by up to −0.5%;
   - count `fall_behind_counter` (offset < −10000) and `fall_far_behind_counter` (offset < −26683, t2 = 16683 + 10000), used for a warning OSD;
   - if `offset < −26683 && !is_currently_advancing`:
     - `frames_to_advance = min(((−offset − 10000) / 16683) + 1, frame > 120 ? 3 : 0)`;
     - `is_currently_advancing = true`.
3. If `frames_to_advance > 0`: return true only on frames where `frame % 5 == 0`, decrementing each time. Otherwise clear `is_currently_advancing` and return false (:1582-1595).

`CalcTimeOffsetUs` (`SlippiNetplay.cpp:1664-1733`):

- Per active remote, take a ring of up to 30 samples, sort it, drop the top and bottom third, and average.
- Return the **minimum** across remotes, or 0 with no samples.
- Each sample is taken when a pad packet arrives (`SlippiNetplay.cpp:295-318`):
  - `(recv_time − ping/2) − last_local_send_time + 16683 × (my_last_sent_frame − their_frame)`;
  - positive means we are ahead.
- The kernel already has this as `so_np_time_offset_us()` / `so_time_sync_*`.

#### What a lockstep (never-rollback) device must answer

The rule: **never return 1 or 4 unless every active remote's pad for `frame` has arrived.** That is the only way the ASM can predict, and prediction is what triggers `0xB1`/`0xB2`.

On each `0xB0` for `frame`:

1. If disconnected, answer 3 exactly as Dolphin does: `03` followed by zeros to 297.
   - The ASM may still predict once and send one `0xB1` before the game ends. That is harmless when `0xB1` is a no-op.
   - Alternative: fill `RXB_OPNT_FRAME_NUMS` with `frame` and pads with zeros. The ASM then never predicts; the inputs it uses are the same zero pads.
2. **The game-start reset runs once per game, not on every `frame == 1` poll.** Dolphin never skips frame 1 (`latest >= 1 − 7` always holds); a lockstep device usually does. Re-running `StartSlippiGame` on a repeated frame-1 poll would wipe the local pad queue mid-start. Trigger the reset on the first `0xB0` whose `frame == 1` after the previous `0xB0` had another frame, or after a `0xB3`/`0xBA`.
3. `ready = for every active remote i: so_np_remote_pad(i, frame) is available`, i.e. `latest_contiguous[i] >= frame`.
4. If not `ready`: answer `2` (SKIP), and only resend un-acked pads (`so_np_send_pad(NULL)`), as Dolphin does.
   - Do not send a pad for `frame + delay`: the ASM reads a fresh local pad on the next poll.
   - Optional deviation: on the first frame-1 poll, send the `delay` dummy pads (frames `1..delay`) at once, even while answering SKIP. They do not depend on local input, and the PC stops waiting for them sooner.
5. If `ready`: send `SlippiPad(frame + delay, finalized_frame, checksum, pad[0..7])`, plus the dummies on the first frame 1, then answer `1`.
6. **Always fill the whole 297-byte RXB**, even on SKIP: the ASM consumes it when SKIP arrives after game over.
   - Count: `remote_count`.
   - Checksum entries: from the latest pad packet per remote.
   - `FRAME_NUMS[i] = min(latest_contiguous[i], frame)`, unused slots `frame`; then the smallest.
   - Pads: pad `k` = remote pad for `FRAME_NUMS[i] − k` (`k` = 0..6, zero if unavailable).
   - Despawn flags 0 in 1v1.
7. Never answer 4 in the minimal device. The Wii cannot run ahead of the opponent's input, and if it is behind, the PC absorbs the difference: it predicts and rolls back, slows itself 0.5% when ahead, and on frames ≤120 skips up to 5 frames when more than 10 ms ahead.
   - Optional later: implement Dolphin's advance rule using `so_np_time_offset_us()`. It returns 4 at most 3 frames per 30, only on `frame % 5 == 0`.
   - Answer 4 only if the pad for `frame` is present. The extra engine frame polls `frame + 1` in the same VI, which will normally get SKIP.
8. No "rollback limit" skip or 7-second force-disconnect is needed: lockstep already waits.
   - A stall timeout is still advisable. After N seconds without the needed pad, treat the opponent as gone (`so_np_force_disconnect`) and answer 3.
   - Dolphin's equivalent is 420 stalled frames (:1405-1413).
9. `finalized_frame` from the TXB still drives `DropOldRemoteInputs` and is the checksum frame sent with each pad. In lockstep the ASM's finalized frame is `frame − 1`.
   - Volatile finalized = frame once all inputs are real (`TriggerSendInput.asm:895-899`); the stable copy follows in `StartEngineLoop.asm:416-429`.

---

### 0xB1 CAPTURE_SAVESTATE / 0xB2 LOAD_SAVESTATE (no-op in lockstep)

**Request.** 33 bytes, `SSRB` (`A/Online/Online.s:131-142`; filled at `InitOnlinePlay.asm:94-106`):

| Offset | Field |
|---|---|
| 0x00 | 0xB1 or 0xB2 |
| 0x01 | s32 frame |
| 0x05 / 0x09 | u32 ODB address / size |
| 0x0D / 0x11 | u32 RXB address / size (297) |
| 0x15 / 0x19 | u32 SSCB address / size |
| 0x1D | u32 0 terminator |

These are the "preserve blocks": memory left untouched when a state loads.

Dolphin:

- `0xB1` (`EXI_DeviceSlippi.cpp:1800-1839`) returns at once if disconnected. Otherwise it takes a savestate object from a pool of 7, evicting the oldest, and `Capture()`s the full emulated state into `active_savestates[frame]`.
- `0xB2` (:1841-1882):
  - if there is no state for `frame`, log an error and **return without doing anything**;
  - otherwise read `(addr, size)` pairs from `payload+4` until an address is 0, `Load(blocks)`, and move every active state back to the pool.
- Neither prepares an answer.

The ASM only sends `0xB1` when `ODB_SAVESTATE_IS_PREDICTING` is set (`StartEngineLoop.asm:638-655`), and `0xB2` only after a misprediction (`StartEngineLoop.asm:318-356`). Under the lockstep rule in `0xB0`, `ODB_SAVESTATE_IS_PREDICTING` is never set, so neither command is sent, except one `0xB1` possible right after a disconnect.

**Console answer:** accept and ignore both; no answer. If a `0xB2` ever arrives, a lockstep invariant was broken and the game will diverge. Log it loudly as an assertion.

---

### 0xB3 GET_MATCH_STATE (essential)

**Request.** 1 byte, `0xB3`, written from the start of the MSRB itself; then a 962-byte read into the same buffer (`A/Online/Static/LoadMatchState.asm:21-46`).

When it is called:

- **every CSS frame** (`A/Online/Menus/CSS/FetchMatchInfo.asm:9-16`, hook 0x80260310);
- at scene decisions: `main.asm:534`, `:600`, `:896`, `:1143`, `:1391`;
- on the VS splash (`InitVsSplash.asm:110`, `PlayOpponentCharAnnouncer.asm:20`);
- at game start, **twice in the same frame**: `InitOnlinePlay.asm:111-146` copies ports, RNG, game info block, alt stage mode and delay; `A/Recording/SendGameInfo.asm:400-416,585-623` copies names, codes, UIDs and match id into the replay;
- by in-game HUD init (`InitInGame.asm:195`);
- by `SkipReturnToCssSound.asm:24` and `ExitSSSUponStageSelect.asm:28`.

It must be cheap and idempotent, apart from the one-shot chat fields below.

**Response.** `MSRB`, 962 bytes (`A/Online/Online.s:314-348`). Built by `prepareOnlineMatchState`, `D/HW/EXI/EXI_DeviceSlippi.cpp:2081-2738`. Dolphin appends 68 more bytes (ranked rating info and bot flags, :2723-2737) that this ASM never reads, because the read length truncates them. The kernel may omit them.

| Offset | Size | Field | Dolphin source / encoding |
|---|---|---|---|
| 0x000 | u8 | connection state: 0 `IDLE`, 1 `INITIALIZING`, 2 `MATCHMAKING`, 3 `OPPONENT_CONNECTING`, 4 `CONNECTION_SUCCESS`, 5 `ERROR_ENCOUNTERED` (`Online.s:307-312`; `D/Slippi/SlippiMatchmaking.h:46-54`) | :2111-2124. It is 5 whenever `forced_error` is non-empty. |
| 0x001 | u8 | local player ready = `local_selections.is_character_selected` | :2126, :2276 |
| 0x002 | u8 | remote players ready | :2127, :2193-2201, :2262-2265, :2277 |
| 0x003 | u8 | local player index (port 0..3) | :2134, :2203-2208, :2278 |
| 0x004 | u8 | remote player index | :2207, :2279 |
| 0x005 | u32 | RNG offset (0 unless both ready) | :2443, :2593 |
| 0x009 | u8 | online delay frames (Dolphin `SLIPPI_ONLINE_DELAY`) | :2596 |
| 0x00A | u8 | `MSRB_USER_CHATMSG_ID`: id of the chat message **we** sent, one-shot | :2286, :2599 |
| 0x00B | u8 | `MSRB_OPP_CHATMSG_ID`: id of the chat message received, one-shot | :2288-2300, :2600 |
| 0x00C | u8 | `MSRB_CHATMSG_PLAYER_INDEX` | :2301-2309, :2601 |
| 0x00D | s8 | `MSRB_USER_RANK`: Dolphin writes **port 0's** rank (`p1_rank`); 0 unless ranked; −1 if hidden | :2603-2622 |
| 0x00E | s8 | `MSRB_OPP_RANK`: Dolphin writes **port 1's** rank (`p2_rank`) | :2623 |
| 0x00F | 31 | local display name, game string with N = 15 (see 4.1). **Empty unless a netplay client exists (connected).** | :2311, :2627-2628 |
| 0x02E | 4 × 31 | P1..P4 display names, from `matchmaking->GetPlayerName(i)` (the matchmaking `players[i].displayName`, `""` past the end) | :2634-2642; `SlippiMatchmaking.cpp:678-685` |
| 0x0AA | 31 | opponent text (below) | :2645-2672 |
| 0x0C9 | 4 × 10 | P1..P4 connect codes, from `players[i].connectCode` (see 4.2) | :2678-2687 |
| 0x0F1 | 4 × 29 | P1..P4 Slippi UIDs: ASCII, 28 characters + NUL, zero-padded | :2694-2703 |
| 0x165 | 241 | error message, game string with N = 120: `forced_error` if set, else the matchmaking error | :2706-2708 |
| 0x256 | 0x138 | **game info block** (below) | :2711 |
| 0x38E | 51 | match id (`recent_mm_result.id`), ASCII, zero-padded or truncated to 51 | :2714-2716 |
| 0x3C1 | u8 | alt stage mode | :2719 |
| 0x3C2 | | end of ASM read (962) | |
| (0x3C2) | 4 × 16 | Dolphin extra: per player `f32 rating, u32 update_count, u16 global, u16 regional, f32 mmr` | :2723-2731 |
| (0x402) | 4 × u8 | Dolphin extra: `is_bot` | :2734-2737 |

**Connection logic** (:2132-2230):

- **State is not `CONNECTION_SUCCESS`:** drop the netplay client (`slippi_netplay = nullptr`, :2227-2230). Names and chat stay empty. Ready flags: local = `is_character_selected`; remote = 0.
- **State is `CONNECTION_SUCCESS`:**
  1. `m_local_player_idx = matchmaking->LocalPlayerIndex()`, the matchmaking port − 1 (`SlippiMatchmaking.cpp:579`).
  2. First time, when no client is held yet (:2136-2166):
     - take the netplay client;
     - copy the matchmaking result into `recent_mm_result`;
     - set `allowed_stages` to the result's stages, or to {2, 3, 8, 0x1C, 0x1F, 0x20} if that is empty;
     - clear `stage_pool`;
     - set `local_selections.stage_id = getRandomStage()` (`is_stage_selected` stays as it was);
     - `SetMatchSelections(local_selections)`, which **sends our selections to the peer now** (`SlippiNetplay.cpp:1336-1345`; packet `0x82`, `:564-573`). Selections made before the connection (`0xB5` comes before `0xB4`) go out here.
  3. `is_connected = status == CONNECTED`. It is also false if `GetActivePlayerIndices().size() != RemotePlayerCount()` (:2171-2181).
  4. If not connected: `handleConnectionCleanup()` (as `0xBA`), then **recompute the whole answer** with the new state, normally `IDLE` (:2211-2219).
  5. If connected:
     - `remote_players_ready = 1` if every remote's `match_info.remote_player_selections[i].is_character_selected`;
     - **in 1v1, `m_local_player_idx = IsDecider() ? 0 : 1` and `m_remote_player_idx = IsDecider() ? 1 : 0`.** The decider is the matchmaking `isHost` (`SlippiMatchmaking.cpp:611`, `:844-845`). The game port comes from the decider flag, not from the matchmaking port.
- **Desync recovery** (:2255-2274; ranked only in practice): while recovering and waiting, report remote ready as 0. On a recovery error, clean up and recompute.

**Chat fields** (:2282-2312), only while a netplay client exists:

- `sent = GetSlippiRemoteSentChatMessage(chat_enabled)`. It returns `remote_sent_chat_message_id` and clears it, so it is one-shot (`SlippiNetplay.cpp:1431-1441`).
- If `sent == 0`, take the received message (`GetSlippiRemoteChatMessage`, `SlippiNetplay.cpp:1396-1429`, which also clears it): `id = message_id`, `player_idx = sender port`.
  - With chat disabled and a non-zero id other than `0x10`, Dolphin instead sends a `CHAT_DISABLED` (`0x10`) chat back.
- In 1v1: `chat_player_idx = sent ? m_local_player_idx : m_remote_player_idx`.
- `chat_enabled` (:2924-2939) is true in Direct when the setting is ON or DIRECT_ONLY.

**When both are ready** (:2314-2549), build the per-game fields:

1. `lps = match_info.local_player_selections`, which carries `player_idx` = the matchmaking port index (`SlippiNetplay.cpp:1339`); `rps[]` = the remote selections. Order them by `player_idx`.
2. Apply `overwrite_selections`, if `0xBF` set any (:2374-2382).
3. Stage = the first selection, in port order, with `is_stage_selected`. Otherwise **0x1F (Battlefield)**. `alt_stage_mode` comes from the same selection (:2384-2395).
4. Fixed-rules modes (Unranked 1, Ranked 0, Party 4; `SlippiMatchmaking.cpp:87-92`) and Teams validate characters (< 26) and stages, and on failure clean up with a `forced_error` (:2397-2440). **Direct does none of this.**
5. `rng_offset = IsDecider() ? lps.rng_offset : rps[0].rng_offset` (:2443).
6. Teams only: team assignment (:2445-2486).
7. Write into the block, per selected player `p` (:2467-2486):
   - `[0x60 + p*0x24] = character_id`;
   - `[0x63 + p*0x24] = color`;
   - `[0x67 + p*0x24] = 0`;
   - `[0x69 + p*0x24] = team_id` (0 unless Teams).
8. Shade fix (:2488-2511): for each human slot (`[0x61 + i*0x24] == 0`), key = (`char`, with Sheik 0x13 counted as Zelda 0x12) and color, or team in Teams. `[0x67 + i*0x24]` = number of earlier slots with the same key.
9. Other fields:
   - `[0x08]` = 1 if Teams, else 0;
   - `[0x61 + 2*0x24]` = 0 if there are ≥ 2 remotes, else 3 (none);
   - `[0x61 + 3*0x24]` = 0 if there are ≥ 3 remotes, else 3;
   - `[0x0E..0x0F]` = stage (u16 BE) (:2512-2522).
10. Pause: `[0x02] &= ~0x08` in Direct (pause allowed), `|= 0x08` otherwise (:2524-2527).
11. `alt_stage_mode = 0` unless Direct or Teams (:2530-2536).
12. Desync-recovery fighter state, applied always; the defaults give 4 stocks and 0%:
    - `[0x62 + i*0x24] = stocks`;
    - `[0x70 + i*0x24]` = u16 health (:2538-2548).

**Always** (:2551-2590), whether ready or not:

- Mode: Party sets `[0x00] = 0x12`, `[0x03] = 0xCC`, timer `[0x10..0x13]` = 300 s. Every other mode sets `[0x00] = 0x32` (stock mode, timer on), `[0x03] = 0x4C`, timer = 480 s.
- Recovering and both ready: timer = the synced `seconds_remaining`.
- Items: `[0x0B] = 0xFF`, and the 8 bytes at `[0x23..0x2A]` = `0xF80000000F000000`. If `recent_mm_result.items != 0`, OR in `items << 28` and set `[0x0B] = 3`. Direct: items = 0.

**The game info block persists.** `online_match_block` is a function-`static` vector (:2085-2107), not rebuilt per call. Fields written while both were ready stay in later answers. The ASM relies on this: after a game, `VSSceneDecide` reads `MSRB_GAME_INFO_BLOCK + 0xE` as "last stage played" (`main.asm:683`). The kernel must keep one persistent 0x138-byte block, initialised once from Dolphin's default bytes (:2086-2106), and patch it as above. The defaults are P1 red Falco, P2 red Bowser, P3/P4 Young Link (type 3 = none), Battlefield, with 0x86 at `[0x02]`.

The ASM copies the whole block into the game:

- 0x138 bytes (`0x60 + 0x24*6`) into the VS/SSS data (`main.asm:1146-1157`, `FN_AdjustNullID`);
- `MATCH_STRUCT_LEN` into StartMelee's game info (`InitOnlinePlay.asm:138-142`).

**Opponent text** (:2645-2672). Collect the names of the other players that are human (`[0x61 + i*0x24] == 0`) and not the local index; in Teams, skip teammates (`[0x69]` equal). Keep only non-empty names. With `n` names, each is cut to `(15 − (n − 1)) / n` code points, they are joined with "/", and the result is encoded as a game string with N = 15. In 1v1 that is the opponent's display name.

**The ASM's state handling on the CSS** (`HandleInputsOnCSS.asm:99-330`):

| State | ASM behaviour |
|---|---|
| ≤ 0 (IDLE) | Start: Direct/Teams open code entry; other modes lock in and search |
| 1-3 (searching) | B: `0xBA` |
| 4 (connected) | Hold Z 3 s: `0xBA`. Start locks in (`0xB5`). Leaves the CSS when bytes 1 **and** 2 are both 1. |
| 5 (error) | B: `0xBA`; the error text comes from offset 0x165 (`LoadCSSText.asm:681-751`) |

Sounds play on the transitions into 5 and out of 4 (:39-94). The SSS/VS scene decisions compare bytes 1 and 2 (`main.asm:537-557`).

---

### 0xB4 FIND_OPPONENT (essential)

**Request.** 20 bytes, `FMTB` (`Online.s:389-392`; `HandleInputsOnCSS.asm:335-382`):

| Offset | Field |
|---|---|
| 0x00 | 0xB4 |
| 0x01 | online mode: 0 Ranked, 1 Unranked, **2 Direct**, 3 Teams, 4 Party (`Online.s:70-74`; `SlippiMatchmaking.h:37-44`) |
| 0x02 | 18 bytes: opponent connect code as typed. The name-entry buffer at 0x804a0740 holds 3-byte cells; the ASM copies the first 2 bytes of each of 9 cells (:358-370). These are full-width Shift-JIS, e.g. 0x8260 'Ａ', with `#` = 0x8194. Unused cells are 0. |

The ASM sends `0xB5` (lock-in) first and then `0xB4` (`FN_LOCK_IN_AND_SEARCH`, :496-508). For the first Direct game the stage behaviour is `SB_RAND` (`OnConfirmButtonHandler.asm:61`). It also resets the game-prep game index (:338-343).

Dolphin (`startFindMatch`, `EXI_DeviceSlippi.cpp:1884-1961`):

1. `mode = payload[0]`; `code = payload[1..18]` **cut at the first 0x00 byte**, still in Shift-JIS.
2. Direct: add the code to the direct-code history (`SHIFTJISToUTF8`, then `direct_codes->AddOrUpdateCode`). Teams uses the teams history (:1895-1907).
3. `last_search = {mode, code}`.
4. Fixed-rules modes: if the character is ≥ 26 or the stage is not allowed, set `forced_error` and return without searching (:1919-1946). That surfaces as state 5 in the next `0xB3`.
5. `matchmaking->FindMatch(search)`, which **sets the state to `INITIALIZING` synchronously** (`SlippiMatchmaking.cpp:56-65`) and starts the search thread.
   - The connect code goes to the server as the raw Shift-JIS bytes (`SlippiMatchmaking.cpp:442-443`). `so_mm` already sends a byte array (`N/slippi_online/so_mm.h:37-39`, `so_mm_msg.c:27-28`).

No answer. **Console:** the next `0xB3` must already report ≥ 1, so set the state before returning. If a search is already running, clean up first (`0xBA` semantics). Store the raw bytes in the code history used by `0xBE` (16 bytes for 8 characters).

---

### 0xB5 SET_MATCH_SELECTIONS (essential)

**Request.** 10 bytes, `PSTB` (`Online.s:367-376`; `HandleInputsOnCSS.asm:391-487`):

| Offset | Field |
|---|---|
| 0x00 | 0xB5 |
| 0x01 | team id (Teams: CSS team index − 1; otherwise 0) |
| 0x02 | character id (CSS external id, from `-0x49f0(r13)` + port × 0x24 + 0x70) |
| 0x03 | character color (+0x73) |
| 0x04 | character option: the ASM always sends 1 (merge). Dolphin takes it as `is_character_selected` (non-zero = selected). |
| 0x05 | u16 stage id |
| 0x07 | stage option: 0 unset, 1 picked, 2 clear, 3 random |
| 0x08 | online mode (ignored by Dolphin) |
| 0x09 | alt stage mode (from the frozen-stadium injection data) |

How the ASM fills the stage fields (:439-463):

| Stage behaviour | Stage id | Option |
|---|---|---|
| `SB_RAND` (−2) | 0 | 3 |
| `SB_NOTSEL` (−1, the winner in Direct) | 0 | 0 |
| a chosen stage `k` (the loser after the SSS) | `k` | 1 |

Dolphin (`setMatchSelections`, :2758-2792):

1. Build `s` with `team = p[0]`, `char = p[1]`, `color = p[2]`, `is_char_sel = p[3]`, `stage = u16(p[4..5])`, `alt = p[8]`.
2. `is_stage_selected = (opt == 1 || opt == 3)`. If `opt == 3`, `stage = getRandomStage()`:
   - draw from `stage_pool` without replacement;
   - refill it from `allowed_stages` when empty (:2740-2756).
3. `s.rng_offset = rand() % 0xFFFF`.
4. `local_selections.Merge(s)` (`SlippiNetplay.h:102-120`). `rng_offset` is always taken. The stage fields are taken only if selected; the character, color and team only if selected. "Clear" (2) therefore clears nothing.
5. If connected, `SetMatchSelections(local_selections)` sends packet `0x82` (format in `N/slippi_online/so_netplay.h:4-14`).

No answer.

---

### 0xB6 OPEN_LOGIN, 0xB7 LOGOUT, 0xB8 UPDATE (no answer)

Each is a 1-byte write from the online submenu (`OnMenuPrep.asm:336-357,373-379,816-824`).

| Cmd | Dolphin | Console answer |
|---|---|---|
| B6 | `handleLogInRequest`: `AttemptLogin()` re-reads `user.json`; on failure opens the browser login page and listens (:2979-2988) | Re-read `/slippi-online/user.json`. The option only shows when B9 reports `app_state` 0. |
| B7 | `LogOut()`: the Rust side deletes `user.json` (:2990-2993) | Do **not** delete the SD file. Forget the in-memory account, so B9 returns 0, or ignore. |
| B8 | `user->UpdateApp()`: runs the updater (:3501-3503) | Ignore. Never return `app_state` 2, so the option never shows. |

---

### 0xB9 GET_ONLINE_STATUS (essential)

**Request.** 1 byte. The menu reads it into `OSB` (`UserDisplayFunctions.asm:399-434`). The result drives which submenu options are locked (`HandleOnlineLockedOptions.asm:27-60`): with 0, Ranked/Unranked/Direct/Teams/Party are locked.

**Response.** 42 bytes (`Online.s:461-464`; built at `EXI_DeviceSlippi.cpp:2995-3021`):

| Offset | Size | Field |
|---|---|---|
| 0x00 | u8 | `app_state`: 0 not logged in (no `uid`), 1 logged in, 2 logged in but `latestVersion` > Dolphin's semver |
| 0x01 | 31 | display name, game string with N = 15 |
| 0x20 | 10 | own connect code (see 4.2) |

**Console:** 1 when `user.json` parsed and has a uid, else 0. Never 2.

---

### 0xBA CLEANUP_CONNECTION (essential)

1-byte write, no answer. Sent from the CSS (B while searching or on error, hold Z while connected), from `VSSceneDecide` after Party, ranked set end or disconnect, and on every main-menu load (`OnMenuLoad.asm:82-89`).

Dolphin (`handleConnectionCleanup`, :3033-3067):

- tear down matchmaking and netplay on a background thread;
- create a fresh matchmaking object (state `IDLE`) and drop the netplay client;
- `local_selections.Reset()` (`SlippiNetplay.h:122-133`);
- clear `stage_pool`, `forced_error` and `overwrite_selections`; `is_play_session_active = false`.

`m_local_player_idx`, `m_remote_player_idx`, `recent_mm_result`, `last_search` and the static match block are **kept**.

**Console:** `so_client_cancel()` / `so_np_stop()` plus the resets above, without blocking the EXI handler. The next `0xB3` must report state 0 at once.

---

### 0xBB SEND_CHAT_MESSAGE (optional; needed for quick chat)

**Request.** 2 bytes, `CMTB` (`Online.s:382-384`): `[0xBB, message_id]`. The ids are pad-direction codes such as 0x88 = Up/Up "ggs" (`D/Slippi/SlippiPremadeText.h:26-46`). Issued by `slpCSS.dat`.

Dolphin (:2941-2962): if chat is enabled and connected:

- set `remote_sent_chat_message_id = id`; the next `0xB3` reports it at 0x00A, once;
- send packet `0x84` (`s32 message_id`, `u8 local player port`; `SlippiNetplay.cpp:575-579`).

No answer.

**Console minimal:** ignore it, so chat never appears; or forward with `so_np_send_chat(id)` and set the one-shot field. A received chat comes from `so_np_take_chat()` into 0x00B/0x00C.

---

### 0xBC GET_NEW_SEED (essential, trivial)

1-byte write; 4-byte read, copied to Melee's RNG seed at 0x804D5F90 after every online game (`main.asm:840-869`). It stops both clients drawing the same random character. Dolphin: `rand() % 0xFFFFFFFF`, u32 BE (:3069-3076). **Console:** any local random u32. It does not need to match the peer.

---

### 0xBD REPORT_GAME (optional; no answer)

**Request.** 369 bytes, `RGB` (`Online.s:469-490`; `InitOnlinePlay.asm:349-453`, sent once the game is confirmed over). Dolphin parses it as `ReportGameQuery` (`D/Slippi/SlippiExiTypes.h:16-38`, byte-swapped at :123-138):

| Offset | Field |
|---|---|
| 0x00 | 0xBD |
| 0x01 | u8 online mode |
| 0x02 | u32 frame length |
| 0x06 | u32 game index (1-based) |
| 0x0A | u32 tiebreak index |
| 0x0E | s8 winner index (−3 disconnect, −2 desync) |
| 0x0F | u8 game-end method (7 = LRAS) |
| 0x10 | s8 LRAS initiator (−1 = none) |
| 0x11 | u32 synced timer |
| 0x15 | 4 × 9-byte player records: `u8 slot type`, `u8 stocks`, `f32 damage done`, `u8 synced stocks`, `u16 synced health` |
| 0x39 | 0x138-byte game info block |

Dolphin (:3078-3157) builds a report with `uid`, `play_key` and match id, sent to Slippi's server through the Rust reporter for every online mode. For Ranked with end method 7 it also sends the netplay synced-state packet `0x86`.

**Console:** ignore. Reports are server bookkeeping, and Direct results are not ranked. Optionally forward later.

---

### 0xBE FETCH_CODE_SUGGESTION (optional; connect-code autocomplete)

**Request.** 32 bytes, `ACTXB` (`A/Online/Menus/CSS/TextEntryScreen/AutoComplete.s:15-22`; `CheckTriggersAndZ.asm:112-163`). Only sent in connect-code entry mode.

| Offset | Field |
|---|---|
| 0x00 | 0xBE |
| 0x01 | 24 bytes: typed text, 8 cells × (Shift-JIS high byte, low byte, 0) |
| 0x19 | u8 number of committed characters |
| 0x1A | u32 current scroll index (unaligned) |
| 0x1E | u8 scroll direction: 0 none, 1 older (index + 1), 2 newer (index − 1), 3 reset (`AutoComplete.s:38-42`) |
| 0x1F | u8 online mode (Teams uses the teams history) |

**Response.** 30 bytes, `ACRXB` (`AutoComplete.s:8-13`). The ASM copies bytes 1..24 back into the name-entry buffer and stores the new index (`CheckTriggersAndZ.asm:165-173`).

| Offset | Field |
|---|---|
| 0x00 | u8 has suggestion |
| 0x01 | 24 bytes: 8 cells × (Shift-JIS 2 bytes, 0) |
| 0x19 | u8 length in characters |
| 0x1A | u32 new index |

Dolphin (`handleNameEntryLoad`, :1983-2079):

1. Adjust the index by direction.
2. Walk the history, sorted most recently played first (`R/user/src/direct_codes/mod.rs:106-121`; out of range returns `"1"`). Stop at the first code whose first `len` characters equal the typed cells, comparing Shift-JIS bytes 0 and 1 of each cell (:1963-1981).
   - Direction 2 walks downward; unsigned underflow ends the loop.
3. If nothing is found, retry the initial index.
4. Not found: `[0]`, the typed bytes echoed (`3*len`), zeros to 24, `len`, the initial index.
5. Found: `[1]`, the code as 8 cells × (2 Shift-JIS bytes, 0), `strlen(sjis)/2`, the new index.

**Console minimal:** always answer "not found" with the echo. Codes from `0xB4` are stored as raw Shift-JIS, so a real history needs no character conversion.

---

### 0xBF OVERWRITE_SELECTIONS, 0xC0 GP_COMPLETE_STEP, 0xC1 GP_FETCH_STEP (ranked game prep; optional)

These are issued by `GameSetup.dat` (the ranked game-prep scene, minor 80, `main.asm:267-276`), which the ASM only enters in Ranked (`main.asm:420-447`).

| Cmd | Request (`SlippiExiTypes.h`) | Dolphin | Answer |
|---|---|---|---|
| BF | 15 bytes: `u8 cmd`, `u16 stage_id`, 4 × {`u8 is_set`, `u8 char_id`, `u8 color`} (:76-87, swap :140-146) | Fills `overwrite_selections` (stage forced as selected), applied in `0xB3` ready building (:3173-3197) | none |
| C0 | 6 bytes: `cmd`, `step_idx`, `char_selection`, `char_color_selection`, `stage_selections[2]` (:52-59) | Netplay packet `0x85` (`SlippiNetplay.cpp:1347-1356`) (:3199-3211) | none |
| C1 | 2 bytes: `cmd`, `step_idx` (:61-65) | Pop the game-prep queue until `step_idx` matches (`SlippiNetplay.cpp:1378-1394`) (:3213-3249) | **6 bytes**: `is_found`, `is_skip`, `char`, `color`, `stage_selections[2]` (:67-74) |

**Console minimal:** parse and ignore `0xBF`/`0xC0`. Answer `0xC1` with 6 zero bytes (not found). Ranked is out of scope (`FEASIBILITY.md` risk 7).

---

### 0xC2 REPORT_SET_COMPLETE, 0xC4 REPORT_MATCH_STATUS_UPDATE (ranked reporting; ignore)

- `0xC2`: the ASM writes `[0xC2, end_mode]` but transfers **length 1** (`main.asm:564-582`: `li r4,1`). Dolphin still reads `end_mode` from RAM. It only acts if the match id contains `mode.ranked` (:3251-3261).
- `0xC4`: `[0xC4, status_idx]`, length 2. It is sent at ranked game start with index `game + 19` (`InitOnlinePlay.asm:174-184`). Dolphin maps the index through `status_idx_map` (`EXI_DeviceSlippi.h:125-136`) and reports, Ranked only (:3263-3286).

**Console:** ignore both, with no answer. Handle `0xC2` as a 1+1-byte command regardless of the DMA length.

---

### 0xC3 GET_PLAYER_SETTINGS (needed if the CSS `.dat` reads it; cheap)

1-byte write. **Response: 3264 bytes** = `GetPlayerSettingsResponse { PlayerSettings settings[4]; }`, where `PlayerSettings = char chat_messages[16][51]` (`SlippiExiTypes.h:89-96`). Message `j` of player `i` is at `i*816 + j*51`.

Dolphin (`handleGetPlayerSettings`, :3288-3329):

- start from all zeros;
- `messages[0]` = the user's own 16 messages, if `user.json` has exactly 16;
- for each matchmaking player: `messages[port − 1]` = that player's `chatMessages` (the matchmaking JSON, falling back to the defaults; `SlippiMatchmaking.cpp:549-560`);
- any player without exactly 16 gets the defaults (`R/user/src/chat.rs:3-20`): "ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good", "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection";
- each string is encoded as a game string with N = 25 and written with `sprintf("%s")`, so it is NUL-terminated inside its 51 bytes.

**Console minimal:** the 16 defaults for all 4 players, pre-encoded once. They are pure ASCII, so the bytes are ASCII with `'` converted to its full-width form.

---

### 0xD0 LOG_MESSAGE (ignore or forward to the log sink)

128-byte write from the `logf` macro (`Common.s:238-277`): `[0xD0, time_flag(0), level, NUL-terminated text...]`. Levels: `Common.s:637-640`. Dolphin logs `payload+2` (:2964-2977). Its size of 0xFFFF ends parsing of that DMA. No answer.

---

### 0xD1 FILE_LENGTH / 0xD2 FILE_LOAD (essential: Slippi's menu and m-ex files)

**Request.** `[0xD1 or 0xD2, NUL-terminated file name]`, DMA length `strlen + 2`. The scene buffer (`OFST_R13_SB_ADDR`) is used.

**Every file load the game makes asks `0xD1` first**: hooks at 0x800163fc (`GetFileSize.asm`) and 0x800166b8 (`TransferFile.asm`).

- `0xD1` answer: 4 bytes, u32 BE length. `0` (or ≤ 0) means "no replacement": the game loads from disc as normal (`GetFileSize.asm:46-50`, `TransferFile.asm:50-53`).
- `0xD2` answer: the whole file, read **directly into the game's allocation**, read length = the `0xD1` length (`TransferFile.asm:77-81`).

Dolphin (`prepareFileLength` / `prepareFileLoad`, :2794-2823; `D/Slippi/SlippiGameFileLoader.cpp:108-162`), cached per name:

- If `Sys/GameFiles/GALE01/<name>` exists, serve it.
- If `<name>.diff` exists, VCDIFF-patch the disc's `<name>` with it.
- Then resolve the file's `slp:` links to disc assets through Rust (`slprs_gamefile_resolve`, :47-100).
- `GrPs1.dat`..`GrPs4.dat` (Pokémon Stadium transformations) are served from the disc itself (:18 in the `.h`, :108-120), so mid-match transformation loads become instant EXI reads (`A/Online/Core/Hacks/Stadium/StadiumFileLoad.asm`).
- Everything else: 0.

The files and sizes in Dolphin's tree (`git cat-file -s`):

| File | Bytes |
|---|---|
| GameSetup.dat | 29,359 |
| GameSetup_gui.dat | 788,239 |
| MxDb.dat | 444,957 |
| MxScn.dat | 10,060 |
| SlippiCSS.dat | 24,143 |
| SlippiCSS_gui.dat | 237,571 |
| MnExtAll.dat.diff | 149 |
| MnMaAll.usd.diff | 186,319 |
| MnSlMap.usd.diff | 58,850 |
| SdMenu.usd.diff | 34,990 |
| SdSlChr.usd.diff | 2,513 |

Plus the `.dat` and `.usd` variants. The patched outputs are larger than the diffs.

**Console:**

- Pre-build the resolved files on the PC (the M4 builder) and store them on SD.
- Answer `0xD1` from a small name table, fast: it runs for every file.
- Answer `0xD2` by streaming the file into the PPC buffer.
- Files of 64 KiB or more cannot pass through Nintendont's 16-bit DMA length (section 1.4, item 6). That needs a fix or a patched ISO (section 6, surprise 1).
- Optional: serve `GrPs1-4.dat` too. With `0xD1 = 0` the hack falls back to a **synchronous** disc read mid-match: deterministic, but it stalls the frame.

---

### 0xD3 GCT_LENGTH / 0xD4 GCT_LOAD (only for the bootloader route)

Sent by Slippi's bootloader code (`A/Bootloader/main.asm`) if M4 uses it.

- `0xD3`: 1-byte write; answer u32 = `8 * code_lines + 16` (`D/Core/GeckoCode.cpp` `GetGctLength`).
- `0xD4`: write `[0xD4, u32 destination address]`; answer the GCT, read length = the size rounded up to 32 (`main.asm:66-90`):
  - `00D0C0DE 00D0C0DE`, then `address data` u32 pairs, then `FF000000 00000000` (`GenerateGct`);
  - the ASM then processes the codes from `buffer + 8` (`main.asm:94-97`).

Dolphin also runs `ConfigureJukebox()` on `0xD4` (:3513-3516). **Console:** serve a prebuilt `.gct` from SD (about 58 KB per `FEASIBILITY.md` M4, close to the 64 KiB DMA limit).

---

### 0xD5 GET_DELAY (only if "Apply Delay to all In-Game Scenes" is in the codeset)

1-byte write; 2-byte read at the start of every **offline** in-game scene (`InitializeInGameDelay.asm:103-119`; it does nothing in online scenes, major 8).

- byte 0 = 1 ("real response");
- byte 1 = delay frames, clamped 0..15 (`InGameDelay.s:11-12`).

Dolphin answers `[1, SLIPPI_ONLINE_DELAY]`, or `[1, 0]` under old-style netplay (:3159-3171). **Console:** `[1, 0]` (no added local delay), or leave the code group out.

---

### 0xD6 PLAY_MUSIC, 0xD7 STOP_MUSIC, 0xD8 CHANGE_MUSIC_VOLUME (ignore)

Write-only (`SlippiExiTypes.h:98-109`):

- `0xD6` = `[cmd, u32 disc offset, u32 size]` of the HPS file (`StartSong.asm:39-66`);
- `0xD7` = `[cmd]`;
- `0xD8` = `[cmd, u8 volume]`.

Dolphin's Jukebox streams the music from the ISO, and the codeset mutes the game's own music (`A/Online/Core/PreventFileAlarms/MuteMusic.asm`, `Online/Core/Music/*`). **Console:** ignore these. Build the codeset **without** the music-mute and Jukebox injections so the Wii plays music natively (`FEASIBILITY.md` M4).

---

### 0xE1 PREMADE_TEXT_LENGTH / 0xE2 PREMADE_TEXT_LOAD (needed for the logout dialog and chat text)

**Request.** 3 bytes for both: `[cmd, text_id, param]` (`LoadPremadeText.asm:66-81`). `0xE1` reads 4 bytes (u32 length); `0xE2` reads that many bytes. It is used when `OFST_R13_USE_PREMADE_TEXT` is set (`LoadPremadeTextDataFromDolphin.asm`, hook 0x803a63a8).

Text ids and templates (`D/Slippi/SlippiPremadeText.h:17-82`):

| Id | Template |
|---|---|
| 1..4 | `SPT_CHAT_P1..P4`: `<LEFT><KERN><COLOR, r, g, b>%s:<S><COLOR, 255, 255, 255>%s<END>`; name colours (229,76,76), (59,189,255), (255,203,4), (0,178,2) |
| 5 | `SPT_LOGOUT`: `<FIT><COLOR, 243, 75, 75>Are<S>You<COLOR, 0, 175, 75><S>Sure?<END>` |
| 6 | `SPT_CHAT_DISABLED`: `<LEFT><KERN><COLOR, 0, 178, 2>%s<S><COLOR, 255, 255, 255>has<S>chat<S>disabled<S><END>` |

Dolphin (`loadPremadeText`, `EXI_DeviceSlippi.cpp:2860-2922`):

1. For ids 1..4: `name = matchmaking->GetPlayerName(id − 1)`. In the name, `<` becomes `\` and `>` becomes `` ` `` (to avoid tag injection), commas are removed, and spaces become `<S>`.
2. If `param == 0x10`: use template 6 with the name. Otherwise fill template `id` with the name and `premadeTextsParams[param]` (the fixed English string for that pad code, spaces → `<S>`). An unknown `param` makes Dolphin's `.at()` throw.
3. Other ids: the template as is.
4. Encode with `GetPremadeTextData` (`SlippiPremadeText.h:93-237`):
   - split on `>` then `<`;
   - a token naming a known tag emits its opcode byte plus parameters (`b` = u8, `s` = u16 BE; table :275-303). For example `COLOR` = 0x0C r g b, `S` = 0x1A, `LEFT` = 0x12, `KERN` = 0x16, `FIT` = 0x18, `END` = 0x00;
   - any other token is text: each code point that appears in the 287-entry `CHAR_MAP` (:405-428) emits u16 BE `0x2000 | index`; others are dropped;
   - append a final 0x00.

`0xE1` returns `u32(len)`; `0xE2` returns the bytes.

Worked example. Logout (`0xE1 05 xx`) has length 35 (0x23), and `0xE2 05 xx` returns:

`18 0C F3 4B 4B 20 0A 20 35 20 28 1A 20 22 20 32 20 38 0C 00 AF 4B 1A 20 1C 20 38 20 35 20 28 20 EB 00 00`

Character indices: 0-9 digits, 10-35 `A-Z`, 36-61 `a-z`, `?` = 235, `!` = 236, `:` = 233, `.` = 231, `-` = 252, `#` = 260.

**Console:** pre-encode id 5. Implement 1, 2 and 6 with an ASCII-only `CHAR_MAP` lookup if chat is supported; non-ASCII name characters are dropped, as Dolphin drops unmapped ones.

---

### 0xE3 GET_RANK, 0xE4 FETCH_RANK, 0xE5 GET_RANK_VISIBILITY (rank display; optional)

These are issued by the `.dat` UI, not by `A/`.

- `0xE3` answer, **16 bytes**, the Dolphin layout (`handleGetRank`, :3331-3353):

  | Offset | Field |
  |---|---|
  | 0 | `u8 visibility` (bit 0 local, bit 1 opponent) |
  | 1 | `u8 fetch_status` (0 Fetching, 1 Fetched, 2 Error; `R/user/src/rank_fetcher/mod.rs:18-23`) |
  | 2 | `u8 rank` (0 = Unranked … 19 = Grandmaster, `SlippiMatchmaking.h:56-78`) |
  | 3 | `f32 rating_ordinal` |
  | 7 | `u32 rating_update_count` |
  | 11 | `f32 rating_change` |
  | 15 | `u8 rank_change` |

  The ASM's `RIRB` (`Online.s:353-362`, 22 bytes, with placings and a u32 rank change) **does not match** and is unused by the ASM. Trust Dolphin's layout.
- `0xE4`: no answer; Rust fetches the match result for `recent_mm_result.id`.
- `0xE5`: in the size table (0) but **has no `case`**, so it falls to `default:` and is written into the replay or spectator stream (:3569-3575). Nothing is prepared, so a following read returns stale data.

**Console:** `0xE3` → `00 01 00 00000000 00000000 00000000 00` (hidden, fetched, Unranked); ignore `0xE4`/`0xE5`. If anything reads after `0xE5`, answer zeros.

## 4. Encodings

### 4.1 Game strings: `ConvertStringForGame(s, N)`

`D/../Common/StringUtil.cpp` (read via `git show HEAD:Source/Core/Common/StringUtil.cpp`), around :452-474:

1. Truncate to N **code points** (UTF-32).
2. Convert UTF-8 to Shift-JIS (CP932). ASCII stays single-byte.
3. `ConvertNarrowSpecialSHIFTJIS`: replace each single-byte ASCII punctuation character with its 2-byte full-width Shift-JIS form, skipping existing 2-byte characters (lead byte ≥ 0x80). For example `!` → 81 49, `#` → 81 94, `.` → 81 44, `-` → 81 7C, `'` → 81 66, `?` → 81 48, `:` → 81 46, `/` → 81 5E. The full 32-entry table is in `ConvertNarrowSpecialSHIFTJIS`.
4. Resize to `2N + 1` bytes, zero-padded; this also truncates.

The resulting fields:

| Use | N | Bytes |
|---|---|---|
| Names | 15 | 31 |
| Chat messages | 25 | 51 |
| Error text | 120 | 241 |

Letters, digits and space stay narrow ASCII. A kernel without a full CP932 table can map ASCII, apply the punctuation table, and substitute or drop other code points.

### 4.2 Connect codes: `ConvertConnectCodeForGame`

`EXI_DeviceSlippi.cpp:114-125`: replace `#` with 0x81 0x94 and resize to 10 (`CONNECT_CODE_LENGTH 8` + 2). For example `ABCD#123` → `41 42 43 44 81 94 31 32 33 00`. Codes come from `user.json` (`connectCode`) and the matchmaking `players[].connectCode`, both ASCII.

### 4.3 Slippi Dolphin's netplay calls and the existing kernel API

This is for wiring; the wire formats already match Ishiiruka 3.6.4 (`N/slippi_online/so_netplay.h:4-14`; `I/Slippi/SlippiNetplay.cpp:200-364,552-558,1284-1285` has the same fields).

| Dolphin call | Kernel |
|---|---|
| `GetSlippiConnectStatus` | `so_np_status()` |
| `IsDecider` | `so_np_is_decider()` |
| `StartSlippiGame` | `so_np_start_game()` |
| `SendSlippiPad(pad / null)` | `so_np_send_pad(pad / NULL)` |
| `GetSlippiRemotePad` | `so_np_remote_pad` / `so_np_remote_head_frame` / `so_np_remote_checksum` |
| `DropOldRemoteInputs` | queue trim in `so_pads` |
| `SetMatchSelections` | `so_np_set_selections` |
| `GetMatchInfo` | `so_np_local_selections` / `so_np_remote_selections` |
| Chat | `so_np_send_chat` / `so_np_take_chat` |
| `CalcTimeOffsetUs` | `so_np_time_offset_us` |
| `ForceDisconnect` | `so_np_force_disconnect` |
| Matchmaking state and result | `so_client_*` / `so_mm_result()` |

## 5. What Direct 1v1 needs

### Essential: Direct cannot start or play without these

| Cmd | Why |
|---|---|
| B9 | Must report `app_state = 1`, or the online modes stay locked |
| BA | Sent on every menu load and on cancel; resets the state |
| B5 | Lock-in, sent before B4 and on every later game |
| B4 | Starts the Direct search; the state must move to 1 at once |
| B3 | Polled every CSS frame. Needs states 0-5, ready flags, ports (decider-based), names, codes, error text, the persistent game info block, RNG offset, delay and match id |
| B0 | Per-frame lockstep inputs: SKIP until the remote pad for `frame` is present, the full 297-byte RXB, pads forwarded with checksums |
| BC | New RNG seed after each game |
| D1 / D2 | Slippi's menu and m-ex files (MxDb, MnMaAll, MnSlMap, SdMenu, SlippiCSS, ...), unless M4 bakes them into the ISO instead. D1 must answer "0" quickly for every other file. |
| E1 / E2 | At least id 5 (logout dialog); ids 1-4 and 6 when chat is used |
| C3 | Cheap (defaults); the CSS `.dat` reads chat strings |
| B1 / B2 | Must be accepted and ignored (never needed in lockstep) |

### Optional, with minimal correct answers

| Cmd | Minimal answer |
|---|---|
| BB | Ignore, or forward with the one-shot chat fields |
| BE | Answer "not found": `[0, echo, len, index]` |
| BD | Ignore |
| B6 | Re-read `user.json` |
| B7 | Clear the in-memory login, or ignore; never delete the file |
| B8 | Ignore |
| D0 | Ignore, or send to the PC log sink |
| D3 / D4 | Only with the bootloader route |
| D5 | `[1, 0]` |
| D6 / D7 / D8 | Ignore, and drop the music-mute codes from the codeset |
| BF / C0 | Ranked only: ignore |
| C1 | Six zero bytes |
| C2 / C4 | Ranked only: ignore |
| E3 | `00 01 00` + 13 zero bytes |
| E4 / E5 | Ignore |

## 6. Surprises and risks for a lockstep console

1. **Nintendont's EXI DMA length is 16 bits**, and it is ORed straight into the mailbox word (`N/asm/EXIDMA.S:31-38`, `N/EXI.c:797`).
   - Slippi's `0xD2` file reads are often ≥ 64 KiB: MxDb.dat 445 KB, the patched MnMaAll/MnSlMap, and GrPs*.dat.
   - The bootloader GCT (`0xD4`, about 58 KB) is near the limit.
   - These need a changed `EXIDMA` stub (pass the length in `EXI_CMD_2` or similar), PPC-side chunking, or putting the files in a patched ISO's FST instead of serving them.
2. **`0xD1` runs for every file load in every scene.** It must answer in microseconds; it is a table lookup.
3. **A lockstep device answers SKIP on frame 1, often repeatedly; Dolphin never does.** The game-start reset (Dolphin's `frame == 1` block) must run once per game, or repeated polls reset the pad queue and selections.
4. **Never answer CONTINUE (1) or ADVANCE (4) while any `RXB_OPNT_FRAME_NUMS[i] < frame`.**
   - Otherwise the ASM predicts and asks for savestates (`0xB1`) and possibly rollbacks (`0xB2`), which the Wii cannot do (Dolphin states are about 8 MB).
   - Always return the full 297-byte RXB, even on SKIP: the ASM consumes it when SKIP arrives after game over (`TriggerSendInput.asm:268-274`).
5. **The game info block in `0xB3` is stateful**: static in Dolphin, patched only while both are ready, and read after the game (`main.asm:683`). It is not a pure function of the current state.
6. **1v1 ports come from the decider flag** (`IsDecider() ? 0 : 1`), while names, codes and UIDs are indexed by matchmaking port, and the netplay player index is the matchmaking port.
   - This is consistent only if the server makes the host port 1. The kernel should copy Dolphin exactly, not "fix" it.
7. **`0xC2` is written with DMA length 1 but carries a payload byte.** The device must read the payload by the table size from RAM, not by the DMA length.
8. **Desync is fatal: the hard-desync check ends the game.**
   - The ASM compares the low 16 bits (integer float sum of positions) of each peer's checksum, ±1 allowed (`StartEngineLoop.asm:540-572`).
   - The Wii must send `finalized_frame`/`checksum` from the TXB with every pad and return the remote's in the RXB.
   - Any hardware-versus-Dolphin floating-point difference will surface as "desync" and end the game. That is a correctness risk to measure in M5, not something the device can hide.
9. **Pokémon Stadium transformations** are loaded mid-match through `0xD1`/`0xD2` (`StadiumFileLoad.asm`).
   - With "no replacement" the fallback is a synchronous disc read: deterministic, but a visible stall on the Wii.
   - Serving them needs reads over 64 KiB (surprise 1).
10. **Online commands must be kept out of the replay ring.** Nintendont currently forwards every DMA write to `SlippiMemoryWrite` (`N/EXI.c:805-809,841-845`).
    - Dolphin also leaks `0xE5` into replays via `default:` (`EXI_DeviceSlippi.cpp:3569-3575`); the kernel need not copy that.
11. **Red herrings in the sources:**
    - `FRAME_RESP_*` in `EXI_DeviceSlippi.h:115-121` is not the `0xB0` result encoding;
    - the ASM's `RIRB` (22 bytes) does not match Dolphin's 16-byte `0xE3` answer;
    - the ASM names `0xBF` "NameEntryAutoComplete";
    - `MSRB_USER_RANK`/`MSRB_OPP_RANK` actually carry port-0 and port-1 ranks.
12. **`0xBB`, `0xBF`, `0xC0`, `0xC1`, `0xC3`, `0xE3` and `0xE4` come from Slippi's compiled `.dat` UI code** (`slpCSS.dat`, `GameSetup.dat`), not from `A/`.
    - Their request lengths and read sizes are inferred from Dolphin's structs.
    - Their use on the Direct CSS (C3, BB, E1/E2 chat) should be confirmed on hardware by logging every EXI command byte the CSS issues.
13. **Dolphin's local name in `0xB3` is empty until connected** (:2282-2312). The CSS shows the user from `0xB9` instead. Copy this so the UI matches.
14. **Time sync is one-sided.** The PC slows and stalls for the Wii; the Wii only stalls (SKIP).
    - If the Wii falls behind, the PC compensates by rolling back; it never stalls the Wii.
    - Implementing Dolphin's ADVANCE rule on the Wii would need two engine frames in one VI. Leave it for later; it is not needed for correctness.

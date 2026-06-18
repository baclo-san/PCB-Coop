# TH07 (PCB) Co-op Netplay — Handoff for Next Session

## 0. Read this first

**Goal:** Add co-op (2-player, possibly 3-player) netplay to Touhou 7 *Perfect Cherry Blossom* (`th07.exe`),
modeled on the EoSD co-op mod **RUEEE/th06_multi_net** (https://github.com/RUEEE/th06_multi_net),
which is built on the **GensokyoClub/happyhavoc** th06 decompilation.

**Constraints / framing:**
- Solo, **functional** RE (behavior-correct, not bit-matching).
- **No PCB decomp exists** (confirmed via th06_multi_net / ZUNcode Discords — they only touched LZSS).
  The th06 decomp is the **phrasebook / ground truth** — same author & era, structures recur.
- Reverse-engineering target is a **Ghidra dump**: `C:\Users\rndmdck\Desktop\th07.exe.c` (~83k lines).
  This is the user's IDE-open file. All `FUN_xxxx` / `DAT_xxxx` names below are Ghidra labels in THAT db.

### ✅ Binary PINNED (2026-06-10) — addresses are build-specific
All addresses in this document are anchored to this exact binary:
- **File:** `D:\Touhou 7 - Perfect Cherry Blossom\th07.exe`
- **Version:** `Perfect Cherry Blossom. ver 1.00b` (embedded version string)
- **SHA256:** `35467EAF8DC7FC85F024F16FB2037255F151CEFDA33CF4867BC9122AAA2E80CA`
- **Size:** 650,752 bytes
- Verified to match the Ghidra db by byte signature at the documented addresses: Rng_Core @0x431870,
  Rng_NextFloat @0x431900 (`FDIV [0x498b28]`), Input_Poll @0x430b50 (tests `g_KeyboardDevice @0x575960`).
  The original Desktop copy fed to Ghidra was since deleted; this byte-level match is the anchor.
- PE: ImageBase 0x400000; .text VA 0x401000 / raw 0x400 ⇒ **file_offset = VA − 0x400C00**.
- PCB has multiple releases (1.00, 1.00b, 1.00c…); offsets differ between them. Patch ONLY this hash.
- Game folder also has vpatch + thcrap; the injector launches `th07.exe` directly, bypassing both
  (keep it that way for determinism runs).

---

## 1. Confirmed th07 addresses (this build)

### RNG  ✅ fully resolved
| Addr | Name | Notes |
|---|---|---|
| `0x00431870` | `Rng_Core` | `__fastcall`, ECX=&state. `s=(seed^0x9630)+0x9aad; seed=(s>>14)+s*4; counter++` |
| `0x004318d0` | `Rng_Next32` | two `Rng_Core` → `hi<<16 \| lo`; relays ECX |
| `0x00431900` | `Rng_NextFloat` | `Rng_Next32 / 0x498b28` → float [0,1); relays ECX |
| **`0x0049fe20`** | **`g_RngState.seed`** (u16) | **The RNG seed.** |
| **`0x0049fe24`** | **`g_RngState.call_counter`** (u32) | **Desync oracle.** |

- All RNG fns are `__fastcall` with ECX = `&g_RngState` threaded like a C++ `this`. The decompiler hid
  the `MOV ECX, offset` at call sites (mistyped fns as `(void)`). Confirmed via listing of caller
  `FUN_004012b0`: `MOV ECX, 0x0049fe20` (opcode B9 = imm32 = the address).
- **ZUN's own proof:** line ~18492 logs `printf("random seed %d %d\r\n", DAT_0049fe20, DAT_0049fe24)` —
  he literally labels these two adjacent globals as the RNG seed+counter.
- Seeded from `(u16)timeGetTime()` at WinMain init (line ~23328), right before `DInput_Init`.
- **Replay system stores & restores the seed** (line ~27582 writes seed into replay obj +0xd4 and resets
  counter=0 at game start; line ~27909 restores seed from replay file +0x20 on playback). This is the
  existing determinism mechanism — netplay should mirror it (seed = a network-supplied value).
- Note: the mod compares **seed only** per frame for sync. Counter is redundant for detection but
  invaluable for *pinpointing* where divergence happened (use in the harness).

### Input  ✅ fully resolved
| Addr | Name | Notes |
|---|---|---|
| `0x00430b50` | `Input_Poll` | GetDeviceState(kbd, vtable+0x24, 0x100 buf) → DIK scan loop → bitmask. (Ghidra mistyped return as void; it returns a u16.) |
| `0x004303f0` | `Input_AddJoyBits` | ORs joystick axis/button bits onto same u16, returns combined |
| `0x00430370` | `Joy_SetBit` | conditional single-bit OR helper |
| `0x00575960` | `g_KeyboardDevice` | IDirectInputDevice8A* |
| `0x00575964` | `g_JoystickDevice` | IDirectInputDevice8A* |
| **`0x004b9e4c`** | **`g_InputMenu`** (u16) | RAW poll result. Used by MENUS. `= Input_Poll()` each frame. |
| `0x004b9e54` | `g_InputMenuPrev` (u16) | previous frame (menu edge-detect) |
| `0x004b9e5c`,`0x004b9e60` | menu autofire flag / counter | |
| **`0x004b9e50`** | **`g_InputGameplay`** (u16) | **THE GAMEPLAY INPUT.** Muxed through replay record/playback. **This is the netcode injection target.** |
| `0x004b9e58` | `g_InputGameplayPrev` (u16) | gameplay edge-detect |

**TWO input globals — key discovery:**
- `g_InputMenu` (0x004b9e4c) = raw keyboard poll. Menus read this; they test high bits
  (0x200=Q, 0x800=Home, 0x1000=Enter, 0x4000=R, etc.).
- `g_InputGameplay` (0x004b9e50) = gameplay input, produced by the **replay record gate** in
  `FUN_00442cd0`: `g_InputGameplay = g_InputMenu` (when recording flag `DAT_0062f648>>2 & 1` set, which
  is ~always during play). Playback (`FUN_00442ee0`) instead feeds `g_InputGameplay` from the replay file.
- **Player gameplay code reads ONLY `g_InputGameplay`, and ONLY the low 9 bits:**
  `0x01`=shot, `0x02`=bomb, `0x04`=focus, `0x10/20/40/80`=up/down/left/right, `0x100`=skip(dialog FF).
  Confirmed read sites: 16266/16340/16492 (skip), 26228–26253 (dirs), 26256/26589 (focus),
  26584 (shot), 26658/26670 (bomb).
- **Bits 0x200–0x8000 are NEVER tested during gameplay.** ⇒ the high-bit collision risk is NOT real
  for gameplay. th06's "P2 in high bits" packing ports directly.

### 16-bit input bitmask layout (decoded from Input_Poll listing)
| Bit | Key | Function | th06 TouhouButton |
|---|---|---|---|
| 0x0001 | Z | shot | SHOOT (1<<0) |
| 0x0002 | X | bomb | BOMB (1<<1) |
| 0x0004 | Shift | focus | FOCUS (1<<2) |
| 0x0008 | Esc | menu | MENU (1<<3) |
| 0x0010 | Up/Num8 | up | UP (1<<4) |
| 0x0020 | Down/Num2 | down | DOWN (1<<5) |
| 0x0040 | Left/Num4 | left | LEFT (1<<6) |
| 0x0080 | Right/Num6 | right | RIGHT (1<<7) |
| 0x0100 | Ctrl | skip | SKIP (1<<8) |
| 0x0200 | Q | (menu only) | — free in th06 → P2 SHOOT2 |
| 0x0400 | S | (menu only) | — |
| 0x0800 | Home | snapshot/menu | — |
| 0x1000 | Enter | OK/select | — |
| 0x2000 | D | (menu only) | — |
| 0x4000 | R | (menu only) | — |
Diagonals are literal OR of two dir bits (Num7=0x50, Num9=0x90, Num1=0x60, Num3=0xa0).

### Frame governor & loop
| Addr | Name | Notes |
|---|---|---|
| `0x004346e0` | `FrameGovernor` | 60Hz loop. Calls GameUpdate once **per logic frame**; frameskip skips only render. 60Hz spin via QPC (freq `0x00575c34`, last-ts `0x0135e208/20c`) or timeGetTime (`0x0135e200/204`). |
| `0x0042fd60` | `GameUpdate` | task-chain tick (ECX = `0x00626218`). Input poll happens INSIDE this. |
| `0x0042fe20` | `GameDraw` | draw chain |
| `0x004345c0` | per-displayed-frame present | |
| `0x0135e1f8` | displayed-frame counter | **Do NOT use as netcode frame index** (under-counts on frameskip) |
| `0x004b9e44` | main game/supervisor object ptr | |
| `0x00626218` | task-chain manager | |
| `0x00626278` | live in-game score/stat object ptr | (cherry/point/graze live here) |
| `0x00437d80`,`0x00437dfc` | scene-tick callers of Input_Poll | |

---

## 2. The reference mod (th06_multi_net) — how it works

Full th06 decomp **fork**. C++ (71%), Python build (26%). License CC0. Default branch = **master**.

**Netcode files:** `src/Connection.cpp/.hpp` (UDP/Winsock), `src/Controller.cpp/.hpp`
(input + delay buffer + merge + sync), `src/ConnectionUI.cpp/.hpp` (host/join UI).
**Hook points live in:** `src/Supervisor.cpp` (the per-frame hook), `src/Player.cpp` (P1/P2 split,
resurrection), `src/GameManager.cpp` (extends/lives), `src/Rng.cpp`, `src/ReplayManager.cpp`.

### Design: delay-based lockstep, ONE u16 carries both players
- P1 = low 9 bits (native TouhouButton layout). **P2 = high 7 bits** (SHOOT2=1<<9 … RIGHT2=1<<15).
- `Player.cpp` has two movement blocks: `IS_PRESSED(TH_BUTTON_UP)` (P1) vs `IS_PRESSED(TH_BUTTON_UP2)`
  (P2), both off the same word → drive `g_Player` vs `g_Player2`.

### Packet (`CtrlPack`, in Connection.hpp)
```
int frame; Control ctrl_type;
union { Bits<16> keys[15]; struct{int delay,ver;} init_setting; struct{int frame_to_re_sync;} resync; };
InGameCtrlType igc_type[15];      // in-game ctrl: quit/restart/inf-life/add-delay/etc
unsigned short rng_seed[15];       // PER-FRAME RNG seed (the sync check)
```
`KeyPackFrameNum = 15` frames batched per packet. `Bits<32>` is also defined (relevant for 3P).
Control enum: `Ctrl_No_Ctrl, Ctrl_Start_Game, Ctrl_Key, Ctrl_Set_InitSetting, Ctrl_Try_Resync`.
Topology: `Host` / `Guest` classes, **point-to-point UDP** (2 players only).

### Per-frame flow (`Controller::GetInput_Net(frame, is_in_UI, cur_ctrl)`)
1. `btn = GetInput()` — read local hardware → u16 (P1 low bits; P2 high bits only in local-2P mode).
2. `g_ctrl_bits_self[frame] = btn`; **`g_ctrl_rng_self[frame] = g_Rng.seed`** (snapshot seed).
3. `SendKeys(frame)` / `RcvPacks()` — UDP exchange of last 15 frames.
4. `GetKeys(frame)` — read `self[frame-g_delay]` & `rcved[frame-g_delay]`; **busy-wait ≤5s** for peer's
   frame (the lockstep stall); set `g_is_sync = (rng_rcved == rng_self)`; **merge** so BOTH machines
   produce an identical word: P1=host's input (low bits), P2=guest's input (mapped to *2 high bits).
5. Old frames (>80 back) erased each tick. Maps are `std::map<int,...>` keyed by frame — **NOT a ring buffer**.

State globals (Controller.cpp): `g_delay` (default 1; M/N keys adjust), `g_is_sync`, `g_is_connected`,
`g_is_host`, `g_is_single_mode`, `g_resync_trigger`, `g_resync_stage_frame`,
`g_ctrl_bits_self/_rcved`, `g_ctrl_rng_self/_rcved`, `g_ctrl_self/_rcved`.

### The hook (Supervisor::OnUpdate — per logic frame)
```cpp
int frame_a   = s->calcCount;                              // netcode frame index = LOGIC-frame counter
bool is_in_UI = (curState != GAMEMANAGER) || g_GameManager.isInGameMenu;
g_LastFrameInput = g_CurFrameInput;
g_CurFrameInput  = Controller::GetInput_Net(frame_a, is_in_UI, cur_ctrl);  // ← injection
```
`g_CurFrameInput` = the single global the whole game reads via `IS_PRESSED`/`WAS_PRESSED`.
On new game it detects `calcCount` reset and clears the rcved maps.

### Desync recovery
Host picks `calcCount + g_delay*2+2`, sends `Ctrl_Try_Resync`; at that frame both set
`g_Rng.seed = 0` and clear rcved buffers, then continue.

### Coop gameplay changes the mod makes (for reference)
- **Lives:** separate `livesRemaining` + `livesRemaining2`; extends grant +1 to both (capped MAX_LIVES);
  new `g_ExtraLivesScores[]` threshold table.
- **Resurrection:** in Player update — hold focus + release shot near dead partner (PLAYER_STATE_SPIRIT)
  for 90 frames → spend one of your own lives to revive them (`lifegiveTime`).
- **Player2 entity:** `DIFFABLE_STATIC(Player, g_Player2)` + duplicated update/draw chain calls.
- Boss HP scaling: mentioned by user; not located in files reviewed (likely ECL/data or elsewhere).

---

## 3. Port plan: th06 mod → th07

### The netcode is engine-agnostic — only 4 seams touch the game
| Seam | th06 (decomp symbol) | th07 (this build) |
|---|---|---|
| Input read | `Controller::GetInput()` | wrap `Input_Poll` @0x00430b50 (or read `g_InputMenu` 0x004b9e4c) |
| Input write/hook | `g_CurFrameInput` in `Supervisor::OnUpdate` | **`g_InputGameplay` 0x004b9e50**; hook the `g_InputGameplay = g_InputMenu` assignment in **`FUN_00442cd0`** (replay record fn) |
| RNG seed snapshot | `g_Rng.seed` | `*(u16*)0x0049fe20` |
| Frame index | `g_Supervisor.calcCount` | replay mgr's frame counter (`*param_1` in FUN_00442cd0) OR maintain your own in the detour. **NOT** 0x0135e1f8. |

`g_LastFrameInput` → `g_InputGameplayPrev` 0x004b9e58.
`Connection.cpp` + most of `Controller.cpp` **lift nearly verbatim**.

### TWO divergences from the th06 approach
**(A) Delivery mechanism.** th06 mod *recompiles from decomp* (symbol-level). **You have no PCB decomp**,
so you port the C++ logic into an **injected DLL that detours th07.exe by address** (MinHook/Detours):
- detour the `g_InputGameplay = …` write in FUN_00442cd0 → overwrite with merged `GetInput_Net()` word
- read 0x0049fe20 for the seed snapshot
- maintain your own frame counter in the detour
The logic ports; the *attachment* is runtime hooking, not a rebuild.

**(B) High-bit collision — RESOLVED as non-issue for gameplay.** th07 uses bits 0x200–0x4000 for
menu/system keys, BUT gameplay reads only the low 9 bits of `g_InputGameplay`. So P2-in-high-bits works
in-game; in menus, do as th06 does (`is_in_UI` ⇒ don't map P2). No separate P2 global needed for 2P.

---

## 3b. NETCODE CORE STATUS (2026-06-10) — ported + self-tested ✅

The engine-agnostic netcode is lifted into `src/netplay/` and **builds + passes a self-test** (16/16):

- `Connection.hpp/.cpp` — UDP transport + wire structs (`Pack`/`CtrlPack`/`Bits<16>`). Lifted nearly
  verbatim from th06_multi_net. `CtrlPack.init_setting` extended with `unsigned short rng_seed_init`
  (host-supplied start seed — the seam for seed-sync, see fork A below).
- `netcode.hpp` — public interface. The th06 engine deps are replaced by **two host callbacks**:
  `readLocalInput()` (th07 → read `g_InputMenu` 0x004b9e4c) and `readRngSeed()` (th07 → `*(u16*)0x0049fe20`).
- `netcode.cpp` — the lockstep core: per-frame `Netcode_GetInput_Net(frame, is_in_UI, &cur_ctrl)`,
  delay-buffer `std::map`s, `SendKeys`/`RcvPacks`, 5s lockstep stall, seed-sync check. The merge that
  makes both machines agree is factored into a **pure `MergeKeys()`** (verified: P1=low bits,
  P2=high bits, host==guest for all sampled combos).
- `tests/netloop_test.cpp` — real UDP loopback round-trip + exhaustive merge agreement. Built by
  `build.ps1` → `build\netloop_test.exe`. (NOTE: ConnectionBase's static Winsock refcount means ONE
  peer per process; the test holds its own WSAStartup ref to coexist host+guest in-process.)

**Frame index = `*param_1` of `FUN_00442cd0`** (the replay-mgr frame counter, incremented once per logic
frame). **Game-start init = `FUN_00442c60`**: saves seed `+0xd4`, resets RNG counter to 0.

### What is NOT ported (lives in th06 files we don't have: Rng.cpp / Supervisor.cpp / ReplayManager.cpp)
- The seed handshake (how both machines load the SAME seed at game start) — **fork A**.
- The Supervisor per-frame hook that locksteps MENUS via `calcCount` — th07 needs a different shape
  because it has **two** input globals, not one — **fork A**.
- The ConnectionUI host/join/start-game dialog (`ConnectionUI.hpp/.cpp` exist in `reference/` but aren't
  ported yet).

### TWO OPEN FORKS before integration can produce playable co-op

**Fork A — menu lockstep + seed sync (the two-input-globals problem).**
th06 has ONE input word (`g_CurFrameInput`) for menus AND gameplay, so its Supervisor hook locksteps
everything off `calcCount`. th07 splits this: menus read `g_InputMenu` (0x004b9e4c), gameplay reads
`g_InputGameplay` (0x004b9e50), and `FUN_00442cd0` (which does `g_InputGameplay = g_InputMenu`) **only
runs in-stage**. So hooking `FUN_00442cd0` alone locksteps gameplay but NOT the pre-game menus
(char/difficulty select) — and if the two players reach the stage with different menu paths or different
seeds, they desync instantly. Options:
  - **A1 (clean):** maintain our own per-logic-frame counter in a hook that runs ALWAYS (e.g. GameUpdate
    `FUN_0042fd60`); overwrite `g_InputMenu` with the merged word right after each poll (locksteps menus,
    `is_in_UI=true` → `self|rcv`); `FUN_00442cd0` then copies merged → `g_InputGameplay` for free
    (`is_in_UI=false` switch when in-stage). One hook owns both globals.
  - **A2 (minimal):** lockstep ONLY gameplay (hook `FUN_00442cd0`); require players to manually
    coordinate menu/char selection; host forces the seed at stage start via `rng_seed_init` (write
    `*(u16*)0x0049fe20` in the `FUN_00442c60` detour). Simpler, but fragile menu UX.
  - Seed sync (both options): host picks the seed, sends it in `Ctrl_Set_InitSetting.rng_seed_init`;
    both write it to 0x0049fe20 at game start (mirror the existing replay seed-restore at line 27909).

**Fork B — the P2 entity graft (Tier-3, the big gameplay effort).**
Feeding P2 input in high bits does NOTHING until a second `Player` entity reads those bits. PCB is
single-player; grafting `g_Player2` (RE the Player struct, register in update+draw chains, give it
collision/shot/bomb, feed high-bit input) is the largest gameplay-side task and gates lives/bombs/
resurrection. Independent of the network (pure local RE) — can proceed in parallel with Fork A.

---

## 4. Coop gameplay scope (user decisions, 2026-06-10)

- **Extra extends: NOT wanted** (PCB already has plenty).
- **Separate cherry counter per player: wanted.**
- **3rd player: maybe** (PCB has 3 chars: Reimu/Marisa/Sakuya → one each).

### Cherry / border system  (PCB-specific; also the #1 determinism risk)
Display fn `FUN_00427f22` (~line 15528); end-of-stage tally ~16392. Score sub-struct via `gameManager+8`:
| Field | Meaning |
|---|---|
| `+0x20a28` | Cherry (cumulative, ×10) |
| `+0x20a24` | Point items (×50000) |
| `+0x20a2c` | Graze (×500) |
| `+0x209f0` | Border state machine: 1=FullPower, 2=SupernaturalBorder, 3=CherryPointMax, 4=BorderBonus |
| `+0x209ec` | Border Bonus value |
| `+0x209fc` | Border banner anim frame counter (0→0xb3); drives float Y at `+0x209e0` |
| `+0x209b8` | Total score |

**⚠️ Cherry is RNG-COUPLED — read carefully:**
Live cherry globals: `DAT_0062f888` (CherryMax), `DAT_0062f88c`, live cherry at `DAT_00626278+0x88`.
Enemy item-drop code (~lines 10445/10495) does:
```c
cherryRatio = ((DAT_0062f88c - *(int*)(DAT_00626278+0x88)) * 100) / DAT_0062f888;
if (Rng_Next32() % 100 <= cherryRatio) { /* spawn item */ }
```
So **cherry fill gates shared-RNG item drops** ⇒ cherry is INSIDE the determinism surface. If two
machines hold different cherry, the item-drop RNG branches differently → desync.
- **Shared cherry pool** = automatically determinism-safe.
- **Per-player cherry pool** = new RNG-feeding state; must be byte-identical on both machines, AND you
  must decide which player's ratio drives the (shared) item-drop roll. The counter @0x0049fe24 harness
  is exactly how to validate this.
- **OPEN DESIGN DECISION (user's):** per-player borders vs shared? Per-player borders each cancelling
  bullets could trivialize the game. Likely sweet spot: "separate counter, shared border activation."
  Decide when the mechanic is visible/playable.

### Difficulty tiers for coop gameplay
- **Tier 1 (easy, data/fn patches):** boss-HP scaling (×2 for double DPS) — patch enemy HP-init or ECL.
- **Tier 2 (mechanical, parallel state):** per-player cherry/lives/bombs — duplicate fields (DLL-owned
  for P2/P3), detour every reader/writer.
- **Tier 3 (the hard core):** the **second player ENTITY** — PCB is single-player; you must graft a whole
  `g_Player2` (RE the Player struct, register it in update+draw chains, feed it high-bit input, give it
  collision/shot/bomb). This is the largest gameplay-side effort and gates resurrection/lives/bombs.

### 3-player verdict (do as v2, not day one)
1. **Input transport:** the 16-bit word is FULL (P1 low9 + P2 high7). 3rd player needs 7 more bits →
   won't fit native word. Fix: widen netcode key to `Bits<32>` (already in Connection.hpp) or 3× u16;
   **P2/P3 read DLL-owned input vars** (they're custom entities anyway), only P1 stays native.
2. **Entities:** graft TWO extra players (2× the Tier-3 work).
3. **Network topology — the real new problem:** th06_multi_net is strictly 2P point-to-point. 3P needs a
   **host-relay (star)**: guests→host, host bundles all 3 & broadcasts. Rework Connection.cpp Host/Guest
   from 1:1 to 1:N + 3-way sync/resync.
Roughly 2× the netcode+entity effort. **Ship 2P first** (near-direct port), add 3P after entity-grafting
and determinism are proven.

---

## 4b. BUILD & HARNESS STATUS (2026-06-10) — pipeline proven end-to-end

**Toolchain installed:** llvm-mingw (MSVCRT) via winget — see memory `th07-toolchain`. Builds 32-bit PE
with no Windows SDK. Compiler: `i686-w64-mingw32-gcc` (clang 22) under `%LOCALAPPDATA%\Microsoft\WinGet\Packages\`.

**Built artifacts** (in `build\`, both confirmed PE machine 0x14c / x86):
- `th07_harness.dll` (deps: KERNEL32/USER32/msvcrt only — fully portable)
- `injector.exe` — CreateProcess(SUSPENDED) → VirtualAllocEx/WriteProcessMemory → CreateRemoteThread(LoadLibraryA) → ResumeThread
- `harness.ini` (mode=record|replay, seed=0x1234), `diff_sync.ps1` (record-vs-replay seed diff)

**Build:** run `build.ps1` (auto-discovers compiler). **Run:** `injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe"`.

**Smoke test (record mode) PASSED:** injector loaded the DLL (hmodule 0x73b50000), the `Input_Poll`
@0x00430b50 hook fired every frame, the forced seed `4660`=0x1234 was applied on poll 0 and held stable
across ~698 polls of the title screen (counter 0 — PCB consumes no RNG idling on title), then began
advancing when a demo replay loaded. `input_log.bin` + `sync_record.csv` written correctly.

**Two empirical findings that shape the determinism runs:**
1. `Input_Poll` fires ~1.7×/logic-frame (multiple call sites). The harness's "poll index" is therefore
   NOT a logic-frame index — fine for a row id and for record↔replay alignment (both replay the identical
   poll sequence), but do not conflate it with the netcode frame index (use the FUN_00442cd0 seam for that).
2. The title screen restores seeds from demo-replay files on idle. To get a MEANINGFUL determinism test,
   the recorded input must reach actual gameplay (don't idle into a demo).

**HOW TO RUN THE ACTUAL DETERMINISM TEST (next action):**
- Set `harness.ini` mode=record. Run injector, **play from title into a border-heavy stage**, quit.
  → produces `input_log.bin` (your inputs) + `sync_record.csv`.
- Set mode=replay. Run injector again (don't touch the keyboard). → produces `sync_replay_<pid>.csv`.
- `powershell -File build\diff_sync.ps1` → reports DETERMINISTIC or the first divergent poll.
- Determinism holds ⇒ the netcode premise is proven. Diverges ⇒ first poll+counter pinpoints the sim call.
- *Optional refinement:* move the per-frame seed-force+log off `Input_Poll` onto a once-per-logic-frame
  seam (GameUpdate @0x0042fd60, or the FUN_00442cd0 g_InputGameplay write) for 1 clean row per frame.

## 5. Recommended next steps (in order)

1. ~~**Pin SHA256 of th07.exe.**~~ ✅ DONE — `35467EAF…E80CA`, ver 1.00b (§ top).
2. ~~**Build the desync harness.**~~ ✅ DONE — built + smoke-tested (§4b). Next: run the real
   record→replay→diff on a border-heavy stage (procedure in §4b).
   - Inject a DLL into two th07.exe instances (MinHook or MS Detours).
   - Force `*(u16*)0x0049fe20` to the same fixed seed on both.
   - Replay an identical scripted input log into `g_InputGameplay` (0x004b9e50) by detouring the
     `g_InputGameplay = g_InputMenu` write in FUN_00442cd0.
   - Log `*(u32*)0x0049fe24` (call counter) every frame to a file; diff the two logs.
   - Run a border-heavy stage. If counters stay locked → determinism premise proven; if they diverge,
     the first divergent frame + counter pinpoints the offending sim call.
3. **Lift Connection.cpp + Controller.cpp** into the DLL; wire the 4 seams to the addresses above.
   Implement 2P first: P1 native low bits, P2 high bits, single u16 into 0x004b9e50.
4. **Graft g_Player2 entity** (Tier-3). Then layer per-player cherry/lives + resurrection on top.
5. (v2) 3rd player: widen transport, second entity, host-relay topology.

## 5b. Session progress — 2026-06-11 (night routine)

**Headline: there is now a full build+test loop that runs ON LINUX (no Windows
needed), and the netcode lockstep core is verified end-to-end.**

- **`build.sh`** — Linux/mingw counterpart of `build.ps1`. Builds the harness DLL,
  co-op DLL, the new netcode-integration DLL, the injector, and both netcode tests
  with `i686-w64-mingw32-g++`. `./build.sh --test` also RUNS the tests under wine.
  Toolchain to reproduce: `apt-get install gcc-mingw-w64-i686 g++-mingw-w64-i686`,
  and to run: `dpkg --add-architecture i386 && apt-get install wine wine32:i386`
  (wine binary ends up at `/usr/lib/wine/wine`). Confirmed working.
- **Lockstep integration test** (`tests/netsim.cpp` + `tests/run_netsim.sh`) — the
  in-process `netloop_test` only covered transport + `MergeKeys`. The new test runs
  a REAL host+guest in two processes exchanging UDP frames through
  `Netcode_GetInput_Net()`. **All checks pass:** both peers compute identical merged
  words over 200 frames, the seed-sync flag holds when seeds agree, and the desync
  oracle flips correctly on an induced seed divergence (while inputs still merge
  consistently). This validates the delay buffer + sync detector that were never
  exercised before. *Bug found & fixed in the test harness only* (mingw/wine writes
  CRLF; the awk column checks needed `tr -d '\r'`). The netcode itself was correct.
- **Fork A — integration seams pinned** in **`docs/th07_fork_a_integration.md`**, read
  straight from `PCBdecomp.c`: `FUN_00442cd0` (input-inject seam, frame index =
  `*param_1`), `FUN_00442c60` (seed-sync seam), `FUN_00443aa0` (record/playback task
  registration), mode flags in `DAT_0062f648` (bit 0x4 recording, 0x8 playback),
  replay-mgr object `DAT_004b9e48`. **Note:** `GameUpdate FUN_0042fd60` (named in §1
  for the menu-lockstep hook) is NOT present in this dump under that label — the A1
  menu-lockstep seam must be re-resolved (find what the frame governor `FUN_004346e0`
  calls once per logic frame). A2 (gameplay-only) is fully specified and is the path.
- **Fork A — integration DLL written** (`src/netplay/coop_net.cpp` → `th07_coop_net.dll`).
  Detours `FUN_00442cd0` (overwrite `g_InputGameplay` with the merged word) and
  `FUN_00442c60` (force shared seed); reads `coop_net.ini`; resets netcode on
  new-game frame reset. **Compile-verified only — NOT game-tested.** This is the next
  thing to put in front of the actual game (two PCs / two instances).
- **Tier 1 — boss HP scaling pinned** in **`docs/th07_boss_hp_scaling.md`**. Verified
  the inverted HP model: damage accumulates into enemy `+0xd18` toward cap `+0xd30`;
  phase ends when `d18 >= d30` (`FUN_00425400/580/700`). Scale target = `+0xd30`, set
  at the ECL set-life opcode in `FUN_00424290:14066`. Runtime-detour recipe included.
  **Caution recorded:** `FUN_0043958d` is a generic counter-lerp helper (~50 call
  sites) — do NOT hook it to scale HP.
- **Address audit:** spot-checked the load-bearing constants in `coop.c`/`harness.c`
  against the dump — `Input_Poll FUN_00430b50` (19469, writes `g_InputMenu`), player
  base `0x4bdad8`, collision fns `0x43e260`/`0x43e6b0`, res ptr `0x626278`, etc. — all
  present and consistent. No constant bugs found. `coop.c`'s struct offsets are also
  empirically validated by its successful runtime log (P2 spawns/updates/draws).
- **Observation (re-verify before chasing):** the committed `build/coop_log.txt` is from
  an OLDER, more-instrumented `coop.c` than the one committed (it logs "P2 draw PRE
  st=3", "update call #N" — lines the current `coop.c` does not emit). In that stale
  log P2 sits in **state 3 (respawn-invuln)** in the tail. Before debugging, reproduce
  with the *current* build. Map data gathered for that hunt: the player's per-frame
  **state processor is `FUN_00441330` (0x00441330)**, `__fastcall(player)`, called
  unconditionally from the player update `FUN_00441fb0` (line 27224). Its state-3→0
  transition (PCBdecomp.c:26875–26888) is **param-relative** — it fires when the invuln
  counter `player+0x16a08` drops below 1, and that counter is advanced param-relative
  too (`FUN_0043958d(player+0x16a08,…)` @26957). So the countdown *logic* supports a P2
  clone; if P2 still doesn't advance, the thing to check is whether
  `FUN_00441fb0`'s sub-calls actually pass `ECX = P2` (vs. reloading the P1 static base
  `0x4bdad8`) — that needs the disassembly, not the decomp (ECX is hidden in the C).

### ⚠️ BLOCKER for whoever runs next: git push was DENIED this session.
`git push` to the proxy returns **403 "Permission to baclo-san/PCB-Coop.git denied"**,
and the GitHub MCP returns **403 "Resource not accessible by integration"** (can't
create a branch or push files either). Reads (fetch/ls-remote) work; writes do not.
The work above is committed **locally** on `claude/optimistic-brown-xe5tz6` but could
not be pushed — if the container was reclaimed before a human re-pushed, recover from
this transcript. First action next session: verify write access, then `git push`.

## 5c. Session progress — 2026-06-11 (continued)

- **Recovered the working tree.** Found an interrupted interactive rebase whose
  `exec git commit --amend --no-edit -S` step was re-signing every commit — but
  no GPG key/`commit.gpgsign` is configured, so it could never succeed and had
  stalled. Branch was fully synced to origin, so the re-sign pass was both
  doomed and unnecessary; aborted it (nothing lost). No commits should be
  `-S`-signed unless a key is actually configured.
- **Finished the dangling `nightwork` feature.** That commit added `ReviveP2()`
  and an `s_prevF11` edge-tracker but never wired them (dead code + a
  `-Wunused-variable` that, on stderr, tripped `build.ps1`'s `Stop` pref and
  aborted the build). Wired **F11 → ReviveP2** (exit ghost, grant a life if at 0,
  drop into respawn-invuln next to P1). This is the manual/debug trigger for the
  th06-style resurrection; the proximity+graze auto-trigger replaces it later.
- **Tier-1 boss/enemy HP scaling — IMPLEMENTED** in `coop.c` (`HookedEclInterp`
  on `FUN_00424290`, Option A from the boss-HP doc). Scales `+0xd30` by the
  active player count (`1 + P2-present`), auto-armed while P2 is live, F5 to
  toggle. Builds clean, 32-bit. **Not game-tested** (see the doc's status).
- Build now completes all targets again (the stderr-warning abort is gone with
  the warning fixed).
- **Per-player cherry — design decided + RE done.** Re-framed determinism
  (lockstep makes all cherry globals auto-identical on both ends, so keeping the
  gameplay cherry shared is safe by construction). Corrected `FUN_0043e4e0`'s label
  (player-area overlap test, not the collect logic) and **implemented step 1: P2
  now collects items it overlaps** (`HookedCollectOverlap`, ECX=P2; credit shared).
- **Cherry model corrected to THREE values** (user domain knowledge, verified):
  Cherry (`DAT_0062f88c`, point value + roll), CherryMax (`DAT_0062f888`), **Cherry+
  (`DAT_0062f890`, the border)** — each a delta from base `*(0x626278+0x88)`. The
  border is Cherry+, recomputed each frame during state 4 from the *per-player*
  border timer (`FUN_00441330`:26914) — so P2's piggyback clobbers it; "shared
  border" needs `DAT_0062f890` reconciliation. Mapped the item-collect credit path
  (`docs/th07_item_collect_credit.md`): power/bombs/1up/points all to the shared
  pool; cherry is NOT credited by collection. ⇒ **separate power/bombs is
  implementable now**; separate cherry display + shared border need the cherry-gain
  trace + the `DAT_0062f890` fix.
- **3a IMPLEMENTED + GAME-TESTED: P2 collects items into its OWN power/bombs/lives.**
  `HookedCollectOverlap` + `HookedItemLoop` hold a whole-struct field-swap across each
  P2-collected item's credit (reuses the proven heal pattern; covers accessors AND
  direct power writes). User confirmed in-game: P2's power is separate, grows only
  from P2's pickups, and P2's shot-type levels off its own power — no anti-tamper
  crash. Score/cherry stay shared.
- **Cherry-gain: grazing does NOT give cherry** (user re-checked in-game; the wiki was
  wrong — matches the RE: graze credit `FUN_0043eb90` bumps graze counters + a score
  bonus, no cherry). Remaining cherry-gain lead = the shot→enemy hit handler.

### Updated next steps
> **User directive (2026-06-11): postpone all *manual netplay* testing until the
> game side is finished.** So step 1 below is parked — proceed through the
> game-side items (2–6) first, then run the two-instance live test.

1. ⏸ *(parked per user)* **Game-test `th07_coop_net.dll`** (A2): inject into two
   th07.exe instances with matching `coop_net.ini` (one host, one guest, same
   delay+seed), reach a stage, confirm both see the merged input + the RNG
   counter `0x0049fe24` stays locked. The netsim test already proved the lockstep
   logic in isolation; this is the live determinism proof, deferred to last.
2. **Game-side: investigate the P2 state-3 observation** (handoff §5b) with the
   *current* build — does P2 advance through respawn-invuln? This gates revive
   and the auto-resurrection trigger feeling right.
3. **Game-side: separate resources / cherry** (Tier-2). **DECIDED (user): separate
   counts per player, SHARED border.** Determinism-safe by construction (all cherry
   globals are lockstep sim state). Split into two now-distinct sub-tasks:
   - **3a. Separate power/bombs — implementable now** (collect-credit map is done,
     `docs/th07_item_collect_credit.md`). Set a collector flag in
     `HookedCollectOverlap` (P1 vs P2 overlap), detour the credit accessors
     (`FUN_004325e0` power, `FUN_0042d612` bomb) and field-swap+heal P2's value when
     P2 collected. Mind the anti-tamper guard — a local (non-netplay) smoke test is
     the cheap de-risk. This is what makes "P2 power separate" actually true.
   - **3b. Separate cherry display + shared border — needs more RE.** Cherry is the
     three-value model (cherry doc §0); it is NOT credited by item collection, so
     attribution needs tracing the cherry-*gain* path (`DAT_0062fXXX` moves). AND the
     **shared border** needs reconciling `DAT_0062f890` (Cherry+) around P2's update,
     which currently clobbers it (cherry doc §0). + a P2 cherry HUD.
4. **Game-side: auto-resurrection trigger** — replace the F11 stand-in with the
   th06 mechanic (hold focus + release shot near the ghost for ~90 frames → spend
   a life). The `+0xb7e68` spirit ptr + state 2/3 are the hooks (player-struct doc).
5. **Resolve the A1 menu-lockstep seam** (once-per-logic-frame hook) so
   char/difficulty select syncs; `GameUpdate FUN_0042fd60` is absent under that
   label in this dump — find what `FrameGovernor FUN_004346e0` calls once per
   logic frame. (RE work, no live netplay needed.)
6. **Real seed handshake**: host sends `Ctrl_Set_InitSetting.rng_seed_init`; guest
   adopts it (today both read the seed from config).
7. **Merge Fork A + Fork B**: feed the merged word's high bits to `coop.c`'s P2
   (it currently reads a local keyboard) — then do step 1.
8. ✅ ~~**Tier 1 boss HP**: implement the `FUN_00424290` detour.~~ Done (§5c).

## 5d. Session progress — 2026-06-12 (cherry border rework + P2 bullet-collision fix)

- **Shared cherry border — REWORKED + game-tested OK.** The border is the AUTOMATIC
  Cherry+ supernatural border (fires when the shared gauge hits 50000); it costs NO
  bombs (bombs are a separate spell-card mechanic — the prior "P2 bomb starts/pays for a
  border" model was wrong). The engine has a SINGLE border-ring effect slot (index 404,
  hardcoded in `FUN_0041c610`), so P2 can't run its own `FUN_00441960` (it would steal
  P1's ring). Design: **P1 keeps the one real ZUN border; P2 rides a ringless "shadow"**
  (state 4 + flag `0x240d` + P1's timer copied each frame, ring ptr left null) so both are
  invincible and either can pop it. Only the break leaf `FUN_00441bd0` is hooked (to
  propagate a pop to the partner). Full design + the two gotchas (single ring slot;
  `FUN_00441bd0` is `__thiscall(player,int flag)` `ret 4`, NOT ECX-only) are in
  `docs/th07_cherry_determinism.md §0`.
- **P2 BULLET COLLISION FIXED — the big one.** P2 could be killed by lasers/enemy-contact
  but NOT regular bullets, and P2 grazing was dead. Root cause (verified): **the per-bullet
  HIT test `FUN_0043e260` is GATED behind the bullet's "grazed" flag (`+0xc01`)** — a
  regular bullet must graze the player before its hit test runs (`PCBdecomp.c:14718-14731`;
  the laser path `FUN_00420490` is ungated, which is why lasers worked). coop.c hooked the
  hit but not the graze (`FUN_0043e3b0`), so P2 grazed nothing → bullets near P2 never got
  flagged → never hit-tested against P2. **Fix: hook `FUN_0043e3b0` and re-invoke for P2,
  returning "grazed" if either player did** — sets the flag (unlocking P2 bullet hits) and
  credits P2's graze. Confirmed in-game: P2 now grazes and pichuuns from bullets.
  - Confirmed along the way (keep for reference): P2's hitbox tracks its `+0x930` position
    param-relatively; `FUN_00441fb0`'s sub-calls (incl. movement `FUN_0043ee50`) all pass
    ECX = the saved player (so they DO drive P2); the sprite draws at `origin+(+0x930)` (no
    sprite/hitbox offset); P2's death FSM is correct. All collision/graze call sites use
    `mov ecx,0x4bdad8`.
- **Cleanup:** all temporary DIAG logging + the F2 (fat-hitbox) / F3 (force-hit) debug keys
  were removed after the fix landed.

## 5e. 2026-06-12 — branches merged to main; doc set consolidated

**Everything now lives on `main`.** Two parallel lines were merged:
- `claude/optimistic-brown-xe5tz6` — the joint sessions through 2026-06-12 01:07
  (everything in §5b–§5d). Fast-forwarded into main.
- `claude/adoring-lovelace-8azf2h` — the 2026-06-12 overnight unattended routine.
  It had branched from the stale `main`, so most of its work re-derived §5b–§5d
  results (good convergence: it independently resolved the A1 seam to the same
  `FUN_00437c70`, and made the identical `ws2tcpip.h` fix). Taken from it:
  - **`merge.{hpp,cpp}`** — MergeKeys extracted winsock-free + **native
    `tests/merge_test.cpp`** (no wine needed) — and **CI**
    (`.github/workflows/build.yml`: Linux build of everything + the native test
    on every push/PR).
  - **`netcode_c_api.{h,cpp}`** — C-linkage shim so `coop.c` can call the
    netcode directly (the single-DLL integration; plan in fork-a doc §8).
  - **New RE folded into the companion docs** (see those): death FSM /
    resurrection seam (`FUN_00440cf0`), score-side border/banner fns, the
    alternative damage-side boss-HP lever (`FUN_0043d9e0`), the
    `FUN_004012b0`-consumes-2-RNG-calls determinism note, the second menu poll
    site `FUN_0045bf15`, and the correction that **`GameUpdate` IS in the dump**
    (renamed — §5b's "absent" note was wrong; the frame governor is still
    genuinely absent).
  - NOT taken: its coop.c F5 damage-divide hook (conflicted with our newer,
    integrated ECL-cap scaling — kept as the documented fallback) and its
    stale-status handoff banner / docs (`th07_integration_forkA.md`,
    `th07_gameplay_seams.md` deleted after folding).
- Hygiene: `build/coop_log.txt` untracked (a stale committed log misled §5b);
  `.gitattributes` pins `*.sh` to LF; the push-recovery bundle removed.
- Verified after the merge: `build.ps1` builds clean on Windows and
  `netloop_test.exe` passes 16/16 (exercises the merge.cpp/netcode_c_api refactor).

### Plan (2026-06-12) — game-side first (netplay live test stays parked)
1. **Game-test Tier-1 boss-HP scaling** (only §5c item never play-tested):
   with P2 spawned, a boss phase should take ~2× damage and the bar drain at
   half rate. Fallback lever documented in boss-HP doc §5 if it misbehaves.
2. **Auto-resurrection** (replace the F11 stand-in): death FSM is fully pinned
   (player-struct doc). Design decisions for the user: revive input + window
   (deathbomb-window catch vs revive-from-ghost), cost (reviver's life), 90-frame
   hold? Then implement in coop.c + play-test.
3. **Cherry-gain trace** (gates per-player cherry display, 3b): the shot-hit
   path is now pinned (`FUN_0043d9e0` calc / `FUN_00420620:12822` apply) — trace
   where the cherry accumulator rises, then the per-player display attribution +
   P2 cherry HUD (score-side fns pinned in cherry doc §5).
4. **Wire the netcode into coop.c** (fork-a doc §8, no game needed): replace
   `ReadP2InputLocal()` with the merged word's high bits via `Nc_*`; one
   DLL-owned frame counter in a `FUN_00437c70` detour; seed sync in
   `FUN_00442c60`. Validate by compile + netsim; live test stays batched last.
5. Then: menu-lockstep A1 in-game validation, seed handshake via
   `Ctrl_Set_InitSetting`, ConnectionUI port, and finally the deferred
   two-instance live determinism test (§5c step 1).

## 5f. Session progress — 2026-06-12 (boss-HP test, resurrection, cherry decision)

- **Boss-HP scaling game test: NOT working for the midboss** (user: stage-1 Cirno,
  20 power, P2 dormant — kill time ~unchanged, definitely not ×2). The run's log
  proves the hook scaled a 60-HP fairy, but the one-shot log hid everything after.
  **Per-cap-write `eclhp:` diagnostic shipped** (boss-HP doc §status) — the next
  Cirno run will show whether her set-life ever passes `FUN_00424290` and, if so,
  which guard rejects it. Do NOT theorize past that log.
- **EoSD-style resurrection IMPLEMENTED (awaiting game test), user-specified:**
  ghost P2 now **auto-wanders** (bounces in the bottom ~1/5 band between the side
  walls, synthesized input through ZUN's movement), the survivor **graze-revives**
  it (within 32 units for 90 consecutive frames) at the cost of **one donated
  life — free if the survivor has no spare extends**, and **dying on the last life
  drops a guaranteed 1up** at the death spot via ZUN's item spawner
  (`FUN_004326f0` = `__thiscall(item_mgr, float pos[3], int type, int mode)`,
  type 5 = 1up, mode 0; item manager captured in the item-loop hook — NEW
  function pin, PCBdecomp.c:20244). F11 stays as debug instant-revive. P1-side
  ghost (P2 revives P1) is NOT in scope yet — P1's game-over is still vanilla.
- **DECISION (user): per-player cherry display DROPPED** — single shared cherry
  counter + the shared border stands; two counters would be confusing. The
  cherry-gain trace (§5e plan item 3) is RETIRED (cherry doc §6/§7 updated).
  **The P2 HUD (lives/bombs/power) is still wanted.**
- Revised plan: 1) user test run (eclhp log + resurrection feel), 2) fix boss-HP
  per the log, 3) P2 HUD, 4) netcode→coop.c wiring (fork-a §8), 5) menu lockstep
  A1 validation, seed handshake, ConnectionUI, live two-instance test last.

### 5f-addendum — first resurrection test + the eclhp verdict (same day)

**Test round 2 results (user) + fixes shipped:**
- **eclhp verdict: every cap write in the entire run was `0 -> 120` (popcorn).**
  Cirno's HP never passes `FUN_00424290`'s set-life — boss/midboss HP is set by
  some other path (un-RE'd). **Boss-HP scaling switched to the damage-side
  lever**: `HookedDamage` on `FUN_0043d9e0` divides only the returned damage by
  the player count (floor 1) — covers every enemy regardless of HP init. The
  ECL-cap detour + eclhp diagnostic were removed (the ~40-line log burst per
  wave spawn was also the likely cause of the user's post-Cirno freeze).
- **Crash (run 1, unreproduced):** P2's last-life death high up the screen
  crashed the game once; run 2 (death low) was clean. Unexplained — the death
  intercept now logs alivePos+itemMgr for forensics. Watch for a repro.
- **1up drop position was wrong:** it spawned at (192,384) — the FSM resets P2's
  pos to the respawn center before the intercept sees the death, and P1 standing
  there ate the 1up instantly ("just the sound of it being consumed"). Now drops
  at the tracked last-alive position.
- **Ghost feel (user):** band ≈ right but ~50px too low (raised: y 302–382),
  speed way too fast (input-bit steering = unfocused speed; now the position is
  driven DIRECTLY at 0.8 px/frame ≈ 1/3 focus, all input masked), revive
  impossible to channel (radius was 32 on a fast ghost). Now: radius 16 ("P1's
  hitbox inside the ghost sprite"), ghost semi-transparent (half-alpha tint
  `+0x1b8`, re-applied each frame), and the real graze-credit leaf
  `FUN_0043eb90` (`__thiscall(player, float* grazed_pos)`) fires every 6 frames
  of the channel for authentic graze SFX/spark/counter feedback.
- Awaiting test round 3: Cirno TTK with P2 (expect ~2× slower), ghost
  feel/speed/band, graze-revive channel, 1up at death spot, and ideally a repro
  attempt of the high-death crash.

### 5f — test round 3 results + the EoSD life-sharing mechanic

**Round 3 (user): boss-HP TTK ✅ (Cirno nearly timed out), high-death crash did
NOT reproduce ✅, ghost movement ✅.** Known accepted trade-off: popcorn now
takes two homing amulets (integer damage halving) — shelved (boss-HP doc).

Refinements + the forgotten EoSD mechanic, all implemented (build clean,
awaiting test round 4):
- **Graze feedback is now visual-only:** the graze-credit call during the
  channel is wrapped in a stat snapshot/restore (graze counters `res+0x14/+0x18`,
  score `res+0x04`, bonus accumulator `0x012fe0d0`) — spark + SFX remain, the
  HUD graze counter no longer rises.
- **Revive trigger reworked (user):** graze radius 24px; the 90-frame channel
  charges regardless of focus state; the revive fires only on a **focus
  RELEASE edge** after the channel is full.
- **NEW — life sharing between two LIVE players (EoSD mechanic):** both within
  24px for 90 consecutive frames with NEITHER shooting (same graze feedback),
  then the donor confirms with one focus release → donor loses a life, a 1up
  pops ~48px above the donor (plus the item's built-in upward pop) for the
  partner to catch. Pickup deliberately universal — a donor re-eating their own
  1up refunds the donation (net zero). P1 donates from the shared struct
  (direct write + checksum heal); P2 donates from `s_p2Lives`.

### 5f — symmetric ghosts + the phantom-spare system (round 4/5 fixes)

Round-4 feedback: revive/share work; remaining issues — (a) the run still ended
when P1 ran out of lives (ghost system was P2-only), (b) P2 resurrected with
FULL lives+bombs (ZUN's lives==0 path continue-resets the resources — cheesy),
(c) a last-life death emitted FULL-POWER items (only useful with continues).

**One mechanism fixes all three — the PHANTOM SPARE:** while co-op is active, a
player at 0 spares gets a phantom spare swapped into the lives field around
every update (write + checksum heal for P1; folded into the existing field-swap
for P2). ZUN's death commit therefore NEVER sees 0 lives → any death runs the
NORMAL path: usual partial power drop (no full-power items), vanilla respawn,
no continue reset, no engine game-over. We detect the consumed phantom
afterwards and overlay ghost mode ourselves.

- **P1 ghosts too now** (input masked around its update, MoveGhost drive, ghost
  tint, collision/graze/item-collect all skipped via `s_p1Ghost` in the hooks);
  P2 revives P1 by the same graze-90f + focus-release, donating from
  `s_p2Lives`. `ReviveByGraze`/`GrazeFeedback` generalized to (ghost, reviver).
- **GAME OVER is ours now:** only when a player goes down while the partner is
  already a ghost (`EnterGhostP1/P2` raise ZUN's flag + `s_runOver`). A stray
  engine game-over during P2's update is cancelled and logged.
- **Revive values fixed (user spec):** NO bonus lives (0 spares), bombs = stock
  at death (alive-tracked, overriding the vanilla respawn refill), power =
  whatever the normal death drop left.
- Untested edge flagged: the auto cherry border firing while P1 is a ghost
  (gauge can fill from P2's play; `FUN_0042f5a2` will border a ghost P1).
  Watch for weirdness in test; fix if it manifests.
  → **Round-5 verdict: everything above works; border-on-ghost confirmed
  harmless (ghost can't affect its flow).**

### 5f — round-5 fixes: session reset + P2 focus ring (new effect-system RE)

- **Stale session across games (user):** after a both-down game over, the next
  run started with P1 still flagged as a ghost. Fix: a >2s gap in P1 update
  ticks while `s_runOver` (updates stop between games) = new-game signal →
  full co-op session reset (ghost flags, tint, despawn the stale P2 clone,
  re-arm auto-spawn). NOTE: a manual quit-to-menu mid-run (no game over) still
  leaves a stale P2 across games — untested/unfixed path, flagged.
- **P2 focus ring — implemented, with new effect-system RE (decomp-verified):**
  - The focus visual = **effect type `0x18`**, spawned by the player's
    option-mode machine via **`FUN_0041c610`** = `__thiscall(effect_mgr, type,
    float *pos, slotArg, a5, color)` into **fixed player-effect slot
    400+slotArg** (focus = slotArg 2 → slot 402; the border ring = 4 → 404;
    slots 405–407 free). Handle stored at `player+0x9d8`; killed on focus
    release via `effect+0x1c6 = 1` (PCBdecomp.c:26412/26420).
  - The spawner **copies** the position (no pointer); following the player is
    done by the per-type update callback (`effect+0x2c8`, from the type table
    `DAT_0049efc4 + type*0xc`): for the focus ring it is **`FUN_0041abe0`**
    (line 10257), which snaps the effect to the **STATIC P1 position**
    (`DAT_004be408` = `0x4bdad8+0x930`) — absolute, so no clone can ever own
    one. This is why P2 never showed focus graphics.
  - **Effect manager is static at `0x012fe250`** (the 400-slot general effect
    array sits at +0x1c, stride 0x2d8; player-effect slots 400–407 follow).
  - coop.c fix: defang P2's vanilla slot-402 spawn each frame (kill the stray
    ring unless P1 is legitimately focused, zero `p2+0x9d8`), spawn our own
    type-0x18 ring in **slot 406** on P2's focus press, kill on release, and
    re-pin its position to P2 after updates AND at draw time (last write
    before the draw wins over the updater's P1-snap).
- Cherry-border ring for P2 stays a known cosmetic gap (single ring slot);
  user: unnecessary.

### 5f — round-6: ring-blip fix, stage-transition P2 rebuild, next goals

- **P1 ring blip confirmed** (P1 holding focus + P2 taps → P1's ring blinks):
  P2's vanilla machine re-initializes slot 402 before the defang acts. Fixed at
  the source: **detour `FUN_0041c610`** — when called from inside P2's update
  (`s_inP2Update`) with type 0x18 / slotArg 2, redirect to slotArg 6. ZUN's own
  machine now runs P2's ring lifecycle natively in slot 406 via `p2+0x9d8`
  (spawn on press, kill on release); coop.c keeps only the per-frame position
  re-pin. All the manual defang/spawn/kill code is gone. The clone's copied
  `+0x9d8` handle is zeroed at SpawnP2 (it pointed at P1's ring).
- **Stage-transition crash (user: crashed shortly after stage 2 loaded):**
  almost certainly the P2 clone's internal pointers referencing the previous
  stage's recycled assets. The new-game gap detector is generalized: ANY >2s
  gap in P1 update ticks while co-op state exists → despawn the stale clone +
  re-arm auto-spawn (P2 rebuilds from the fresh P1 ~3s into the new stage).
  **Resources carry across stages** (`s_p2Carry`); after a game over the next
  run reseeds from P1. A ghost is revived by the transition (still 0 spares).
  Known false positive: a >2s PAUSE rebuilds P2 with carried resources —
  tolerable; replace with a real stage-start hook (`FUN_00442c60` candidate —
  verify it fires per stage, not just per game) if it annoys.
- **NEXT GOALS (user, 2026-06-12):**
  1. **Test stage transitions** with the rebuild in place (the crash repro).
  2. **P2 character select** — the big one. P2 currently clones P1's loaded
     character. RE needed: where chardata (`+0xb7e70`), the char's anm sprite
     bank, and the shot-type tables load at stage start (likely keyed off the
     character/shot selection globals), and whether a SECOND character's
     assets can be loaded alongside (th07.dat anm loader). Likely staging:
     (a) same character, different shot type A/B first (asset overlap),
     (b) full different-character load after the loader RE.
  3. Then back to the netplay track (handoff §5e plan steps 4–5: netcode→
     coop.c wiring via `Nc_*`, menu lockstep, seed handshake, live test).

### 5f — round-7: stage-transition crash root-caused; stage-init hook; P2 HUD

- **The gap detector NEVER fired** (log-proven): PCB keeps the player task
  ticking straight through stage transitions — the revive channel even charged
  during the results tally (that's why the user's ghost "revived at the
  transition": idle P1 + bouncing ghost + focus release = legitimate but
  surprising donation; P1 paid a life). The stale stage-1 P2 clone then crossed
  into stage 2 with freed asset pointers → locked in place, crash on shoot.
- **Fix: hook `FUN_00442c60` (stage-init task)** — re-registered and run once
  per stage load; the positive stage-start signal (and the future netplay
  seed-sync detour, same seam). On stage init with co-op state: despawn the
  stale clone, clear ghosts/tint, re-arm auto-spawn; resources carry
  (`s_p2Carry`) unless `s_runOver` (post-game-over run reseeds from P1). The
  GetTickCount gap detector is REMOVED (its >2s-pause false positive with it).
- **P2 HUD (user priority) — implemented via ZUN's text queue:**
  `FUN_00402060(ascii_mgr@0x0134ce18, float pos[3], fmt, ...)` = vsprintf +
  queue on the ascii manager; its own draw task renders per frame (16px line
  height; ZUN's HUD "%d/%d" point counter uses it at (496,176) —
  PCBdecomp.c:17163; renderer = `FUN_004020b0`). coop.c queues once per frame
  from the draw hook, sidebar at x=448: `P2 L<lives> B<bombs> P<power>` (or
  `P2 GHOST`), `P1 GHOST` when applicable, and live `REVIVE n/90` /
  `SHARE n/90` channel progress.
- Awaiting test: stage 1→2 with P2 alive AND with P2 ghost (rebuild both ways),
  HUD legibility/position, focus-ring blip gone.

### 5f — round-8: FUN_00442c60 is NOT once-per-stage (P2 rebuild cycled)

- Round-7 test: HUD renders correctly; but **P2 was destroyed in a loop** —
  the log shows 12× `stage init -> rebuild -> despawn -> 3s -> spawn` cycles in
  one stage-1 run. **`FUN_00442c60` RE-FIRES mid-stage** (so it also re-zeroes
  the RNG counter mid-stage in vanilla — the fork-a doc §3 claim "runs once" is
  corrected; harmless for the idempotent seed-sync detour, fatal as a
  stage-start signal). Symptoms explained: invisible P2 (despawned), one-frame
  flickers (its single live frame), stuck spell-name/bomb-circle (despawn
  mid-bomb left the global spell state dangling).
- **Fix: stage start = the logic-frame counter going BACKWARDS.** coop.c now
  hooks **`FUN_00442cd0`** (the per-frame record task — the netcode's
  input-inject seam, now installed and warm for the netplay wiring): `*param`
  increments once per logic frame and restarts from 0 when a stage load
  re-news the task object; a decrease triggers the one-shot session rebuild
  (same carry semantics). Pause-safe (counter only pauses, never decreases).

### 5f — round-9: spawn-together, bomb-lethality diagnostics; HUD verdicts

- Round-8 verdicts: rebuild loop gone, HUD functional + position OK, focus
  blip gone, `REVIVE n/90` works. **Shelved (user-accepted):** HUD in P1's
  native style (gauges + life/bomb icon rows) — needs the HUD sprite-draw RE;
  text version stands for now.
- **Both players now spawn together at stage start** (user request): auto-spawn
  fires 30 frames after P1 reaches state 0 post-fly-in; at spawn P1 steps
  -24px, P2 +24px — symmetric around the spawn point. (P2 does not share the
  fly-in animation — cloning during state 1 risks the fly-in target snapping
  both to the same spot; not attempted.)
- **OPEN BUG — P2 bomb not lethal** (user: focused ReimuA bomb's homing
  bullets "didn't home on anyone, as if enemies weren't present"; normal shot
  fine). Decomp facts gathered (NOT yet conclusive): the per-enemy update
  gates bomb interaction on **P1's bombing flag read ABSOLUTELY**
  (`DAT_004d44f8` = `0x4bdad8+0x16a20`, PCBdecomp.c:12690) and the
  damage-apply block has a second `FUN_0043d9e0` sweep against a second enemy
  box whose result swaps in a hidden-ECX getter value (12824-12826) — the
  suspected bomb-damage path. Whether P2's bomb EVER dealt damage is unclear.
  **Diagnostics shipped:** bombing-transition logs for both players + the
  first 10 damage-fn returns while any bomb is active. Bisect run requested:
  P1 bomb vs P2 bomb, with F5 (damage divisor) ON and OFF, and P2 bomb with
  F7 (shot transfer) OFF — read the log after.
- **BISECT VERDICT (same day): P2's bomb NEVER dealt damage.** P1's bomb:
  `r=8, outflag=1, self=004bdad8` ×10 per bomb. P2's bomb: ZERO damage calls,
  invariant under F5/F7 — our hooks innocent. The per-enemy sweep invokes
  `FUN_0043d9e0` with **ECX = static P1 always**; the fn itself is
  param-relative (reads the given player's bombing flag + bomb projectiles).
  **Fix: `HookedDamage` re-invokes with ECX = P2 while P2 is bombing and adds
  the result** (out_flag propagated; no shot double-count — P2's regular shots
  live in P1's array via the transfer). Awaiting test: P2 bomb damage + whether
  the homing also resumes (suspected downstream of sweep contact).
- **Bomb damage CONFIRMED in-game; homing was separate — root-caused + fixed:**
  the per-enemy update writes the frame's chosen homing-target position ONLY
  into the **static P1's `+0x2428/+0x242c`** (absolute `DAT_004bff00/04`,
  PCBdecomp.c:12908-12932; `+0x2440` = chooser's "have candidate" flag). All
  homing READERS are param-relative: bomb orbs steer at `player+0x2428`
  (6061, in the bomb fn `FUN_004091b0` — orb slots at `player+0x16a4c`,
  stride 0x1428, ×8; the bomb damage boxes live at `player+0x9dc`, ×8 ×0x20)
  and homing shots likewise (25226) — P2's normal shots only home because
  they're transferred into P1's array. P2's own field stayed at the ≤-100
  sentinel forever. **Fix: copy P1's `+0x2428` (8 bytes) into P2 at draw time**
  (the bomb fns run from the player draw). Both players home at P1's chosen
  enemy (the chooser ranks by distance to P1) — acceptable. Bomb DIAG logging
  stripped. Bomb damage balance (currently also halved by the co-op divisor)
  deferred to a later rebalance pass (user).

### 5f — round-10: revive invuln armed; P2 shots stay home (MarisaB lasers)

- Verdicts: stage transitions pass (alive + ghost P2); bomb damage + homing
  confirmed working.
- **Revive left P2 stuck flickering-invulnerable** (until a border rewrote the
  fields): `ReviveP2/P1` set state=3 without arming the multiplexed timer
  triplet. Now armed explicitly: `+0x16a08=240` (≈4 s invuln), `+0x16a04=0`,
  `+0x16a00=0xfffffc19` (the −999 count target) — the state-3 handler counts
  down and exits normally.
- **MarisaB option lasers dead for P2 — root-caused:** player lasers ARE shot
  slots, but they're maintained through owner-side slot POINTERS
  (`player+0x169d0/+0x169e0/+0x169f0`, 3 slots ×0x10 with timers at
  `+0x169c4/+0x169cc`; per-frame maintenance in the shot-update
  `FUN_0043d2f0`, PCBdecomp.c:25665). `TransferP2Shots` moved the slot into
  P1's array and cleared the original — orphaning the pointer; the laser died
  instantly. **Fix: the transfer is OFF by default** (F7 re-enables as legacy
  A/B); P2's shots now live in P2's OWN array — drawn/moved param-relatively
  as always — and `HookedDamage` re-invokes the enemy sweep with ECX=P2
  unconditionally (not just while bombing), covering P2's shots, lasers, and
  bombs. No double-count: each shot lives in exactly one array. Side benefit:
  P2's homing shots now read P2's own homing-target field (fed at draw time)
  instead of riding P1's array.
- Awaiting test: revive → invuln ends ~4 s; MarisaB P2 lasers fire/damage;
  ReimuA P2 normal+bomb still fine (the array switch touches all shot types).

### 5f — round-11: SakuyaA aimed focus shot for P2

- Round-10 verdicts: **all pass** (revive invuln ends, MarisaB lasers work,
  ReimuA unaffected). One new find: **SakuyaA's focused AIMED shot fired
  straight for P2** (the focused spellcard aimed fine).
- Same disease as the homing bug: the enemy update maintains a whole
  absolutely-addressed target block at static P1 `+0x2428..+0x2443` —
  homing xyz (`0x4bff00/04/08`), the **SakuyaA aim target** xyz
  (`0x4bff0c/10/14`, only filled when global char id `DAT_0062f645 == 2`
  and the enemy sits in the upward ~60° cone from P1's position,
  PCBdecomp.c:12913-12943), and a valid flag (`0x4bff18`). The aimed-shot
  spawn callback `FUN_0043c0d0` (25168) consumes `param_1+0x2434`
  param-relatively, so P2's copy was the −999 sentinel → straight shot.
  (The spellcard aims via a different path — it never broke.)
- Fix: the existing draw-time homing memcpy now mirrors the **full 0x1c-byte
  block** (`HOMING_TGT_LEN`) into P2 instead of 8 bytes.
- Charselect note for later: the aim-target *acquisition* is gated on the
  GLOBAL char id == Sakuya. If P2 is Sakuya but P1 isn't, the block never
  fills — the acquisition loop will need a P2-aware shim then.
- Awaiting test: SakuyaA P2 focused shot aims; then green light for
  P2 charselect.

### 5f — round-12: P2 shot-type select (charselect stage 1) — F3

- Round-11 verdict: **SakuyaA aim fix works.** User note for LATER: the
  P1-centric target *selection* (both homing and aim source from P1's
  position) is a noticeable gameplay-feel change for Sakuya — seek a
  **per-player aiming/homing source** eventually (would mean running our own
  target acquisition for P2 in the frame hook, mirroring PCBdecomp.c
  12904-12943 with P2's position; the consumption side is already per-player).
- **The loadout architecture, fully RE'd** (decomp + disasm of the init
  call sites at 0x442400/0x442429):
  - Globals: `0x62f645` char (0R/1M/2S), `0x62f646` type (0=A/1=B),
    `0x62f647` combined sel 0-5. Set at game start from the menu choice.
  - Player init `FUN_004423e0` (27340) consumes them once:
    `FUN_00442b70(ECX=&player+0xb7e70, EDX=nameTbl0x49f530[sel])` loads the
    UNFOCUSED `data/ply{00,01,02}{a,b}.sht`, and `+0xb7e74` /
    `0x49f548[sel]` the FOCUSED `…s.sht` twin. The loader relocates the
    buffer and resolves per-shot-entry cb indices via tables at 0x49ecb0+
    (entry+0x24 spawn cb — 0x43c0d0 is SakuyaA's aim; +0x28 move — 0x43c250
    is homing; +0x2c/+0x30 draw).
  - Bomb logic is 4 per-sel fn ptrs IN THE PLAYER (+0x16a3c..48) from the
    table at **0x49ec50** (6×4: unfoc-bomb upd/draw, foc-bomb upd/draw;
    e.g. ReimuA 0x408710 = Fantasy Seal update). Update calls +0x16a3c/44,
    draw calls +0x16a40/48.
  - Init also bakes from the unfocused .sht header: speeds +0x994/+0x990 =
    sht[0xc]/2, +0x9a0/+0x99c = sht[0x10]/2, hitbox +0x23f8 = sht[8]. The
    item loop reads the GLOBAL header (POC line sht[0x20], attract sht[0x14])
    — global copies of the buffers live at DAT_00575948/4c, freed at stage
    end by FUN_004428e0.
  - **Every gameplay consumer is param-relative** — the firing iterator
    `FUN_0043d160` (25600) walks player+0xb7e70/74 (picks the power-bracket
    list from header+0x34). So a per-player loadout = rewriting those fields.
- **Implemented (F3 toggles P2 between A/B of P1's character):**
  `ApplyP2Selection` loads P2's .sht pair through the engine's own loader
  (cached per sel for the process lifetime — never freed, so live shots'
  cb pointers stay valid across re-applies/stages), writes P2's +0xb7e70/74,
  installs the 4 bomb cbs, re-bakes the header stats. `SwapSelGlobals`
  swaps the three selection globals to P2's identity around P2's update,
  draw, and damage-sweep calls — covers char-gated GLOBAL branches that run
  inside the player path (MarisaB's fire-suppress during a border
  `FUN_0043d880`:25804, SakuyaB checks at 26391). HUD now shows `P2A`/`P2B`.
- **Known/accepted (stage-1):**
  - ReimuA's damage nerf in the enemy update (12844, gates on GLOBAL f647==0)
    applies P1's identity to the COMBINED damage — rebalance bucket.
  - SakuyaA's aim-target acquisition gates on GLOBAL char==Sakuya (12913) —
    same-char teams fine; cross-char is a stage-2 problem.
- Round-12 verdict: **all shot types work, none break.** Green light for
  different characters.

### 5f — round-13: P2 different CHARACTER (charselect stage 2) — F2

- **Why shifted-id-base (the old stage-2 plan) was WRONG:** the player
  MOVEMENT update re-binds the body sprite to HARD-CODED ids 0x400..0x404
  every tilt change (`PCBdecomp.c:26323-26341,26759`, reading
  `mgr+0x29ef0..0x29f00` directly), and shots bind ids straight from the
  .sht — all 0x400-range, in code SHARED with P1. A shifted base would need
  patching code sites that also serve P1. Dead end.
- **The anm system (RE'd):** one anm manager (base ptr at `*0x4b9e44`) holds
  two parallel global tables — scripts at `mgr+0x28ef0+id*4` (a bytecode ptr)
  and sprite defs at `mgr+0x60+id*0x40` (0x40-byte entry: tex ref + UV +
  a global sequence at +0x3c) — plus a per-SLOT file ptr at
  `mgr+0x2def0+slot*0xc` (0=free) and a per-id reverse-base at `mgr+0x2b6f0`.
  `FUN_0044df90(ECX=mgr, slot, file, base)` (TRUE __thiscall: 3 stack args,
  callee-cleaned) registers a char's scripts AND sprites at `base+local`;
  `FUN_0044e4e0(mgr, slot)` frees a slot + clears its ids. A bind
  (`FUN_0044ea20`) stores the RESOLVED script ptr into the sprite obj (+0x77)
  — resolves once.
- **Implementation (table-swap):** both chars load at base 0x400 into
  SEPARATE slots; we swap the 0x400-range table entries to P2's char around
  P2's update + draw (the only windows P2's binds/anim run). `LoadP2CharAnm`
  picks a free high slot (scan 0x31→0x14, skip 10), snapshots the script +
  sprite tables across `[0x400,0x4a0)` (below the in-stage dialogue-face ids
  at 0x4a0+), loads P2's `data/player0{N}.anm` over base 0x400, **diffs** to
  capture exactly the ids P2 defines (script ptr + 0x40-byte sprite each),
  then restores P1's tables. `SwapAnm(enter)` exchanges those captured ids
  in/out (self-inverse swap, re-entrancy-guarded). The reverse-base table
  needs no swap (both chars = 0x400) and the per-slot texture array is
  per-slot (P1's slot 10 untouched). `.sht`/bombs/stats were already
  char-agnostic from round-12, so different-char = anm swap only.
- **Lifecycle:** F2 cycles P2's char (Reimu→Marisa→Sakuya, keeps A/B);
  `s_allowDiffChar` lifts the same-char clamp. `DespawnP2` + the stage-rebuild
  `FreeP2CharAnm` (no-op if slot already cleared, so transitions are safe);
  respawn reloads against the new stage's mgr. HUD shows `P2{R/M/S}{A/B}`.
- **Known v1 gaps (cosmetic, not crashes):**
  - Body shows P1's char until P2 first MOVES (tilt handler re-binds on a
    tilt-state change; static P2 keeps the clone's bind one beat).
  - OPTION sprites (player+0x24c/+0x498, bound once at init to ids
    0x480/0x481, never re-bound) keep the CLONE's P1-char option sprite —
    fires P2's shots but the little satellite art is P1's. Fix = rebind the
    two option objs to P2's captured 0x480/0x481 scripts (deferred — the
    real `FUN_0044ea20` ECX convention was ambiguous; replicating its
    else-branch + FUN_004010f0/FUN_00450d60 side effects needs care).
  - Cross-char globals from round-12 (ReimuA damage nerf @12844, SakuyaA aim
    @12913) key off the GLOBAL char id; `SwapSelGlobals` covers the ones
    inside the player path, but the enemy-update ones see P1.
- **TEST round 13a (Reimu P1):** first F2 (→ MarisaA) WORKED — body, shots,
  bomb all Marisa, no crash, no cosmetic faults (textures resolve correctly,
  so the swap design is sound). Second F2 (→ SakuyaA) crashed: P1's sprite
  vanished, crash on shoot / next F2.
- **BUG + FIX (the free clobber):** the FIRST transition never frees (nothing
  loaded yet) — that's why it worked. The SECOND calls `FreeP2CharAnm`, whose
  engine slot-free `FUN_0044e4e0` ZEROES the global script + sprite tables for
  that slot's ids. But P2 loaded at base 0x400, so those are the SAME ids P1
  uses, and the table then holds P1's entries → the free wiped P1's body/shot
  scripts (P1 vanished; a NULL bind crashed on shoot). Fix: `FreeP2CharAnm`
  now snapshots P1's `[0x400,0x4a0)` window, lets the engine free (it also
  releases P2's buffer + textures), then restores P1's window. (The implicit
  free on reload is harmless — we always free the old slot first and load into
  a fresh empty one.)
- **Bomb declaration portrait is P1's (SEPARATE, cosmetic, deferred):** the
  bomb cb calls `FUN_0042868d(0x4a1, <spell name>)` — portrait id **0x4a1 is
  in the FACE range (0x4a0+)**, NOT the player anm, and only P1's character's
  face anm is loaded in-stage (`face_rm/mr/sk00.anm` at stage start by global
  char id). So P2's bomb shows P1's face. Outside our 0x400-range swap; fixing
  needs P2's declaration-portrait anm loaded too. Not a crash.
- Awaiting re-test: F2 cycle Reimu→Marisa→Sakuya→… repeatedly while P1 is
  Reimu → P1 stays visible, P2's body+shots+bomb are the new char, no crash on
  spawn / move / shoot / bomb / repeated F2 / stage transition / revive.

### 5f — round-13b: bomb declaration portrait + anm overlay refactor

- **TEST round 13a-fix:** different-char P2 is functional (Marisa/Sakuya body,
  shots, bomb all correct). Remaining items the user reported:
  1. **Bomb declaration portrait shows P1's face** (FIXED below — the ask).
  2. SakuyaA aimed knives don't aim when P2=Sakuya, P1≠Sakuya — the known
     cross-char GLOBAL-gate (aim acquisition in the ENEMY update @12913 reads
     the global char id; `SwapSelGlobals` only covers P2's own update). This is
     the "per-player aim source" task. **Deferred** (user aware).
  3. Switch crash on a 2nd/3rd F2 — **user said leave it** (real play picks the
     char once). Cause known: freeing the old char's anm leaves P2's in-flight
     shots bound to freed bytecode. A per-char anm cache would fix it; not worth
     it pre-menu.
  4. Stage transition: P1 briefly renders glyphs/number sprites then doubles
     before separating — transient, mechanically fine (resources + shot type
     transfer correctly). Likely the overlay free/restore racing the new
     stage's anm load for a frame. Noted, not chased.
- **Portrait fix (the declaration is a SECOND char-specific anm):** the bomb cb
  calls `FUN_0042868d` which binds the portrait from the FACE anm
  (`face_{rm,mr,sk}00.anm` @ base 0x4a0, id 0x4a1) loaded for the GLOBAL char.
  `FUN_0044e8e0` caches UV at set-time but resolves the TEXTURE from the live
  table entry at DRAW time, and the declaration DRAWS in a global UI pass
  (`FUN_0042b603`, runs while a player-bomb declaration is active — it draws the
  +0x574c portrait obj; boss spell cards use a different obj/path). So P2's face
  must be swapped in at BOTH creation (P2's update) and draw. Implemented:
  - load P2's face anm alongside the player anm (overlay window [0x4a0,0x4b0));
  - `SwapFace` folded into P2's update window (so the set-time UV caches P2's);
  - hook `FUN_0042b603` (ADDR_DECL_DRAW 0x0042b603) and swap P2's face around it
    when `s_declP2` (the active declaration was created during P2's update,
    detected by the `DAT_00575ab4`→2 transition; cleared when P1's update makes
    a declaration).
- **Refactor:** the load/capture/free/swap is now a generic `AnmOverlay`
  (slot + captured ids/scripts/sprites + window); the player anm and the face
  anm are two instances. `LoadP2CharAnm` loads both; `FreeP2CharAnm` frees both
  (each restores the live window across the engine slot-free — the round-13a
  clobber fix, now generic).
- Awaiting test: P2=Sakuya (P1=Reimu) bombs → Sakuya's portrait + spell name in
  the declaration; P1 bombs → still Reimu's; no crash; repeated bombs both ways.

### 5f — round-13c: round-13b REVERTED (portrait attempt regressed)

- **TEST round 13b: REGRESSION.** After F2 BOTH players rendered as stage-start
  glyph sprites, the bomb portrait was still wrong, and bomb-bullet textures
  vanished. I.e. the LIVE (un-swapped) 0x400 player table was corrupted — the
  face-overlay load and/or the `AnmOverlay` refactor broke the player path that
  worked in 13a. Couldn't be diagnosed without in-game iteration (user AFK).
- **Action: `src/coop/coop.c` reverted to d0926b1** (the round-13a state — F2
  different-character confirmed functional, the only knowns being the wrong bomb
  portrait, SakuyaA cross-char aim, and the accepted cycle-back crash). This is
  the confident shippable baseline. The face/portrait + refactor are dropped
  from the build; the analysis below stays as the design record.
- **Why the regression likely happened (hypotheses, for next time):**
  - Most suspicious: loading `face_XX00.anm` at base 0x4a0 corrupted the global
    sprite/texture state shared with 0x400. The face window [0x4a0,0x4b0) is
    only 0x10 ids — if the face anm spans past 0x4b0 the load wrote ids we never
    restored. But that alone wouldn't corrupt 0x400, so something broader (the
    per-slot texture array / the global sprite-sequence counter mgr+0x28eec, or
    a bad 2nd free-slot pick at stage 2) is implicated.
  - The decl-draw hook on `FUN_0042b603` (a per-frame global draw fn) is the
    other new surface; a swap held across the wrong frames could corrupt draws.
- **DO THE PORTRAIT IN MENU-SELECT, NOT MID-STAGE.** The clean place to load
  P2's face is BEFORE the stage starts (at character select), when no live
  player/shot entities reference the tables and the stage anm set isn't mid-flux.
  At menu time, load P2's char's player anm + face anm into dedicated slots ONCE,
  capture the id sets, and the in-stage code only ever SWAPS (never loads/frees
  mid-stage). That removes the mid-stage load/free churn that this round and the
  cycle-back crash both stem from. The 13b design (overlay + decl-draw hook +
  s_declP2 latch) is reusable — only the LOAD timing should move to menu-select.
- Current shippable state = d0926b1 logic: F2 different character works
  (body/shots/bomb), portrait shows P1's face, SakuyaA cross-char aim absent,
  cycle-back F2 can crash. Good enough to build menu-select on.

### 5g — EoSD-style menu character select (DONE — 2026-06-14)

Goal: P1 picks char+type on the normal screen; instead of starting, the game
holds on character select and lets P2 pick its own char+type; then start.

**AS BUILT** (commit on `main`, all in `src/coop/coop.c`):
- Hook the screen dispatcher `FUN_004554d6` (`HookedMenuDispatch`, ECX=menu).
  FSM `CM_IDLE → CM_P2_CHAR → CM_P2_SHOT → CM_COMMIT`.
- P1's shot-type COMMIT is intercepted pre-orig (substate==1 + confirm edge +
  not-practice-locked): capture P1's `645/646`, `MenuGotoState(menu,state-1)`
  back to char-select, FSM=P2_CHAR. Engages for normal (4/5/6) + practice
  (0xc/0xd/0xe); extra (8/9/0xa) left vanilla.
- During P2's pass, route `DAT_004b9e4c|prev` = real P1 menu input OR P2's
  synthesized menu bits (`ReadP2MenuInput`: IJKL/Space/O in MENU layout —
  up 0x10 down 0x20 left 0x40 right 0x80 confirm 0x1001 cancel 0xa) so either
  player can drive P2's cursor (no soft-lock). Restore real input after orig.
- On P2's shot COMMIT (orig dispatcher returns 0): record `s_p2Sel` +
  `s_allowDiffChar`, restore P1's `645/646/647`, FSM=COMMIT. The existing
  in-stage auto-spawn machinery loads P2's char anm + bakes the loadout — the
  clean **stage-start** load §5g called for (no live entities).
- `ResetCoopSession()` (called from the dispatcher, front-end only): full
  teardown on return to menu — frees clone + anm slot, DROPS stale fx/effect
  handles (never deref), clears every latch. Needed because returning to the
  menu doesn't hit the in-stage `HookedFrameTask` rebuild; without it the stale
  P2 clone froze the 2nd game's stage load.

**Companion fixes shipped same session** (different-char robustness):
- **Anm slot choice**: scan for a slot the engine NEVER loads into
  (`kGameAnmSlots` blocklist; only 0–0x31 exist, loader frees-first). 0x31 was
  the staff-roll slot → mid-game reuse freed P2's anm → "crash after some time".
  Now picks 0x30 down, skipping the blocklist. `SwapAnm` also self-validates the
  slot still holds our file (retires the overlay gracefully if ever reused).
- **Reverse-base table** (`mgr+0x2b6f0`): the engine slot-free zeroes script,
  sprite AND reverse tables; `FreeP2CharAnm`/`LoadP2CharAnm` now snapshot+restore
  all three (fixes the stage-transition glyph bug). And `SwapAnm` now SWAPS the
  reverse-base too — the sprite bind does `global = local + reverse[id]`
  (PCBdecomp.c:34342); at Marisa-only ids P2 was reading Reimu's reverse (0), so
  MarisaB's laser bound to 0x46 (a font glyph) instead of 0x446 → "red-ish wrong
  sprite". Swapping reverse fixes the laser graphics.
- **Per-character starting bombs** `kCharStartBombs = {Reimu 3, Marisa 2,
  Sakuya 4}`: P2 seeds bombs from its OWN character on a fresh game, and a normal
  death's respawn refill is overridden to the char default (ZUN refills to the
  P1-tracking config value otherwise).
- **Retry vs stage-advance**: `HookedFrameTask` uses a team-score DROP
  (`RES_SCORE` 0x04, phantom-spare-proof) to tell a retry/fresh start (reset P2)
  from a stage advance (carry resources).

Remaining gaps: bomb-declaration PORTRAIT still shows P1's face for a
different-char P2 (the portrait is a global char-id anm at base 0x4a0; not yet
overlaid); SakuyaA cross-char aim source (deferred). See §8.

### 5h — P2 icon HUD (§8a) + P2 SELECT prompt (§8d) — 2026-06-14 (overnight)

Both shipped on branch `claude/affectionate-cray-grpwnb` (PR #2, draft). Additive
+ toggleable so the known-good build is preserved. **Not yet visually confirmed
in-game** — no game host this session; F12 falls back to the old text HUD if the
icons render wrong.

**8d — "P2 SELECT" prompt** (`HookedMenuDispatch`): during P2's char/shot pass,
queue `P2 SELECT CHARACTER` / `P2 SELECT SHOT` on the global ascii manager
(`FUN_00402060` @0x402060, mgr 0x0134ce18). Confirmed the ascii subsystem is
registered at process init (`FUN_00401e30`→`FUN_00401d70` loads `data/ascii.anm`
into anm slot 1; its draw task is a global task), so it renders in the menu scene
too — not just in-stage. **Cross-confirmed:** ZUN's own front-end menus (high-score
/ music room / stats list at PCBdecomp.c 30405-30602, 38085-38190) print via the
same `FUN_00402060`/mgr 0x0134ce18, proving the queue draws on menu screens. So the
prompt WILL show; only its position vs the char-select art is unverified — tune
`MENU_PROMPT_X/Y` (#defines) after a look.

**8a — P1-style icon HUD for P2.** RE of ZUN's sidebar draw:
- HUD draw fn `FUN_0042b603` (@0x42b603, `__fastcall(ECX = score singleton
  0x626270)`). Sets a full-screen (0,0,640,480) viewport then paints the sidebar
  onto a **persistent surface**: background tiles redraw only on a "full-dirty"
  condition (line 16943: `DAT_00575a9c>>0xc&1 || *(scoreStruct+0x1d70) ||
  DAT_00575ab4`), each value section only when its 2-bit dirty field in
  `*(singleton+4)` is set.
- `scoreStruct = *(singleton+8) = *0x626278` — the LARGE score-manager data block
  (~0x20a30 bytes, cleared at FUN_00427… init), NOT a 200-byte struct (the old
  coop.c "operator_new(200)" note is a misread; the VALUE offsets it uses —
  lives +0x5c, bombs +0x68, power +0x7c — are still correct).
- LIVES are a **row of icon sprites**: baked sprite object at `scoreStruct+0x14ac`,
  one per life, X=496 +16px each, Y=96, scale 0x3eeb851f. BOMBS: object
  `scoreStruct+0x16f8`, Y=112. The blit is `FUN_0044f770(spriteObj)` (@0x44f770,
  `__cdecl`; self-validates flags at +0x1c0/+0x1bb → no-ops if unbound, so it's
  crash-safe). It writes screen X/Y/scale to the object at +0x1c8/+0x1cc/+0x1d0
  then APPENDS a quad to the anm sprite batch; the batch is flushed by
  `FUN_0044f5c0` (@0x44f5c0). Power + point-items are ascii numbers
  ((496,160) and (496,176) "%d/%d").
- **Implementation**: hook `FUN_0042b603`; before orig set `*(scoreStruct+0x1d70)=1`
  to force the full sidebar redraw each frame (so a dropped P2 count can't leave a
  stale icon on the persistent surface — the bg tiles span x416..624/y16..464,
  covering P2's region); after orig, append P2's life/bomb rows with the SAME icon
  objects + `FUN_0044f770`, just below P1's point line (lives Y=192, bombs Y=208),
  plus a `2P` marker and the power number (Y=224). Reusing P1's icon objects is
  safe because the blit reads position per-call. F12 toggles icons⇄text.
- **What to verify in-game**: (1) icons actually appear at x≈496 below the point
  line and look like P1's stars/bomb icons; (2) no stale/ghost icon when P2 loses
  a life or bombs; (3) no perf hit from the forced redraw; (4) the `2P`/power
  text sits where intended. Tune the `HUD_*` #defines if positions are off.

### 5i — general RE mapping pass + dump-completeness note (2026-06-14 overnight)

After 8a/8d, spent the rest of the unattended window mapping core systems the
repo hadn't documented (per the user's "map more of PCB" directive). New docs:
- **`docs/th07_hud_sprite_system.md`** — HUD/score draw `FUN_0042b603`, the anm
  sprite-object layout + blit `FUN_0044f770`, the batch flush `FUN_0044f5c0`, the
  ascii text queue `FUN_00402060`. (Basis for 8a; also for the 8b portrait.)
- **`docs/th07_bullet_system.md`** — enemy-bullet manager (singleton 0x0062f958),
  per-bullet struct (stride 0xd68, pos +0xb8c, graze flag +0xc01…), spawn
  `FUN_00423730`, update `FUN_00425a50`, draw `FUN_00426f60`.
- **`docs/th07_enemy_system.md`** — enemy manager/array (slots stride 0x4f48 in the
  0x954xxx region), per-enemy struct, the ECL-VM tick `FUN_00410520`, death
  `FUN_004202d0`, and the shot-damage sweep `FUN_0043d9e0` co-op scales.

**Dump-completeness finding:** `PCBdecomp.c` has exactly **ONE** function Ghidra
couldn't lift — `FUN_00410520` (the **ECL danmaku VM**, "Unable to decompile" at
line 8840). Everything else is present. The other genuinely-absent pieces are the
top-level loop fns noted in `fork_a` §6 (frame governor `FUN_004346e0`, task-chain
mgr `0x626218`). So the highest-value un-mapped systems all live OUTSIDE this dump:
the ECL VM (get it from the user's fuller Desktop `th07.exe.c`, a raw disasm of
0x410520, or community thtk/ECL docs) and the frame governor. In-dump systems
still worth mapping next: the player shot/.sht + bomb dispatch (supports 8c), the
item system, effects/particles, and the menu beyond char-select.

### 5j — aim/suction nearer-player + per-player aim/homing source (2026-06-15)

Two gameplay-targeting features, both committed on `main` (the latter awaits an
in-game look — implemented per the decomp, no live host this session):

- **Enemy aim + item suction now target the NEARER player (committed, user-confirmed
  working).** The one choke point every "toward the player" direction flows through
  is `Player::AngleToPlayer = FUN_00442370(this=player, pos)` (reads player+0x930/4,
  returns the atan2 angle as x87 long double). Its callers are enemy aimed shots
  (ECL 7825/8142, bullet fire 14298/14346, lasers 9591/14538) AND item-collection
  suction (`FUN_00432990` @20399). `HookedAngleToPlayer` hands the original a
  different `this` (the P2 object) when P2 is nearer to the fire/suction origin
  `pos` than P1 (or P1 is a ghost) — so shots/items track whoever was closer. No
  position/cache poking, no ECL-VM hook. This is the breakthrough the EoSD clean
  decomp pointed at (`BulletManager::AngleProvokedPlayer` → one seam). The
  `coop-enemy-aim-nearest-wip` memory hunt is **closed** by this.

- **Per-player aim/homing SOURCE for P2 (committed, needs in-game look).** P2's
  ReimuA homing amulets / SakuyaA aimed knives chased P1's target: the enemy update
  (`FUN_00420620`, PCBdecomp 12904-12943) fills the homing/aim block at the static
  P1 (`+0x2428` homing xyz, `+0x2434` aim xyz, `+0x2440` valid) using **P1's**
  position, and coop.c mirrored it to P2. Now `BuildP2TargetBlock` replicates ZUN's
  acquisition relative to **P2**: among bit6 (boss/lifebar) enemies pick nearest-in-X
  for homing; SakuyaA additionally records the nearest enemy in the upward ±30° cone
  as the aim target; popcorn falls back to lowest-enemy / in-cone (same valid-flag
  semantics as ZUN). The enemy set is snapshotted **for free** from `HookedDamage`
  (`FUN_0043d9e0(enemy+0x2b0c, …)` fires per damageable enemy → enemy base =
  `pos-0x2b0c`), avoiding an enemy-manager-base pointer-walk (crash risk). Cone is
  geometric (`dy<0 && |dx| ≤ -dy·tan30`) — no atan2/math.h dep. `FUN_0048bcaa` =
  the x87 `fpatan` intrinsic, confirming ZUN's cone is `atan2(dy,dx)∈[-120°,-60°]`.
  - **Never worse than the old mirror:** any channel P2 doesn't resolve this frame
    (empty snapshot, e.g. a boss mid-invuln that isn't currently damageable) falls
    back to mirroring P1's block.
  - **Also fixes §8c cross-char SakuyaA aim** as a bonus: `BuildP2TargetBlock` gates
    the cone on **P2's** character (`s_p2Sel`), not the global char id, and computes
    independently of the engine's P1-only acquisition. So P2=SakuyaA aims per-player
    even when P1≠Sakuya (the engine never fills an aim block then). The remaining §8c
    note is retired.
  - **To verify in-game:** (1) ReimuA P2 homing amulets prefer the boss nearest P2's
    column (vs P1's); (2) SakuyaA P2 focused knives aim up at the enemy in P2's cone,
    differing from P1's, both same-char and cross-char (P1≠Sakuya); (3) no perf hit
    from the per-frame snapshot; (4) bombs (consumed at P2's draw, fresh) home
    correctly, homing shots (consumed in P2's update) acceptably (1-frame source lag,
    same as the old mirror). `s_perPlayerAim` default on (no hotkey; F-keys full).

- **Character-specific bomb-declaration PORTRAIT for a different-char P2 — 2nd
  attempt, REGRESSED + REVERTED (f850120). See §8b for the verdict + next approach.**
  2nd attempt at §8b (round-13b regressed→reverted). The
  portrait is `data/face_{rm,mr,sk}00.anm`, loaded by the engine at base 0x4a0 into
  slot 0x19 for the GLOBAL char only (`FUN_0044df90`, PCBdecomp 15833/43/53); the
  player face id window is **[0x4a0,0x4ad)** (boss faces start at 0x4ad). Created by
  `FUN_0042868d` (a bomb cb; sets the portrait sprite, UV cached at set-time) and —
  **the key new finding** — drawn in **`FUN_0042c577` (@0x42c577, PCBdecomp 17301)**,
  NOT the `0x42b603` round-13b hooked (the likely regression cause). Implementation:
  a **parallel** face overlay (a copy of the proven player-anm overlay, NOT a refactor
  of it — working-build discipline) — `LoadP2FaceAnm` loads P2's face into its own
  spare slot at the CLEAN stage-start point (`ApplyP2Selection`/`SpawnP2`, no live
  entities — the round-13c lesson), diff-captures the ids, restores P1's tables;
  in-stage only SWAPS. `SwapFace` exchanges the portrait ids to P2 around (a) P2's
  update+draw window (create-time UV) and (b) `FUN_0042c577` when the live
  declaration is P2's. Ownership via hooking the create fn `FUN_0042868d`
  (`HookedDeclMake`, the proven `__fastcall`-models-`__thiscall` pattern): whoever's
  update/draw window created it owns it. Best-effort + isolated: a face-load failure
  / engine slot reuse retires the overlay (P2 shows P1's face, no crash); default-on
  (`s_p2Portrait`); freed on despawn/session reset.
  - **TEST RESULT: REGRESSED → REVERTED (f850120).** P1=ReimuA + P2=SakuyaA: at the P2
    spawn P1 turned to glyph sprites, P2 invisible (still grazeable), crash on P1
    shoot — the round-13b failure. Since the clean stage-start load + the corrected
    draw fn did NOT prevent it, the fault is the face-anm LOAD touching shared global
    state (texture array / sprite-sequence counter), not the swap/draw. Full verdict +
    the untried next approach (don't load a second whole anm; load at menu-select into
    the engine's own face slot 0x19, or snapshot the texture/sequence globals) in §8b.

### 5k — netcode wired INTO coop.c (single-DLL netplay, fork A §8) — 2026-06-15 (night)

The PRIMARY night-shift goal: P2's input now comes from the WIRE, not the local
keyboard, by linking the engine-agnostic netcode core straight into
`th07_coop.dll` (was a separate `th07_coop_net.dll`). Implements
`docs/th07_fork_a_integration.md §8`. **Compile- + native-merge-test verified;
NOT yet network-tested** (no two live game instances available this session).

**What shipped (all gated behind `coop.ini [net] enabled=1`, default OFF):**
- `coop.c` now `#include "netcode_c_api.h"` and the build links the netcode TUs
  (`netcode/Connection/merge/netcode_c_api.cpp`) + `-lws2_32` into
  `th07_coop.dll`. Because `coop.c` is C and the core is C++, the build compiles
  `coop.c` to an object with the C compiler then links with `g++`/`clang++`
  (libstdc++). `th07_coop.dll` stays a 32-bit PE (637 KB, machine 0x14c).
  `build.sh` + `build.ps1` both updated; both emit a `coop.ini` template.
- **One seam owns the frame counter + both input globals** (the §8 design):
  - `HookedSceneTick` detours **`FUN_00437c70`** (0x00437c70), the per-logic-frame
    scene-input task that runs in BOTH menus and gameplay (th07's
    `Supervisor::OnUpdate`). After ZUN polls (`g_InputMenu = Input_Poll()`), it
    calls `Nc_GetInputNet(s_netFrame++, is_in_UI, &ctrl)` and overwrites
    `g_InputMenu` with the lockstep-merged word. `is_in_UI = !((DAT_0062f648>>2)&1)`
    (recording-active bit = in a stage). In a stage the engine's own
    `FUN_00442cd0` then copies `g_InputMenu → g_InputGameplay` for free, so P1
    (low 9 bits) becomes the merged P1.
  - `HookedGameStart` detours **`FUN_00442c60`** (0x00442c60) and forces
    `g_RngState.seed = Nc_GetInitSeed()` before ZUN snapshots it (idempotent on
    the documented mid-stage re-fires).
- **P2's gameplay input** (the existing input-swap in `HookedUpdate`) now sources
  its word from `UnpackP2(s_netMerged)` (merged high bits → low-bit layout) under
  netplay, else `ReadP2InputLocal()`. `UnpackP2` is the exact inverse of
  `merge.cpp`'s NB_*2 (bit<<9) mapping.
- **Resilience:** if the lockstep peer is lost (the 5 s stall expires →
  `Nc_IsConnected()` goes false), `s_netActive` drops to 0 and P2 reverts to the
  local keyboard; seed-mismatch (`!Nc_IsSync()`) is logged once.

**Known limitation (this cut = A2 + menus-together):** under netplay the two-pass
per-player char-select FSM is BYPASSED (`HookedMenuDispatch` early-returns when
`s_netActive`). Both machines navigate the menu together off the merged UI word
(deterministic identical char/difficulty pick) and **P2 clones P1's character**.
Per-player char/type over the wire (drive the menu FSM from the remote word) is
the next step — see §8e. The seed is still config-supplied on both sides (A2
stand-in); a real host→guest seed handshake is also a follow-up.

**Safety:** with `enabled=0` (default) `StartNet()` returns immediately,
`s_netActive` stays 0, and every netplay branch is skipped — the confirmed-good
local-keyboard co-op baseline is byte-for-byte unchanged. The two new MinHook
detours are installed but their bodies are pure pass-through when `!s_netActive`.

**To network-test (next session, needs two machines / two th07 instances):**
1. Build, copy `th07_coop.dll` + `coop.ini` + injector to BOTH machines.
2. Machine A: `coop.ini` `enabled=1 role=host port=47000 delay=2 seed=0x1234`.
   Machine B: `enabled=1 role=guest peer=<A's IP> port=47000 local=47001
   delay=2 seed=0x1234`.
3. Inject on both, start a game from the same menu choices. Verify: (a) both see
   the same two players; (b) the guest's stick drives P2 on the host and vice
   versa; (c) `coop_log.txt` shows "netplay: UP …" and no DESYNC spam; (d) the
   5 s stall only appears if one side pauses. `seed`/`delay` MUST match on both.
4. If menus desync, that's the known A2 limitation — both must pick identically
   (they navigate together, so this should be automatic once both are connected
   before the menu).

### 5l — proximity transparency prototype (NIGHT_SHIFT #2) — 2026-06-15 (night)

Fade the OTHER player out as the two players overlap, so the local one stays
readable. `ApplyProximityFade` (coop.c, near `MoveGhost`) writes the remote
player's tint (`OFF_TINT` 0x1b8) after the player update each frame, alpha ramped
on the P1↔P2 **squared** distance (no sqrt/math.h): opaque ≥ `PROX_FAR2` (96px),
floored at `PROX_FLOOR` (0x40) ≤ `PROX_NEAR2` (24px), linear between. ASYMMETRIC
per the spec: `s_netActive && !Nc_IsHost()` ⇒ guest, fade **P1** (keep P2
opaque); else (host / single-machine) fade **P2**. Skipped while either player is
a ghost or the run is over (those own the tint). Gated behind `coop.ini [coop]
proximity_fade=1`, default OFF. Reuses the exact tint mechanism the ghost
half-alpha already proves. **Compile-verified; needs a visual look** to tune the
ramp/floor, and the real per-instance asymmetry needs a two-machine netplay test
(the single-machine path is the prototype — both screens are identical, so
fading P2 only "helps see P1" from P1's seat).

### 5m — RE coverage pass: player shot/bomb, hitbox, item spawner (2026-06-15 night)

After the two feature items, spent the rest of the window on zero-risk RE (the
user's "map more of PCB" directive), all verified against `PCBdecomp.c` this
session:
- **New doc `docs/th07_player_shot_bomb_system.md`** — the shot-fire gate
  (`(g_InputGameplay&1)` + `FUN_0042ad66` bombing predicate @16632 + fire entry
  `FUN_0043d990` @26584), the `.sht` buffers/baked stats, the per-shot slot struct
  (offsets confirmed by reading the damage sweep `FUN_0043d9e0`), and the bomb +
  cherry-border handler `FUN_004409f0` @26652. Honest confidence key throughout.
- **`player+0x23f8` resolved = death/deathbomb-window timer** (NOT a hitbox), via
  the dying-state update `FUN_00440cf0` @26730 (at 0 → finalize death: power→0,
  power-item drop `FUN_004326f0` type 4). Matches `th07_player_struct.md`'s
  "respawn timer". coop.c's `OFF_HITBOX` renamed → **`OFF_DEATH_TIMER`** (cosmetic,
  value/behaviour unchanged, builds clean).
- **Player hit/graze boxes LOCATED** (filled a gap `th07_player_struct.md` flagged
  "not yet located"): collision is an AABB — hit-box edges `+0x948/+0x94c/+0x954/
  +0x958` (`FUN_0043e260` @25995), graze box `+0x960/+0x964/+0x96c/+0x970` (+20px,
  `FUN_0043e3b0` @26035).
- **Item spawner mapped** (`th07_item_collect_credit.md` §5): `FUN_004326f0` @20244
  — signature, pool/cursor (stride 0x288), slot-init fields, spawn modes (mode 2
  scatter uses 2 RNG calls — netplay determinism note), max-power auto-convert.
- **Corrected `FUN_0048b8a0`**: it is a float→int round intrinsic (x87 ST0, ≈
  `lroundf`, @81531), NOT a bomb/power counter (an earlier-pass mislabel; fixed in
  both docs).

Still-open RE threads (low-risk, good next-session fillers): the actual shot-spawn
loop + `.sht` power→pattern selection (verify the §6 sketch), the raw player
half-extent input field, the bomb-stock decrement site, and the ECL VM
`FUN_00410520` (the lone undecompilable fn — needs an external source).

---

#### Original plan (kept for reference)

**The menu state machine (all verified in PCBdecomp.c):**
- Dispatcher `FUN_004554d6(menu)` switches on the screen-state word at
  `menu+0xd0f8` (0x36277):
  - 4 / 8 / 0xc → `FUN_0045798b` (char-select INTRO/grid build)
  - 5 / 9 / 0xd → `FUN_00457fe5` — **CHARACTER select**; on confirm sets
    `DAT_0062f645` (char id) and `FUN_00455435(menu, 6/0xc/...)` → shot screen
  - 6 / 10 / 0xe → `FUN_00459518` — **SHOT-TYPE select**; on confirm sets
    `DAT_0062f646` (type) and **COMMITS** (37670-37688): `DAT_00575aa8 = 2`
    (game mode), `DAT_0062f648 &= ~8`, `FUN_0043a05f()` (begins stage load),
    drain `FUN_0044c9c0()`, `return 0`. (The 4/5/6 vs 8/9/0xa vs 0xc/0xd/0xe
    triples are the normal / extra / practice variants.)
  - `FUN_00455435(menu, state)` (0x455435): the screen-transition setter —
    saves old state to menu+0x64, writes `menu+0xd0f8 = state`, resets the
    per-screen counters. Use it (or write +0xd0f8 directly) to send the menu
    back to char-select for P2's pass.
- Input globals: `DAT_004b9e4c` = current input bits, `DAT_004b9e54` = previous
  (the screens fire on a rising edge `cur & BIT != prev & BIT`). Confirm bit
  `0x1001` (shot/Z), cancel bit `0xa` (bomb/X). `DAT_00575a89` = the char index
  the shot screen is selecting for; `DAT_0062f647` = combined sel (char*2+type),
  written from 645/646 at player init.

**Implementation plan (two-pass, P2 picks second):**
1. Hook `FUN_00459518` (shot-type select). Track a small coop-menu FSM:
   `IDLE → P1_DONE → P2_CHAR → P2_SHOT → COMMIT`.
2. When P1 hits the COMMIT path (detect: about to set `DAT_00575aa8=2` — easiest
   to hook the commit by checking, after calling orig, whether it returned 0
   / `DAT_00575aa8` became 2; OR pre-empt by watching the confirm edge in state
   6 while our FSM is IDLE):
   - SAVE P1's `645/646/647` → `s_p1MenuSel`.
   - CANCEL the start: keep `DAT_00575aa8` at its menu value and send the menu
     to the char-select state via `FUN_00455435(menu, charState)`; set FSM =
     `P2_CHAR`. (Need the menu object ptr — it's the screen fns' param_1/ECX;
     capture it in the hook.)
3. While FSM is `P2_CHAR`/`P2_SHOT`, ROUTE P2's input into the menu: around the
   orig screen call, swap `DAT_004b9e4c`/`DAT_004b9e54` to P2's key bits (reuse
   `ReadP2InputLocal`, edge-tracked) so P2 drives the cursor. P1's input is
   ignored during P2's pass (or P1 confirm = lock-in, like EoSD).
4. On P2's shot-type COMMIT: SAVE P2's `645/646/647` → set `s_p2Sel`; **LOAD P2's
   anms NOW** (player + face) — this is the clean menu-time load the round-13
   regression calls for (no live entities, stage anm set stable); RESTORE P1's
   `645/646/647` into the globals so the engine starts the stage as P1; set FSM
   = COMMIT and let the real start proceed.
5. In-stage: SpawnP2/ApplyP2Selection consume `s_p2Sel` and the pre-loaded anm
   overlays; the per-frame code only SWAPS (never loads/frees) — so no
   mid-stage churn, and the cycle-back crash is moot (no F2 cycling in real
   play).

**Open items for the (testable) next session:**
- The menu object base address (capture via the screen-fn param, or find the
  global it's dispatched from in `FUN_004554d6`'s caller).
- Exactly which `FUN_00455435` target re-shows char select for the current game
  mode (state 5 vs 9 vs 0xd) — read `menu+0xd0f8` at intercept to pick the
  matching triple.
- A visible "P2 SELECT" prompt (reuse the ascii text queue) during P2's pass.
- Where the menu-time anm load should stash the overlays so they survive into
  the stage (process-lifetime statics; the stage rebuild re-loads anyway).
- Net play later drives P2's menu input from the remote word instead of
  `ReadP2InputLocal` — same swap seam.

## 6. Reference file locations
- Ghidra dump: `C:\Users\rndmdck\Desktop\th07.exe.c`  (committed in-repo as `PCBdecomp.c`)
- Reference mod: https://github.com/RUEEE/th06_multi_net (branch `master`, `src/`)
- th06 decomp base: GensokyoClub/happyhavoc
- Persistent memory index: `C:\Users\rndmdck\.claude\projects\d--PCB-Co-op-PCB-Coop\memory\MEMORY.md`
  → see `pcb-coop-overview.md` (this handoff is the expanded form of that memory).

## 7. Glossary of Ghidra fn renames (proposed; not yet applied in the db)
Rng_Core=FUN_00431870, Rng_Next32=FUN_004318d0, Rng_NextFloat=FUN_00431900,
Input_Poll=FUN_00430b50, Input_AddJoyBits=FUN_004303f0, Joy_SetBit=FUN_00430370,
DInput_Init=FUN_004383d8, FrameGovernor=FUN_004346e0, GameUpdate=FUN_0042fd60, GameDraw=FUN_0042fe20,
ReplayRecord=FUN_00442cd0, ReplayPlayback=FUN_00442ee0, ScoreCherryDisplay=FUN_00427f22.
(Renaming these in Ghidra + the input/cherry globals is a cheap, high-value early task.)

**Mapped 2026-06-14 (see the new RE docs):**
HudDraw=FUN_0042b603 (singleton 0x626270), SpriteBlit=FUN_0044f770,
SpriteBatchFlush=FUN_0044f5c0, AsciiPrint=FUN_00402060 (mgr 0x0134ce18) —
`docs/th07_hud_sprite_system.md`. BulletMgrUpdate=FUN_00425a50,
BulletMgrDraw=FUN_00426f60, BulletSpawn=FUN_00423730 (mgr 0x0062f958, array
+0xb8c0, stride 0xd68) — `docs/th07_bullet_system.md`. EnemyUpdate=FUN_00420620
(enemy array in the 0x954xxx globals; pos +0x2b0c, box +0x2b3c/+0x2b48, state
byte +0x2e29 [bit0 alive, bit1 collidable, bit3 invuln, bit4 takes-damage,
bit6 boss], multi-hitbox +0x4f30/+0x4f34, per-frame shot-damage via
FUN_0043d9e0 @12822 — see `docs/th07_boss_hp_scaling.md`; full enemy struct map
is a good next RE target, only entry points pinned so far).

### 5n — per-player character select OVER THE WIRE (2026-06-15)
Made P2 pick its OWN character under netplay (was: clones P1). Root issue: the UI
merge fuses both players (`MergeKeys` UI mode returns `self|rcv`), so the menu FSM
couldn't tell P1 from P2. Added `Nc_GetLastSplit(&p1,&p2)` to the netcode: `GetKeys`
now stashes the frame's two raw words de-merged to identity (`g_last_p1` = host's,
`g_last_p2` = guest's — same on both machines). `HookedSceneTick` caches them into
`s_netP1Menu/s_netP2Menu`; `HookedMenuDispatch` no longer bypasses under netplay —
P1's pass drives the menu with `s_netP1Menu` (isolated), P2's pass with
`s_netP2Menu`, so the existing two-pass FSM runs deterministically on both sides.
Title/difficulty still use the UI-union (host-led). The in-stage different-char
machinery (anm overlay, target block, etc.) is unchanged — it just now gets a real
P2 selection. Build: 32-bit OK; all 16 netcode self-tests pass. NOT yet 2-PC tested.
Files: `netcode.cpp/.hpp`, `netcode_c_api.{h,cpp}`, `coop.c` (`HookedMenuDispatch`,
`HookedSceneTick`). See §8e.

### 5o — connection handshake / auto-sync + seed push (2026-06-15)
The bring-up was optimistic (connect immediately; both inis had to carry the same
`seed`/`delay`). Ported the EoSD `ConnectionUI` handshake HEADLESSLY into the netcode:
`Netcode_BeginHandshake(delay,seed)` arms it; `Netcode_PumpHandshake()` (called every
front-end frame from `HookedSceneTick` while `s_netStarted && !s_netActive`) sends
PING(`Ctrl_Set_InitSetting`)/answers PONG until the peer responds, then flips the link
live. The host's `delay`+`seed` ride in `init_setting`; the guest adopts them (its own
ignored). `MULTI_NET_VER` mismatch is latched + logged. Auto-retries, so launch order
is free. On a later mid-game drop the pump does NOT restart (stays local — reconnect is
future work). Verified over real UDP loopback: `tests/handshake_test.cpp` (host+guest
connect; guest seed 0x9999 → host's 0x1234). All 16 netcode unit tests still pass.
Files: `netcode.{cpp,hpp}`, `netcode_c_api.{h,cpp}`, `coop.c` (`StartNet`,
`HookedSceneTick`), `tests/handshake_test.cpp`. See §8e.

## 8. Goals / TODO (next sessions)

Ordered by the user's priority. Items marked **[autonomous-ok]** can be drafted
without live testing (RE + implement + document for the user to verify); items
marked **[needs user]** require the user's eyes in-game before they're "done".

### 8a — P2 proper, P1-like HUD  **[DRAFTED 2026-06-14 (§5h, PR #2) — needs in-game confirm]**
User spec (2026-06-14): give P2 a HUD that mirrors P1's, not the current ascii
`P2xx Ln Bn Pn` line. Specifically:
- **Lives** drawn as the life ICON sprite × current count (like P1's life row),
  not a number.
- **Bombs** drawn as the bomb/spell ICON sprite × current count, same style.
- **Power** drawn as P1's power gauge/number, same widget.
- **Same layout as P1's HUD**, placed on P2's side: the LIFE counter row starts
  *just a bit below the current point-item counter* (the "点" / point-item line),
  NOT below the total score. Match P1's vertical rhythm from there down
  (lives row, then bombs row, then power).
- The existing **ghost / revive / share** text HUD is fine as-is — keep it,
  only replace the resource readout with the sprite-based one.

Implementation pointers (RE first, don't guess offsets):
- Find ZUN's HUD/sidebar draw (the score-manager draw — near
  `ScoreCherryDisplay=FUN_00427f22` and the score singleton `0x626270`/struct
  `0x626278`). Identify the life-icon and bomb-icon sprite ids + the per-icon
  x-step + the row Y of the life counter, and the power-gauge widget draw.
- Read P1's point-item counter ("点") Y so P2's life row can sit "just below" it.
- Draw P2's icons from `s_p2Lives` / `s_p2Bombs` / `s_p2Power` at a P2-side
  origin (mirror P1's column, or place under P1's stack — pick what fits the
  448-col sidebar `DrawCoopHud` already uses, then refine after the user looks).
- Keep it behind the existing `DrawCoopHud(p2)` call in `HookedDraw`.
- Caveat: drawing engine sprites from the DLL needs the right draw call +
  sprite ids; the icons live in the front/HUD anm (`front.anm`, slot 0x15, base
  0x600 — see the FUN_0044df90 table). Resolve the real ids from the decomp.

### 8b — Bomb-declaration portrait for a different-char P2  **[RESOLVED via SUPPRESSION 2026-06-15 (commit ec1955f); correct-face load still open]**
**Shipped fallback (user-accepted): hide the wrong face instead of loading P2's.**
A different-char P2's bomb declaration showed P1's face; rather than load P2's face
(the unsolved glyph problem below), we now HIDE just the face for a P2 declaration
and keep the spell name + bar + sound. `HookedDeclMake` (FUN_0042868d create, runs in
P2's update window) flags the live declaration as P2's via `s_inP2Update && s_p2AnmActive`;
`HookedDeclDraw` (FUN_0042c577) clears the face-block draw-gate bit `declBase+0x590c & 1`
(PCBdecomp 17300-17304) around the original and restores it — the name block (gated
separately at +0x66d4) still draws, P1 is never touched, no anm load/swap so zero §8b
corruption risk. Default on (`s_hideP2Portrait`). **Awaiting in-game confirm.** The
correct-face-for-P2 goal is still open below (low priority; the suppression covers the
visible bug).

2nd attempt (commit c7741e4) **regressed identically to round-13b and was reverted**
(f850120). User test (P1=ReimuA + P2=SakuyaA): at the P2 spawn, P1 renders as glyph
sprites, P2 is invisible (still grazeable), and P1 shooting crashes. **What this rules
in/out:** the 2nd attempt FIXED two suspected round-13b causes — it used the CLEAN
stage-start load (not mid-stage) and the CORRECT draw fn `FUN_0042c577` (@0x42c577,
PCBdecomp 17301; NOT the `0x42b603` round-13b hooked) — and it STILL corrupted the
0x400 player table. So the fault is in the face-anm **LOAD itself**, not the swap or
the draw window. The player-anm overlay loads ONE extra anm (`player0N.anm` at base
0x400) and is fine; adding a SECOND extra anm (`face_{rm,mr,sk}00.anm` at base 0x4a0,
id window [0x4a0,0x4ad)) corrupts 0x400 even though the two id windows are disjoint
and both diff-capture+restore their windows. ⇒ the corruption is in **shared global
state the per-id table restore doesn't cover**.

**EoSD-decomp consult (2026-06-15, `d:/PCB Co-op/th06_multi_net`, per user request):**
The th06 netplay fork solves per-player portraits cleanly — P2 gets a FULLY PARALLEL
anm namespace: separate file slots (`ANM_FILE_FACE_CHARA_A2`=48..53), separate id
range (`ANM_OFFSET_PLAYER_DIFFERENCE` = `ANM_OFFSET_PLAYER2(0x719) - ANM_OFFSET_PLAYER(0x400)`
= 0x319, so P2 face at 0x4a0+0x319=0x7b9), separate face anm *files*, all loaded ONCE
at GUI init (`Gui::ActualAddedCallback`) keyed on `g_GameManager.character` (P1) +
`.character2` (P2) — never per-bomb. `BombData` then draws with the P1 vs P2 script id
(`ANM_SCRIPT_FACE_BOMB_PORTRAIT` vs `…PORTRAIT2`). **But that requires recompiling the
engine to ENLARGE the fixed arrays** (more file slots, id space up to 0x9ff already
tight) — not available to a binary DLL mod. The swap technique is the binary-feasible
equivalent and is already PROVEN by the player overlay.

**CORRECTION (disproves the old hypothesis): PCB textures are PER-SLOT, not a shared
surface pool.** `FUN_0044e070` (the per-entry registrar, PCBdecomp 32841-32847) stores
the loaded texture object at `mgr+0x282ac + slot*4` and `mgr+0x28acc + slot*4`, keyed by
the **anm FILE slot number** — NOT by an absolute surface index from the anm header
(that was the th06 `surfaces[32]` model; PCB differs). So loading the face into its own
free slot CANNOT clobber the player's texture. **The "per-slot texture array" guess in
the old note is WRONG.** What the load/free DO touch beyond the id tables: a CHAINED anm
(sub-files linked by `+0x38`) consumes CONSECUTIVE slots at rising bases (`FUN_0044df90`
outer loop 32762-32772, per-slot span at `mgr+0x2def8+slot*0xc`); the free `FUN_0044e4e0`
RECURSIVELY frees slot+1.. up to that span and resets global sprite caches at
`mgr+0x2e4cc/0x2e4d0..2` (32945-32948).

**Where this leaves it:** the player overlay uses the IDENTICAL recipe (base into a free
non-engine slot, diff-capture+restore the id window, swap around P2's draw) and works;
static analysis does NOT explain why the face overlay glyphs 0x400 at spawn. ⇒ **the next
attempt must be INSTRUMENTED, not blind.** Log, around `LoadP2FaceAnm`: (1) the face's
consumed slot count + span `mgr+0x2def8+faceslot*0xc` — does `face_*00.anm` CHAIN past
[0x4a0,0x4ad)? if so the capture window is too small and ids above 0x4ad are overwritten-
not-restored; (2) the player sprite/script/rev entries for 0x400..0x404 immediately
before vs after the face load — catch exactly who writes 0x400; (3) the actual id span
the face defines (engine loads it at slot 0x19 base 0x4a0, next engine load is far away
at 0x61e, so faces have room to be wider than 13 ids). **Reusable findings (keep):**
portrait = `data/face_{rm,mr,sk}00.anm`, engine-loaded via `FUN_0044df90(0x19,…,0x4a0)`
by GLOBAL char id `DAT_0062f645`; player face id window starts 0x4a0; created by
`FUN_0042868d` (bomb cb, sets sprite + UV at set-time, sets `DAT_00575ab4`); drawn by
`FUN_0042c577`; ownership attributable by hooking `FUN_0042868d`. LOW priority / polish;
needs live iteration — do not retry blind.

### 8c — SakuyaA cross-char per-player aim source  **[IMPLEMENTED + USER-CONFIRMED 2026-06-15 (§5j, commit 1789e0e)]**
SakuyaA's aimed shot used an aim target filled by the enemy update keyed off the
GLOBAL char id, only into static P1's block; P2 mirrored it. Now `BuildP2TargetBlock`
(handoff §5j) computes P2's homing AND aim target relative to P2's own position,
gating the SakuyaA cone on P2's own character (`s_p2Sel`) rather than the global —
so it is per-player both same-char and cross-char (P1≠Sakuya). The same change also
gives ReimuA per-player homing. **User-confirmed working in-game (2026-06-15)** with
P1=ReimuA + P2=SakuyaA after the portrait revert unmasked it.

### 8d — "P2 SELECT" on-screen prompt  **[DONE 2026-06-14 (§5h, PR #2) — visual tune pending]**
During P2's menu pass there's no visual cue it's P2's turn. Add a prompt via the
ascii text queue (the same `ADDR_ASCII_PRINT` the coop HUD uses) while
`s_coopMenu` is `CM_P2_CHAR`/`CM_P2_SHOT`. Cosmetic, safe. SHIPPED: shows
`P2 SELECT CHARACTER` / `P2 SELECT SHOT` at `MENU_PROMPT_X/Y`; verify the
position looks right (and isn't behind menu art) and tune the #defines.

### 8e — Netcode → coop.c wiring  **[GAMEPLAY + PER-PLAYER SELECT DONE 2026-06-15; needs 2-PC test]**
The original long-term goal. **Gameplay lockstep is wired** (§5k): the netcode
core links into `th07_coop.dll`, `FUN_00437c70` injects the merged word into
`g_InputMenu` every logic frame, P2's input comes from the merged high bits, and
`FUN_00442c60` syncs the seed. Behind `coop.ini [net] enabled=1`, default off.

**Per-player character select over the wire — DONE (§5n).** The UI merge fuses both
players into one word (`MergeKeys` UI mode = `self|rcv`), which is why the first cut
cloned P1. Fix: the netcode now stashes the frame's two RAW words de-merged to
player identity (P1 = host's, P2 = guest's) and exposes them via
`Nc_GetLastSplit(&p1,&p2)` (`netcode.cpp` `g_last_p1/p2`, set in `GetKeys`; same on
both machines). `HookedSceneTick` stashes them into `s_netP1Menu/s_netP2Menu`, and
`HookedMenuDispatch` now RUNS the two-pass FSM under netplay: P1's pass overrides
`g_InputMenu` with `s_netP1Menu` (isolated from P2), P2's pass with `s_netP2Menu`.
Both machines run the identical deterministic FSM → each player picks its own
char/type. Title/difficulty still navigate together (UI-union). Compile + all 16
netcode self-tests pass; **needs a two-machine network test** (see NETPLAY_TEST.md).

**Connection handshake / auto-sync — DONE (§5o).** Replaced the optimistic
immediate-connect with a headless port of the EoSD `ConnectionUI` PING/PONG +
`Ctrl_Set_InitSetting` flow: `Netcode_BeginHandshake`/`PumpHandshake` (netcode.cpp),
exposed as `Nc_BeginHandshake`/`Nc_PumpHandshake`/`Nc_HandshakeVersionBad`.
`StartNet` now arms the handshake (no immediate connect); `HookedSceneTick` pumps it
each front-end frame and flips `s_netActive` on only when the peer answers. The HOST
pushes its `delay`+`seed` over the wire and the guest ADOPTS them (guest's ini values
ignored) — no more hand-matching, and launch order is free (it retries until the peer
appears). Version mismatch (`MULTI_NET_VER`) is detected and refused. **Verified over
real UDP loopback** (`tests/handshake_test.cpp`: host+guest connect, guest's 0x9999
seed correctly overridden to the host's 0x1234).

**Still open:** the two-machine in-game test; optional host seed randomization per run.

**Launcher + beta drop — DONE (§8g).** `src/launcher/launcher.c` is a small Win32
GUI (modelled on EoSD's `ConnectionUI`) that replaces hand-editing `coop.ini` +
`injector.exe` for end users: fields for Host IP / Port / Listen port / input delay
+ a fade checkbox, and three buttons — **Host Game / Connect / Local Co-op**. Each
writes the matching `coop.ini` next to the DLL, then injects `th07_coop.dll` into
`th07.exe` (same suspended-launch + remote-`LoadLibrary` recipe as `injector.c`) and
exits. It is a config-and-launch front-end, NOT a live socket — the DLL's headless
handshake still does the actual connecting at the title, which keeps the one UDP
socket inside the game process. The **host mints a fresh random seed per launch**
(pushed to the guest), so each run plays differently. `proximity_fade` now defaults
**ON**. The launcher remembers the `th07.exe` path in `coop.ini [launcher] exe_path`
(prefilled next run; falls back to one sitting next to itself). Beta drop =
`launcher.exe` + `th07_coop.dll` + `coop.ini` + `README.txt`, assembled by
`package_release.ps1` into `build/release/PCB-Coop-beta.zip`.

NOTE on "launch then connect": the launcher launches the game immediately and the
DLL's headless handshake connects at the title — the live UDP socket must live in the
game process, so an EoSD-style "wait for connect, then Start Game" launcher can't hand
the live connection across to the game. Connection state is surfaced in-game (the NET
overlay) + `coop_log.txt` instead.

**Demo / attract-mode disabled — DONE (§8i).** The title-menu update (~`0x004559xx`)
counts idle frames in `menuObj+0xd100` and at `900 < idleFrames` loads
`data/demo/demorpyN.rpy` — which advances state + RNG and, under netplay, would fire
while a player idles at the title waiting for the peer. `PatchDisableDemo()` (coop.c,
at attach, gated on `[coop] disable_demo`, default ON) raises the threshold to
`0x7FFFFFF8`: it byte-patches the `imm32` of `CMP [EAX+0xd100],0x384`
(`81 b8 00 d1 00 00 84 03 00 00` at **0x00455a94**), so the imm32 is at
**0x00455a9a** (0x00455a94+6), guarded by a read-back that the bytes still equal 900
so a wrong build is left untouched. (Same approach as EoSD's mod.)

> **Fix 2026-06-15:** the first cut used `0x00455a9c`, which is +2 past the imm32 —
> it lands on the imm's high half plus the following `0f 8e` JLE, so the guard read
> back `0x8e0f0000` and (correctly) skipped, leaving the demo live. Corrected to
> `0x00455a9a` after re-scanning th07.exe: the CMP starts at `0x00455a94`, not `…96`
> (the `00 d1…` the scan matched is the disp32, two bytes into the instruction).

**Live sync telemetry — DONE (§8h).** First 2-PC test desynced (frame-4 "DESYNC"
flapping, lying "back in sync" at mismatched frames on each side, then 1 fps →
`peer lost` at the menu→stage transition). Added instrumentation:
`Nc_GetSyncStats(selfRng, rcvRng, waitMs)` + `Nc_GetNetFrame` expose the RNG pair the
oracle compares and the per-frame lockstep wait (the direct read on a stall).
`HookedSceneTick` now (a) only judges desync **in-stage** (menus aren't RNG-locked —
that was the false frame-4 alarm), (b) requires a sustained 120-frame run before
declaring recovery (no more lying resyncs), (c) logs a throttled `STALL` line when a
frame blocks ≥250 ms and a ~2s heartbeat with the seed pair, (d) draws an EoSD-style
`NET H F#### d# SYNC / WAIT###ms / LOST` line top-right via `DrawCoopHud`.

**Root cause of the desync (diagnosed).** Two facts from the logs: the menus held
together (both reached stage select) but the per-frame seeds never matched ("DESYNC
at frame 4" onward), and at the **menu→stage transition** the link hit 1 fps then
`peer lost` at *different* frames on each side (host 330 vs guest 468 — a 138-frame
gap). A 138-frame split is impossible while both sides keep calling the blocking
lockstep (it pins them within `delay` of each other), so the clocks had **drifted
apart at the stage load**: our port used a **free-running** frame index that, unlike
the reference, *never reset at a scene boundary*. Each machine burns a different
number of front-end ticks loading the stage, so the two `s_netFrame` counters came
out of the load permanently offset → each waits for a frame the other already passed
→ 5 s timeout → drop. (The seed mismatch in the *menus* is cosmetic; menus aren't
RNG-locked.)

**Start barrier — DONE (§8j).** The th06 reference's lockstep clock is the game's own
per-scene `calcCount`, which the engine **resets at every scene boundary**, and
`Supervisor::OnUpdate` clears the peer buffer there (the `last_frame_a > frame_a`
block). Ported faithfully: `HookedSceneTick` reads PCB's top-level scene id at
`self+0x154` (the field the engine's own `"scene %d -> %d"` log prints) and, when it
changes, re-zeros `s_netFrame` and calls `Nc_Reset()` (clears the peer's stale
per-frame inputs + frame index — exactly th06's reset, no new netcode needed). So
**every scene — and crucially the stage — begins lockstep frame 0 on both machines**,
discarding the per-scene load-tick asymmetry. Both commit the menu→stage transition
on the same input-driven logic-frame (input drives control flow; only RNG was out of
step), so the re-zero lands together; the shared seed is forced by `HookedGameStart`
as the stage inits. The link-up path anchors `s_netSceneId` to the connect screen so
the first active frame doesn't fire a spurious re-zero. The `NET _ F####` overlay
frame now restarts per scene — in-stage, two machines showing close F numbers ⇒
aligned; a large split ⇒ desync. **Needs the 2-PC test to confirm.** If a *mid-stage*
desync still appears (dropped-packet RNG split, not a boundary), the next lever is the
th06 host-driven `Ctrl_Try_Resync` (infra already in `netcode.cpp`, not yet driven):
host schedules a realign `delay*2+2` frames out and both reset RNG + clear rcv there.

### §8k — Co-op replays

**What's already free.** PCB's replay *record* fn `FUN_00442cd0` stores the full
**16-bit** `g_InputMenu` word per frame ([PCBdecomp.c:27610]). Our merge writes P1
(low 9) **and P2 (high 7)** into that exact word, so **every co-op .rpy already
contains P2's complete per-frame input stream** — vanilla only ever used the low 9
bits, leaving the high 7 as free space we now populate. In netplay both machines
record the identical merged stream.

**What was missing → P2 character tag — DONE.** The vanilla header has one player
slot; P2's *character pick* wasn't stored. `HookedReplayHdrInit` (hook on the header
builder `FUN_00443040`) now writes a 4-byte magic block `C2 07 <p2sel> <diffchar>`
into the header's **0x58–0x5d gap**. Proven safe: the loader `FUN_004433b0` validates
only magic@0x00 + checksum@0x08, and `FUN_00443550` consumes only 0x56 (P1 char),
0x57 (stage), the 0x14–0x53 stage table, and 0x70–0xa7 — nothing in 0x58–0x5d (and no
date field lives there). The @0x08 checksum is summed by the *save* after our write,
so the file stays self-consistent and **vanilla PCB still loads it**. Only written
when co-op is actually in play (`s_p2Sel>=0 || s_netActive`), so a solo run made with
the DLL stays a byte-clean vanilla replay.

**Still missing → two-player PLAYBACK (planned, not built).** Watching a co-op replay
needs three more pieces, all building on the data now in the file: (1) read the
0x58 block back on load to recover P2's character; (2) recreate the P2 clone at
stage-start during playback (the menu/char-select is skipped on playback, so drive
the existing `SpawnP2` auto-spawn from the stored sel); (3) in playback mode source
P2's input from the replayed word's high bits (`g_InputGameplay`) instead of the
keyboard/wire — today [coop.c:2255] always reads live input. PCB exposes a
playback-mode flag (the `DAT_00626274+0x25` / `DAT_00575adc` bit the record/playback
fns gate on). Determinism is already on our side (live lockstep depends on it).
Until that lands, a co-op replay played back shows only P1 and (since P2's
world-effects aren't reproduced) desyncs — so **co-op replays are for archiving now,
watchable later**. Viewing will require the DLL loaded (vanilla can't render P2).

**Proximity fade** range tightened (§ misc): `PROX_FAR2` 96px→48px, `PROX_NEAR2`
24px→16px, so the other player only fades once the ~32px sprites actually overlap,
not from across the screen.

### §8l — LAN 2-PC test rounds + desync diagnosis (2026-06-15/16)

First real 2-machine tests (LAN). **The injector + DLL load fine on Linux** (unlike
the new mahjong game) and the two instances link up cleanly. Three test rounds, each
with a determinism trace (`det-trace`, `coop_trace_{host,guest}.csv` — one row per
logic frame; diff by frame/`readFrame` to pinpoint divergence). **Findings, in order:**

**1. STALE BUILD invalidated round 1.** The tester's DLL pre-dated the demo-disable
fix (`adb34b9`): its log printed `demo-play patch SKIPPED … 0x00455a9c` (the old
address), so the title attract-demo stayed live and consumed RNG → the menu/difficulty
desync (one machine on Easy, other on Lunatic). **Lesson: the attach banner now carries
a build tag; always confirm `demo-play disabled … 0x00455a9a` on both machines first.**

**2. The seed-force was firing EVERY frame (not game-start).** `FUN_00442c60`
(`HookedGameStart`) turned out to be a *per-frame* function (~15k `SEEDFORCE` lines /
15k frames), so forcing the seed there pinned the live RNG to `initSeed` every frame.
That (a) **caused Chen's nonspell to bug out** (all background orbs flew the same way —
a one-orb-per-frame "random" direction reads the same pinned seed each frame) and
(b) masked desyncs (both machines snap to `initSeed` ⇒ false SYNC). **Fixed:** force the
seed **once per scene** (`s_seedForced`, re-armed on each scene change), EoSD's
force-at-start semantics; ZUN's RNG then evolves naturally and lockstep holds it.

**3. The real desync is a guest→host INPUT-delivery fault (NOT seed/demo/frame-drift).**
Round 3 trace (seed-force fixed, oracle now honest) pinned it:
  - `readFrame` is identical on both every frame ⇒ **frame counters are aligned**
    (the §8j start-barrier works; not a clock-drift bug).
  - **P1 (host→guest) transports perfectly** for all 7314 frames.
  - **P2 (guest→host) drops on ~7 frames**: e.g. at `readFrame=1289` the guest sent
    `selfKey=0040` (left) but the host's `rcvKey=0000`, with `rcvStatus=0` (the host
    found a value *immediately* and never waited) — so it advanced on a **present-but-
    wrong** P2 slot. That 3-frame P2 drop seeds the divergence; the RNG counter splits
    ~40 frames later when the now-different P2 position changes a roll → P1/P2 die in a
    different order on each screen.
  - **Timing asymmetry:** host `rcvStatus==1` (waited) on 7110/7313 frames, guest on 0.
    The host runs a hair ahead and is almost always sub-ms-waiting for the lagging guest;
    the drops are the rare frames it read a stale slot instead of blocking.

**Seam map for the netcode internals** (added for this hunt, all in `netcode.cpp` /
exposed via `netcode_c_api.h`): `Nc_GetReadStats(readFrame, selfKey, rcvKey, rcvStatus)`
— `readFrame = netFrame - delay`; `rcvStatus` 0=immediate / 1=after-wait / 2=timeout.
The det-trace CSV carries these per frame.

**Round 4 (v3, provenance) — drop is on the SEND side, not reception.** Added per-slot
provenance: `Nc_GetRcvSrc(srcPktFrame, writes)` — which packet (its frame field) last
wrote the received slot, and how many packets wrote it (CSV cols `rcvSrc,rcvWrites`). The
trace pinned the drop hard: 7 drops at `readFrame 844–850`, **all `rcvSrc=850`** (a real
guest packet wrote them, never `-1` ⇒ NOT host-side staleness), yet the values the host
got (`0000`/`0001`) **disagree with the guest's own `self[]` readback (`0080`, the guest
held right)**. The host had **no guest packet newer than 850** while racing to netFrame
854 (host waits on 5751/5785 frames — well ahead), so it burst-read those stale slots.
⇒ **The guest is transmitting input values that disagree with its own buffer.** Two
mechanisms remain: the self slot being **rewritten** (same frame recorded twice, different
value) or **zero-filled** (recent slot missing at send time). v4 adds detectors for both:
`Nc_GetSendDiag(...)` + coop.c logs `SELF-REWRITE` / `SEND-ZFILL` the instant either fires.
**Next run decides it.** (Hypothesis to keep in mind: the merged word is written back to
`g_InputMenu` after `Nc_GetInputNet`, and `HookedSceneTick`/`FUN_00437c70` may fire >1×
per logic frame — a second fire would re-`readLocalInput()` the now-merged word and
re-record the same `s_netFrame`'s self slot with a different value. That is exactly what
`SELF-REWRITE` is built to catch.)

**Round 5 (v4 detectors) — buffer is CLEAN; the phantom is on the wire.** `SELF-REWRITE`
and `SEND-ZFILL` both stayed silent, ruling out a buffer rewrite or recent-slot zero-fill.
Same fault signature: host received `0001` (shoot) for `readFrame 1057` that the guest's
own `self[]` (and its own sim) read as `0000`. The host got **four consecutive `0001`s**
(packet 1060's `keys[0..3]` = `self[1057..1060]`), which looks like a genuine 4-frame held
bit — yet the guest's buffer shows none of it. By code, the guest's `SendKeys` and its own
`GetKeys` read the *same* `self[]` slot, so they cannot disagree → an assumption is wrong
somewhere only the wire bytes can settle. Symptom matches the user exactly: P2 "fired" and
killed a fairy on the host screen, not on the guest ("a fairy died for P2 not P1, neither
firing"). **v5 logs the wire:** `WIRE SEND frame=F key0=V` (a frame's own input as
transmitted) and `WIRE RECV pkt=P slot=S val=V` (first non-zero arrival), via a netcode log
callback (`Nc_SetLog`). **Decision rule next run:** if the guest logs `WIRE SEND
frame=1057…` then it really recorded+sent the bit and its *own* merge dropped it (a
read-side bug — instrument `GetKeys` self read); if the guest has NO such SEND but the host
logs `WIRE RECV …val=0001`, the bit was injected in transit/recv (serialization / a stray
packet — instrument `RcvPacks` / `ReceiveOnePack`).

**Gameplay bug backlog (separate from this desync):** the user's local-coop Parsec
sessions surfaced 11 gameplay bugs (2 crashes + resource/bomb/extend issues) — captured
in `docs/th07_coop_gameplay_bugs.md` for an unattended session. Not transport-related, but
note a **crash on one machine reads as `peer lost` on the other**, so the two crashes
(P1-Sakuya+P2-Reimu on hit; stage-4 death-fairy) are worth fixing regardless.

### §8m — DESYNC ROOT-CAUSED + FIXED: stale-scene packet contamination (2026-06-16)

**The v5 wire trace settled it. The "present-but-wrong P2 slot" of §8l is NOT a drop —
it is the previous scene's input bleeding into the stage through a reused frame index.**

Diagnosis from the friend's two-run logs (`coop_trace_{host,guest}.csv` + `coop_log.txt`,
host=P1 Reimu / guest=P2):
- Cross-checked, per read-frame `k`, `host.rcved[k]` (what the host thinks the guest sent)
  vs `guest.self[k]` (what the guest actually recorded). **Whole-stage totals:**
  - **Run 2: exactly 3 disagreements** — frames **536, 537, 538**, all `host=0001`,
    `guest=0000`. Host→guest direction 100% clean.
  - **Run 1: exactly 3 disagreements** — frames **698, 699, 700**, same shape; host→guest
    clean.
- **The disagreeing frame == the char-select length.** The single re-align log reads
  `scene 1 -> 2 — lockstep re-aligned to frame 0 (was 539)` (run 2) / `(was 701)` (run 1).
  Run 2's contamination is at 536–538, run 1's at 698–700 — i.e. the **last ~3 frames of
  the menu scene**, replayed into the stage at the **identically-numbered** stage frames.
- **The phantom value is `0001` = Z held in char-select.** The guest (P2) holds shot to
  confirm its character; `WIRE SEND key0=0001` runs through the whole menu. The v5 wire
  logs are CLEAN: guest `WIRE SEND frame=538 key0=0001` and host `WIRE RECV slot=538
  val=0001` agree. The provenance columns nail it: all three contaminated host slots have
  `rcvSrc=538` (last written by guest **packet frame 538** — a *menu* packet, since the
  menu ended at 539) with `writes` 3/2/1 for slots 536/537/538 — the exact trailing-batch
  signature of `keys[0..2]` of the last menu packet.

**Mechanism.** The §8j start-barrier resets the netcode frame index to 0 at every scene
boundary (needed so the two machines realign after the asymmetric menu/stage load). That
makes frame numbers **reused** across scenes. `Nc_Reset()` clears the *local* maps, but it
cannot un-send the guest's in-flight/last-window menu packets. Those packets carry frames
≈524–539 with value `0001`; arriving just after the host's reset, they write the **stage**'s
`rcved[524..539]=0001`. Lower slots get overwritten in time by fresh stage packets, but the
slots at the menu's very end (the last ~3) are read by the slightly-leading host in the
narrow race **before** the lagging guest reaches those stage frames and sends the real
`0000` — so the host injects ~3 phantom P2 shots. Those shots kill a fairy on the host
screen only, change an item-drop roll, and the **shared RNG counter splits** (host ctr=0 /
guest ctr=4 at the first diverged frame) — exactly the user's "fairies spawn/drop
differently" + "a shot fires on one screen not the other." **Run 1 vs run 2 are the SAME
bug**; it just lands at char-select-length frames in, so a fast select desyncs "from the
start" and a slow one desyncs "after Letty's first spell."

**FIX (committed): scene-generation (epoch) tag on the wire.** `Pack` gains a `unsigned int
epoch`; `g_epoch++` in `Netcode_Reset()` (so it bumps with the per-scene re-zero, in
lockstep on both machines — both reset on the same scene-id change at `self+0x154`).
`SendKeys`/`HsSend` stamp the current epoch; `RcvPacks` **drops any `Ctrl_Key` packet with
`epoch < g_epoch`** (a previous generation). Newer epochs are accepted (a peer that crossed
the boundary a frame ahead) and cleared by our own imminent reset. `MULTI_NET_VER`
3950→3960 so a mixed-build pair refuses to connect (the handshake already gates on ver).
A deduped `EPOCH DROP …` line logs each stale generation dropped, for the next test's
confirmation. Builds clean (all 6 targets, 32-bit); native merge test 0 failures.
**Needs the 2-PC re-test:** expect the trace to show **0** host↔guest input disagreements
across the whole stage and the RNG counter to stay locked through Letty and beyond.

> Note (unchanged, separate): the P1-Sakuya+P2-Reimu crash did NOT reproduce in this build
> (both reached Cirno). It was last seen on the **Phantasm** stage locally, not main-game
> stage 1 — still tracked in `docs/th07_coop_gameplay_bugs.md`, unrelated to this transport
> fix.

### §8n — epoch fix CONFIRMED for transport; RESIDUAL desync is SIM-side (2026-06-16)

The 2-PC re-test of the epoch build (delay=2; guest logged `EPOCH DROP` ×3 — the fix is
live and catching the stale menu packets). **The input transport is now clean:** diffing
the two stage traces shows **zero P2-input disagreements** (`host.rcvKey == guest.selfKey`
every frame) and **`merged` identical on both** at the first divergence. §8m's contamination
is gone.

**But the run still desynced** (user reached Cirno with different power on each machine).
The det-trace pins a *new, different* class of bug:
- Frames are **byte-identical** (same seed AND counter) up to **netFrame 625**, then at
  **626** — with **identical merged input `0081`**, P2 idle, P2 power 0 on both — the guest
  makes **24 more RNG calls** than the host (host ctr 64 → seed `727e`; guest ctr 88 → seed
  `bb83`). So **identical input now produces different RNG consumption** ⇒ a *simulation*
  determinism divergence, not transport.
- Vanilla PCB is cross-machine deterministic (its replays are), so the cause is in **our
  additions** — overwhelmingly the **grafted P2 entity** (the one non-vanilla sim object).
  Two corroborating signals: the divergence is byte-identical for 625 frames then tips at a
  discrete event (a slow-accumulating float/state difference crossing an RNG-gated branch,
  e.g. an item-drop `Rng_Next32()%100<=cherryRatio` roll), and the P2 clone lives at
  **different addresses** on the two machines (host `res=0x029e79d0` vs guest `0x001e1338`)
  — any logic keyed on that pointer/order diverges.
- **Strong mechanism lead: the x87 ST0 / FPU hook hazard** documented in
  `docs/th07_coop_gameplay_bugs.md` (nightshift). Several ZUN resource/damage/item fns read
  values off the x87 stack the *caller* pushed; a non-FP-safe C hook (`HookedDamage` on
  `FUN_0043d9e0`, the collect/item hooks) can leave the x87 stack/control word perturbed,
  making ZUN's FP — hence damage/positions/item rolls — diverge.

**Diagnostic shipped (v6): `coop.ini [coop] suppress_p2=1`** (gates `SpawnP2`, auto + F9).
Run a 2-PC test with `suppress_p2=1` on BOTH machines (P1-only co-op-less netplay):
- **Counter stays locked the whole stage ⇒ the P2 entity graft is the determinism culprit**
  → next: audit P2's update for FP-stack safety (wrap hooks with `fnsave`/`frstor` or
  `_control87` save/restore; verify no hook leaves ST0 pushed) and for any address/pointer-
  ordered logic; bisect by selectively disabling P2 sub-systems (shot, collision, aim).
- **Still desyncs without P2 ⇒ the cause is a hook that runs even without P2** (e.g. the
  damage/seed hooks themselves) — instrument those.

This is the current frontier: transport is solved, the remaining desync is sim-determinism
from the P2 graft (most likely an x87/FP-safety or pointer-order issue).

### §8o — suppress_p2 test: P2 graft RULED OUT; residual desync is CROSS-PLATFORM FP (2026-06-17)

The §8n hypothesis ("the P2 graft is the determinism culprit") is **DISPROVEN.** A clean
2-PC run with **`suppress_p2=1` confirmed on both machines** (both logs: `spawn: SUPPRESSED`,
CONFIG echo `suppress_p2=1`) — i.e. **no P2 entity exists at all** — *still desynced*
(fairies, item drops, death-[P] pattern, then a hard desync at the stage-1 boss from
divergent power states).

**What the traces show (decisive):**
- Both machines are **bit-identical** — same `seed` AND `counter` every frame — from stage
  frame 2 (where the per-scene seed force lands, both → `2915`) through **frame 742**. 740
  frames (~12.4 s) of perfect lockstep.
- At **stage frame 743**, with **identical inputs** (P2 suppressed, P1 word identical) and
  identical RNG state through 742, the **guest makes +24 RNG calls** (host ctr 84 → seed
  `271b`; guest ctr 108 → seed `b1b3`). From there they cascade apart.
- The earlier `DESYNC in stage at frame 4` log line is a **false alarm** — it compares the
  frame-1 *pre-force* seeds (`93b3`/`f880`), before the stage-start force applies at frame 2.
  Ignore it; the real divergence is frame 743 (and it varies by run — §8n saw 626).

**Why this is NOT our mod:**
- **coop never calls the game RNG** (it only *reads* `0x0049fe20/24` for the trace). So the
  +24 calls are entirely inside **ZUN's own code** taking a different branch.
- **P2 is suppressed** ⇒ every P2-gated path is off; `HookedAngleToPlayer` is a pure
  passthrough when `s_p2==NULL` (coop.c:2932); no coop hook writes game state or calls RNG.
- Identical inputs + identical RNG + same code diverging ⇒ the only remaining variable is a
  **floating-point result differing between the two machines.**

**The machines are different platforms:** host = `Z:\home\muddy\...` (**Wine/Linux**), guest
= `C:\game\Touhou\...` (**native Windows**). 740 bit-perfect frames then a sudden branch
divergence is the textbook signature of **subtle x87 FP nondeterminism** — accumulated
last-bit differences (almost certainly **CPU-vendor-dependent transcendentals**: `fsin`/
`fcos`/`fpatan`, used by PCB for movement/aim) crossing a decision threshold. A gross
mismatch (FPU precision/rounding control word) is ruled out — it would diverge at frame 2,
not 742.

**Implication:** delay-based lockstep requires *bit-identical* simulation. If the two
machines' x87 math isn't bit-identical, **no fix in our code can hold sync** — the divergence
is in ZUN's own arithmetic. This is exactly what EoSD's periodic resync window papers over
(see the resync discussion); pure lockstep does not.

**Next (confirmation, cheap):**
1. **Same-platform / same-CPU-vendor test** — two native-Windows boxes (ideally same vendor),
   or two Wine boxes on the same CPU. Expectation: stays synced ⇒ confirms it's the
   Wine-vs-Windows / Intel-vs-AMD FP gap.
2. **Vanilla replay cross-test** (no mod): record a PCB replay on machine A, play it on B. If
   the *vanilla* replay visibly desyncs, PCB itself isn't cross-platform deterministic between
   these two machines — proves the cause is the platform, not netplay.
3. Capture both machines' **CPU vendor/model + OS** (Wine version on the Linux side).

**If confirmed cross-platform FP — strategic options (all heavier than a code fix):**
- **Constrain deployment:** document that lockstep needs matching FP (same CPU vendor / both
  native or both Wine). Lowest effort; works today between like machines.
- **Periodic state resync** (EoSD-style): host authoritatively pushes RNG + critical sim state
  every N frames; guest snaps to it. Hides small divergences but needs real state transfer
  (enemies/bullets) to avoid visible rubber-banding ⇒ approaches rollback complexity.
- **Deterministic FP shim:** force identical x87 control word (likely already matching) and
  replace the divergent transcendentals with a bit-identical software `fsin/fcos/fpatan` on
  both machines (hook the math or the callers). Most invasive; only route to true
  cross-CPU lockstep.

### §8p — user reports vanilla replays DO sync cross-OS ⇒ suspect OUR netcode's FPU use (2026-06-17)

User correction: he has received working PCB replays from the Wine-host friend many times, so
**vanilla PCB is deterministic across these two machines** — the base-game-FP hypothesis (§8o)
is wrong. Re-frame: the base game is fine; **our netplay additions introduce the divergence**,
and they must do so in a way that *differs per machine* (else both machines would just change
identically and stay in sync with each other). Vanilla and replays have **no netcode running**,
which is exactly why they're immune.

Prime suspect (P2 already ruled out): the **per-frame netcode perturbing the shared x87 FPU**.
`Nc_GetInputNet` (called from `HookedSceneTick` right before ZUN's player update) runs the
lockstep wait — double timing math busy-spinning a **machine-dependent number of times**
(network latency / scheduling differ). If that leaves the x87 control word, rounding, or stack
depth even slightly different on the two machines, ZUN's subsequent FP forks. Fits the trace:
bit-identical for ~740 frames, then a sudden branch (a slow-creeping float crossing an RNG-gated
threshold), and the fork frame varies by run (626 / 743) like a timing-sensitive effect.

**v8 ships the probe + a candidate fix:**
- **Probe:** trace gains `fpucw,fpusw` columns — the x87 control + status words read right
  AFTER `Nc_GetInputNet` each frame. Diff host vs guest: if they diverge, the netcode is
  perturbing the FPU (status-word TOP field = stack depth catches stack leaks; control word
  catches precision/rounding).
- **Fix (gated `coop.ini [coop] fpu_guard=1`, launcher checkbox):** `fnsave`/`frstor` the full
  x87 state around the netcode call — a firewall so the netcode can't touch ZUN's FPU at all.

Test plan: round 1 `fpu_guard=0` (observe `fpucw/fpusw` diverge at/around the fork frame) →
round 2 `fpu_guard=1` (should hold sync). If it still desyncs with the guard AND fpucw/fpusw
are identical host-vs-guest throughout, the cause is NOT the FPU and the search moves to other
per-machine inputs our code might feed the sim (pointer/address-ordered logic, uninitialised
reads) — though P2-suppressed leaves very little of that surface.

### §8q — fpu_guard gave ONE perfect run but desync is INTERMITTENT ⇒ D3D x87 precision; PIN the control word (2026-06-17)

**Result of the v8 test.** With `suppress_p2=1, fpu_guard=1` the main pair (Ryzen 5800X ↔
i7-2620M, one on Wine) ran **18,766 stage frames bit-perfect** (seed AND counter identical the
whole stage, to a Chen death) — `diverging frames: 0`. Proof the netcode WAS a real source and
the firewall removes it. **BUT** the user reports that was the *only* time it worked: all
further runs "back to Desync City, with or without P2." A second old-laptop pair (i7-2620M ↔ i5,
Win7↔8.1) desynced with `fpu_guard` on AND off — its trace shows the x87 **status word differing
host (`0020`) vs guest (`0000`)** at the fork (frame 936), i.e. the two CPUs handling identical
FP ops differently.

**The intermittency is the key clue** (the desync fork frame varies: 626 / 743 / 936 / never).
A deterministic cause (fixed CPU-transcendental gap) would fork at the SAME frame every run.
Varying ⇒ a per-run variable. The strongest fit, and the correct read (caught by nightshift's
keen-ramanujan branch, commit 8d09974): **the x87 PRECISION control word set by Direct3D8.**
ZUN omits `D3DCREATE_FPU_PRESERVE`, so D3D8 sets 24-bit single precision at device-create and can
RESET it on `Present()`. Two machines' D3D drivers/wrappers (Wine vs Windows, different GPUs)
can leave it in DIFFERENT states (24- vs 53-bit) — so whether they match **varies run-to-run**:
a clean run when they happen to agree, desync when they don't. This also corrects §8o (the base
game IS cross-deterministic — vanilla replays sync; the user confirmed receiving working replays
many times).

Why `fpu_guard` didn't fix it: `fnsave`/`frstor` faithfully *preserves* whatever control word
the game already had — if D3D left it at 53-bit on one machine, the guard keeps it 53-bit. The
guard removes the NETCODE's stack churn (a second, real source — hence the one clean run when the
control words happened to match) but cannot equalise a D3D-induced precision mismatch.

**v9 fix (ported from the branch, adapted): `PinFpuForNetplay()`** — `_controlfp(_PC_24 |
_RC_NEAR, _MCW_PC | _MCW_RC)` once per logic frame in `HookedSceneTick` under `s_netActive`, so
BOTH machines run ZUN's sim at vanilla's 24-bit/round-nearest regardless of what D3D did. It logs
the PREVIOUS control word on the first pin — **if the host and guest "was 0x____" values differ
in the next 2-PC test, that was the desync, confirmed.** `fpu_guard` stays on by default as a
complement (it covers x87 *stack* state, which the CW pin does not). Gated on netplay only, so
local/replay is byte-identical.

**Branch housekeeping:** `keen-ramanujan-6unds2` (B2+B4) is fully redundant with main. From
`keen-ramanujan-mrh2n7` only two things were new and were ported to main manually (not merged —
the branch's B2/B4/D1/§8n duplicate main in conflicting form): the **CW pin** above and **B5**
(P2 bomb → Extra/Phantasm boss invuln, using main's validated `ADDR_DIFFICULTY 0x626280`, not the
branch's `0x62f85c`). Both branches can be deleted once this is pushed.

**Next test:** `suppress_p2=1` (keep P2 out to stay minimal), `fpu_guard=1`, run 2-3 stages.
Read the `control word pinned (was 0x__)` line on each machine — differing values confirm the
mechanism. If it now holds sync across several runs, re-enable P2 (`suppress_p2=0`) for the real
full-co-op test.

### Working-build discipline for the overnight session
The build on `main` is GOOD (menu select + different char + lasers + bombs +
retry all confirmed by the user). Before any risky change, note the last-good
commit. Prefer 8a (HUD) and 8d (prompt) which are additive/cosmetic. Do NOT
refactor the anm swap or the menu FSM. Build with `powershell -File build.ps1`
and confirm `th07_coop.dll` stays 32-bit (machine 0x014C). Commit incrementally
with clear messages; leave the tree clean. Anything needing live verification:
implement + document what to test, don't mark it done.

### 5p — Gameplay-bug backlog pass (2026-06-16, unattended; PR #5)
Worked `docs/th07_coop_gameplay_bugs.md` (local-co-op bug list). No game this
session — fixes are reasoned + compile-verified (6 targets, 32-bit; native merge
test green; CI green), NOT play-tested. **Branch `claude/quirky-heisenberg-znrauh`,
PR #5.** Full per-bug detail (with the RE) lives in the bugs doc; summary:

- **D1 — no free revives** (DONE): `ReviveByGraze` now needs a spare extend; the
  last-life-death 1up drop is kept (user-confirmed).
- **B1 — point-item extend shared** (DONE): mirrored to the partner via the
  extend-tier counter `res+0x2c` (the 1up *item* doesn't bump it), through the
  existing collect-time field-swap. FPU-safe.
- **B3 — P2 autocollect** (DONE): `ApplyP2Autocollect` replicates ZUN's P1-only
  line-cross / Extra trigger for P2 (sets item homing-mode `+0x27f=1` for items P2
  is nearer to; existing nearer-player homing carries them in).
- **B4 — P2 bomb clears bullets** (DONE): bomb clear = per-player region array
  `player+0x17dc` (tested by `FUN_0043e0a0` inside the bullet hit test coop already
  re-invokes for P2). `HookedAddClearCircle/Box` force ECX→P2 in P2's update window
  so P2's bomb fills `P2+0x17dc`. Mechanism now mapped in `th07_player_struct.md`.
- **B2 — full-power→cherry gate** (DEFERRED): blocked by a **new hazard** —
  `FUN_004326f0` (item spawner) and `FUN_0042d83a` (extend) read the x87 **ST0** their
  caller pushed, so a MinHook C detour can't wrap-and-forward them. Routes documented.
- **B5 — boss invul-during-spell for P2 bomb** (DEFERRED): `+0x1fbac` is the BOSS
  spell index (`FUN_00429a4f`); `DAT_012fe0c8` = boss-spell-active. The real gate is
  in the boss-damage path and P2's bomb-damage route isn't confirmed to pass through
  `FUN_0043d9e0`, so the fallback wasn't shipped blind. Lead documented.
- **C1/C2 — crashes** (need repro): hit-path identity logging plan in the doc.

**⚠️ Reusable hazard for the next session:** never C-hook a fn that reads the x87
`ST0` set by its caller (`FUN_004326f0`, `FUN_0042d83a`, `FUN_0048b8a0` itself) — the
detour clobbers ST0 before the trampoline runs. Use a side-channel (B1's tier counter)
or a binary patch / `fldz`-guarded forward instead.

## 5x. Session 2026-06-18 (pm) — overnight test follow-ups

Three items reported from overnight testing:
- **Extra boss bomb armour (B5): WORKS** (user-confirmed). No change needed.
- **Cherry/power on boss spell-capture: FIXED + USER-CONFIRMED 2026-06-18 (commits fef9f19 WIP, 0ba087d fix).**
  The both-full gate worked for regular drops but the boss spell-capture reward still
  gave no power to a below-full P2 — crucial on Extra/Phantasm refuel. Initial theory
  (boss pre-converts power->cherry/type-7) was WRONG and the un-convert it inspired was a
  no-op. A broadened `B2 diag spawn` log (every type + spellcard flag + capwin) settled it
  empirically: **the capture reward is a burst of `type=1` (POINT) items at `p1pow=128`,
  `sc=0`** — i.e. when P1 is full the boss substitutes point items for the power drop it
  would give, and the decision is made UPSTREAM of the spawner `FUN_004326f0` (which is why
  nothing reached our hook as type 0/2/7). User domain note: PCB never drops full-power "F"
  (type 4) except on continue, so that's not it. **Fix:** detect spell-end
  (spellcardInfo.isActive 1->0) in `HookedEnemyUpdate` and open a `CAPTURE_WIN_FRAMES`
  (~240f) refuel window; in `HookedItemSpawn`, while that window is open AND `g_b2Suppress`
  (P2 below full), convert **type 1 -> type 0** so the reward drops as power for P2.
  Window-gated so in-stage point items are untouched; the reward lands in the `sc=0` gap
  after capture (verified — a new spell's `sc=1` only returns for the next phase). P1 (full)
  collecting the power item still scores points on pickup, so P1 loses nothing meaningful.
  **Refinement (window):** changed to hold the window open for the WHOLE spell (`sc=1`) plus
  `CAPTURE_WIN_FRAMES` after, robust whether the reward lands at the capture instant or in the
  `sc=0` gap.
  **Cherry REGRESSION + final fix (0ba087d):** the first build dropped CHERRY, not power —
  `type=1->0` then calling SpawnItem directly re-triggered SpawnItem's OWN full-power->cherry
  conversion (`FUN_004326f0:20256` — `round(P1pow)>=0x80 && type in {0,2} => 7`) because P1 is
  full. Fix: after `type=0`, FALL THROUGH to the existing power-zero wrapper (zero `res+0x7c`
  around the call) so `round(power)=0` and the cherry convert is skipped — identical to the
  regular-drop path. Cross-checked vs EoSD `ECL_OPCODE_DROPITEMS` (`uth06dos EclManager.cpp:
  802-817`): boss reward loops SpawnItem, POWER when P1<128, POINT-substituted when full; the
  power->cherry step is a PCB-only second conversion inside SpawnItem. **User-confirmed working.**
  The converted reward is SMALL power (type 0); EoSD drops the first as BIG — bump to type 2 if
  Extra/Phantasm refuel feels too weak.
- **Proximity focus-hitbox fade: FIXED + USER-CONFIRMED 2026-06-18 (commit fef9f19).** Only the sprite faded; the
  focus RING/hitbox (type-0x18 effect, colour at effect+0x1b8) did not. Cause: the
  fade colour was written in the player UPDATE (`ApplyProximityFade`), but the
  effect-manager's own anm/update task runs in a separate, later task that rewrites
  effect+0x1b8 — clobbering us before the draw. (The sprite tint survives only because
  it's written after the player's own anm.) Fix: defer the focus-ring tint and apply
  it at **draw time** in `HookedDraw` (after all update tasks, just before the effect
  draws), via the existing `FadePlayerFocusFx` helper + `s_proxFx*` stash. A one-shot
  `prox-fade: focus-ring tint at draw` log reports the fx ptr/type/tint. If it still
  does not fade, the effect draws BEFORE the player draw and an effect-draw hook is
  needed.

### §8r — first-run-after-launch desync: AUTO-RESYNC (completed the th06 handshake) — 2026-06-18

**Target.** The residual netplay issue ([[netplay-desync-x87-fpu]]): with the transport/epoch
fix + x87 CW pin, every run AFTER the first is bit-perfect; only the FIRST run after process
launch forks (cold-load timing asymmetry → the two sims one lockstep frame apart). Both control
words matched (`0x001f`) at the fork, so it's NOT FP precision — it's a one-time frame offset,
which a manual `Escape → Give up → Retry` already fixes (fresh scene → re-zero + SEEDFORCE).

**Fix — make that recovery AUTOMATIC by completing the dormant `Ctrl_Try_Resync` protocol.**
The th06 resync handshake was HALF-ported into `netcode.cpp`: the *receive* side (sets
`g_resync_trigger`/`g_resync_stage_frame`) and the `Reset` clear existed, but nothing SENT the
request and nothing CONSUMED the trigger. Completed it from the EoSD reference
(`th06_multi_net/src/Supervisor.cpp:124-164`):
- **Host initiates** (only after a SUSTAINED desync — `g_resync_desync_run >= threshold`, default
  180f/~3s, so the ≤15f self-healing blips never trigger it): picks `target = frame + delay*2+2`,
  arms the trigger, and SENDS `Ctrl_Try_Resync(target)` every frame until it fires.
- **Guest adopts** the target on receive (the existing window-validated `RcvPacks` branch).
- **Both EXECUTE** when their own lockstep frame reaches `target`: clear the (misaligned) rcved
  maps, set `g_is_sync=true`, latch `g_resync_did_fire`. The peer re-sends its recent window
  (SendKeys re-ping), so the buffers re-fill aligned.
- **coop.c reseed**: `HookedSceneTick` polls `Nc_PollResyncFired()` right after `Nc_GetInputNet`
  and, on fire, snaps `*ADDR_RNG_SEED = Nc_GetInitSeed()` on THAT frame — both machines fire on
  the same lockstep frame, so they reseed together and re-converge (the automatic SEEDFORCE).

**Safety.** Gated IN-STAGE only (menus aren't RNG-locked; `RunResync` early-returns on `is_in_UI`)
and behind `g_resync_enable` (core default OFF so the native tests stay deterministic; coop.c opts
in via `Nc_SetAutoResync`). It is **inert while synced** — only counts/acts after 180 unbroken
desynced in-stage frames — so it cannot perturb the confirmed-good steady state. New
`[coop] auto_resync` flag (default ON) + launcher checkbox (per [[launcher-owns-coop-ini]]).

**Status: BUILT, native netloop_test green; NEEDS 2-PC TEST.** The resync path can't be unit-tested
(the netcode globals are a single-process singleton — host and guest can't co-exist in one process).
Test: host+guest, force a first-run fork (the usual cold start). Watch `coop_log.txt` for
`RESYNC host-initiated: target frame N`, then `RESYNC #1 executed at frame N`, then
`AUTO-RESYNC reseed at frame N` — and confirm the game returns to sync WITHOUT a manual retry. If a
resync fires but it re-desyncs immediately (loops every ~3s), the first-run cause is a deeper state
divergence than a frame offset — set `auto_resync=0` and capture the trace. Tuning: drop the 180f
threshold (`COOP_RESYNC_THRESHOLD`) for faster recovery if 3s feels long.

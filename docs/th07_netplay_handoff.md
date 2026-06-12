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

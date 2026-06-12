# TH07 (PCB) Co-op Netplay — Handoff for Next Session

> **⚠️ 2026-06-12 UPDATE — read the newer companion docs first; parts of THIS file
> are now superseded.** This doc predates the P2-entity work. Current state:
> - **Fork B (the second-player entity) is largely DONE** in `src/coop/coop.c`:
>   P2 is a piggyback clone with collision/death, separate bombs+power, ghost mode,
>   and shot transfer — but driven by **local keyboard**, not the network.
> - The netcode core (`src/netplay/`) builds + passes tests but is **not yet wired**
>   to the game DLL.
> - New docs:
>   - **`th07_integration_forkA.md`** — true project state, the 4 seams re-verified
>     against `PCBdecomp.c` with line numbers, Fork A (seed-sync + menu lockstep)
>     concrete hooks, and the netcode↔coop integration plan. **Start here.**
>   - **`th07_gameplay_seams.md`** — boss-HP scaling lever (`FUN_0043d9e0`) and the
>     cherry/item-drop determinism coupling.
> - Build now also works on Linux/web: `./build.sh` (distro mingw-w64); CI in
>   `.github/workflows/build.yml` runs it + a native merge unit test on every push.
> - `PCBdecomp.c` in the repo root is the Ghidra dump; note `FUN_0042fd60` is
>   already renamed **`GameUpdate`** in it.

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

## 6. Reference file locations
- Ghidra dump: `C:\Users\rndmdck\Desktop\th07.exe.c`
- Reference mod: https://github.com/RUEEE/th06_multi_net (branch `master`, `src/`)
- th06 decomp base: GensokyoClub/happyhavoc
- Persistent memory index: `C:\Users\rndmdck\.claude\projects\C--Users-rndmdck\memory\MEMORY.md`
  → see `th07-netplay.md` (this handoff is the expanded form of that memory).

## 7. Glossary of Ghidra fn renames (proposed; not yet applied in the db)
Rng_Core=FUN_00431870, Rng_Next32=FUN_004318d0, Rng_NextFloat=FUN_00431900,
Input_Poll=FUN_00430b50, Input_AddJoyBits=FUN_004303f0, Joy_SetBit=FUN_00430370,
DInput_Init=FUN_004383d8, FrameGovernor=FUN_004346e0, GameUpdate=FUN_0042fd60, GameDraw=FUN_0042fe20,
ReplayRecord=FUN_00442cd0, ReplayPlayback=FUN_00442ee0, ScoreCherryDisplay=FUN_00427f22.
(Renaming these in Ghidra + the input/cherry globals is a cheap, high-value early task.)

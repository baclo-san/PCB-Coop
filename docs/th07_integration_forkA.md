# TH07 Co-op — Integration & Fork A (menu lockstep + seed sync)

**Status date:** 2026-06-12 (unattended session). This doc is the actionable
follow-on to `th07_netplay_handoff.md`. It records seams **re-verified against the
decomp dump in this repo** (`PCBdecomp.c`, the Ghidra C export of the pinned
ver-1.00b binary, SHA256 `35467EAF…E80CA`) and lays out the next integration step.
Where this doc and the older handoff disagree, **trust the decomp line citations
here** — they were checked against the committed dump.

> Naming note: `PCBdecomp.c` was exported *after* the user renamed one symbol in
> Ghidra — `FUN_0042fd60` is now spelled **`GameUpdate`** in the dump (line 18943).
> Every other function is still `FUN_<addr>` / `DAT_<addr>`, and the hex in the
> name **is** the VA (e.g. `FUN_00437c70` lives at `0x00437c70`).

---

## 1. Where the project actually is (not what the old handoff implies)

There are **two independent, UNINTEGRATED DLLs**:

| DLL | Source | What it does | Network? |
|---|---|---|---|
| `th07_coop.dll`    | `src/coop/coop.c` (466 ln) | **Fork B is largely DONE.** Grafts a full P2 entity by *piggyback*: detours player update `FUN_00441fb0` + draw `FUN_004420b0` and re-invokes each with `ECX=P2 clone`. P2 is killable (detours collide `FUN_0043e260`/`FUN_0043e6b0`), has separate bombs/power (field-swap around its update + checksum heal via `FUN_004012b0`), ghost mode on death, and shot transfer into P1's array. **P2 input is LOCAL KEYBOARD (IJKL/Space/U/O).** | ❌ none |
| `th07_harness.dll` | `src/harness/harness.c` | Determinism harness: detours `Input_Poll` `0x00430b50`, forces a fixed seed, records/replays input, logs RNG counter. | ❌ |
| (netcode core)     | `src/netplay/*` | Lockstep transport + delay-buffer + merge, **self-tested** (`netloop_test.exe` + the new native `merge_test`). | ✅ but unwired |

**The gap:** the netcode core has never been wired to a game DLL. `coop.c` reads P2
from the keyboard; `netcode.cpp` reads P2 from UDP — but nothing connects them. The
single most valuable next step is **the integration in §4**, gated by **Fork A (§3)**.

### What this session verified / changed
- `MergeKeys` + the `NetButton` layout are split into `src/netplay/merge.{hpp,cpp}`
  (no winsock) and covered by `tests/merge_test.cpp`, which runs **natively on
  Linux/CI** (no game, no wine) and proves host==guest agreement exhaustively over
  all 65536 host inputs × a P2 sample set.
- `build.sh` builds everything with distro `mingw-w64`; `.github/workflows/build.yml`
  runs it on every push/PR (green as of this writing).
- Fixed `Ws2tcpip.h`→`ws2tcpip.h` (broke case-sensitive builds).

---

## 2. The four seams — re-verified with decomp line numbers

| Seam | Address / symbol | Decomp evidence | Notes |
|---|---|---|---|
| **Gameplay input write** | `FUN_00442cd0(int *param_1)` @ `0x00442cd0` | line 27593; body does `DAT_004b9e58 = DAT_004b9e50; DAT_004b9e50 = DAT_004b9e4c;` (27601-27602) then `*param_1 = *param_1 + 1;` (27618) | The replay-record gate. Runs **only in-stage** (gated by `DAT_0062f648>>2 & 1`). `g_InputGameplay`=`0x4b9e50`, prev=`0x4b9e58`. |
| **Logic-frame index** | `*param_1` inside `FUN_00442cd0` | line 27618 `*param_1 = *param_1 + 1;` | Increments once per logic frame. This is the netcode frame counter (NOT the displayed-frame counter `0x0135e1f8`). Note `*param_1=*param_1+1` is a common task-chain idiom (appears in many tasks); the one at 27618 is the replay-record task's. |
| **Menu input write** | `FUN_00437c70(int param_1)` @ `0x00437c70` | line 22774; body: `DAT_004b9e54 = DAT_004b9e4c; DAT_004b9e4c = FUN_00430b50();` (22804-22805) + autofire | th07's per-frame **scene/supervisor task** — registered via `FUN_00430090(FUN_00437c70)` at line 23513, so it ticks every logic frame in menus AND gameplay. `g_InputMenu`=`0x4b9e4c`, prev=`0x4b9e54`. This is th07's analog of th06 `Supervisor::OnUpdate`. |
| **Seed snapshot / RNG** | `DAT_0049fe20` (u16 seed), `DAT_0049fe24` (u32 counter) | decl 1662/1665; ZUN's own `printf("random seed %d %d", …)` at 18492 | seed read for the per-frame sync word. |
| **Game-start init (seed)** | `FUN_00442c60(int param_1)` @ `0x00442c60` | line 27578: `*(u16*)(param_1+0xd4)=DAT_0049fe20; DAT_0049fe24=0;` | record-start: saves the current (timeGetTime) seed into the replay obj, zeroes the counter. The seed-sync detour point. |
| **Seed restore (playback)** | `FUN_00442ee0` body @ line 27909 | `DAT_0049fe20 = *(u16*)(iVar1 + 0x20);` | proves seed is freely writable at game start — mirror this for net seed. |

There is a **second** menu poll site: `FUN_0045bf15` @ line 38341 also does
`DAT_004b9e4c = FUN_00430b50()` (line 38409). It's a different scene path (likely a
specific submenu). For lockstep, overwriting in `FUN_00437c70` covers the common
case; watch this one if a particular menu desyncs.

---

## 3. Fork A — the two-input-globals problem, with concrete hooks

th06 has ONE input word for menus+gameplay, so its Supervisor hook locksteps
everything off `calcCount`. th07 splits it: **menus read `g_InputMenu` (0x4b9e4c)**,
**gameplay reads `g_InputGameplay` (0x4b9e50)**, and the gameplay copy only happens
in-stage (`FUN_00442cd0`). Two pieces are needed:

### 3a. Seed sync (host forces the seed both machines load)
- Host picks `rngSeedInit` (already plumbed through `Netcode_SetConnected(...,
  rngSeedInit)` and `CtrlPack.init_setting`).
- **Detour `FUN_00442c60`** (game-start init). In the detour, BEFORE calling the
  trampoline, write `*(u16*)0x0049fe20 = rngSeedInit;` on BOTH machines. The
  original then saves *that* seed into the replay obj and zeroes the counter — both
  machines now start byte-identical. (This is exactly what playback does at line
  27909, so it's a known-safe write point.)
- Validate with the harness counter `0x0049fe24`: if both machines' counters march
  in lockstep, the sim is in sync.

### 3b. Menu lockstep (so char/difficulty select can't diverge)
Recommended = the handoff's **option A1**, now with a verified hook:

- **Detour `FUN_00437c70`** (the scene task @ 0x00437c70). Run the trampoline first
  (it sets `DAT_004b9e54 = old menu`, then `DAT_004b9e4c = poll()`). **After** it
  returns, overwrite:
  ```c
  uint16_t self = *(uint16_t*)0x004b9e4c;          // local poll the original just did
  int ctrl;
  uint16_t merged = Netcode_GetInput_Net(frame, /*is_in_UI=*/true, &ctrl);
  *(uint16_t*)0x004b9e4c = merged;                 // both machines now navigate together
  ```
  In UI mode `MergeKeys` returns `self|rcv`, so both menus move in lockstep. Because
  we overwrite `g_InputMenu` *every* frame, next frame's `DAT_004b9e54` (prev) is the
  *merged* value too, so menu edge-detection stays self-consistent.
- **Frame index for menus:** `FUN_00437c70` has no obvious per-frame counter of its
  own — maintain your **own** `static int g_uiFrame; g_uiFrame++` in the detour. When
  the stage starts and `FUN_00442cd0` begins running, switch the netcode frame source
  to its `*param_1`, OR just keep one DLL-owned counter incremented in `FUN_00437c70`
  (which runs every frame, menus and stage) and pass it to both hooks. **One counter,
  one owner** is the clean shape — see §4.

> ⚠️ Timing subtlety to test on the real game: `Input_Poll` fires ~1.7×/logic-frame
> from multiple sites, but the *write* to `g_InputMenu` we care about is the one in
> `FUN_00437c70`. Confirm the merged value isn't clobbered by the second poll site
> (`FUN_0045bf15`) in the menus you actually use (host/join/char/difficulty).

### 3c. Alternative = A2 (minimal): skip menu lockstep
Hook only `FUN_00442cd0` for gameplay; have players coordinate menus manually; host
forces the seed in the `FUN_00442c60` detour. Fragile UX but far less surface. Good
as a **first end-to-end milestone** before investing in 3b.

---

## 4. Integration plan: wire the netcode into the co-op DLL

Goal: replace `coop.c`'s `ReadP2InputLocal()` (keyboard) with the netcode's merged
P2 bits, so the existing, already-working P2 entity is driven over UDP.

**Build:** `coop.c` is C, the netcode is C++. The C-callable seam already exists:
**`src/netplay/netcode_c_api.h` / `.cpp`** wrap the `Netcode_*` API with plain C
linkage + C-friendly types (`Nc_StartHost`, `Nc_GetInputNet(frame, is_in_UI,
&ctrl)`, etc.; verified to compile from a C TU and is built by CI). To make the
integrated DLL, compile `coop.c` and link the netcode TUs (`netcode.cpp`,
`Connection.cpp`, `merge.cpp`, `netcode_c_api.cpp`) + `-lws2_32` into
`th07_coop.dll` — i.e. add those four to the coop link line in `build.sh`/`build.ps1`
(they're already compiled for the netcode test). `coop.c` then `#include
"netcode_c_api.h"` and calls `Nc_*`.

**One hook owns the frame counter and both input globals:**

1. In the **`FUN_00437c70` detour** (runs every logic frame):
   - `int frame = ++g_netFrame;` (DLL-owned, single source of truth).
   - `uint16_t merged = Netcode_GetInput_Net(frame, is_in_UI, &ctrl);`
     where `is_in_UI = (not in an active stage)` — derive from the same
     `DAT_0062f648>>2 & 1` flag that gates `FUN_00442cd0`, or from scene state.
   - Write `g_InputMenu = merged` (UI) so menus lockstep.
   - `readLocalInput` callback returns the local poll (`*(u16*)0x4b9e4c` BEFORE
     overwrite, or call `Input_Poll` directly); `readRngSeed` returns `*(u16*)0x4b9e20`.
2. In-stage, `FUN_00442cd0` copies `g_InputMenu`→`g_InputGameplay` for free, so P1
   (low bits) is already the merged P1. **For P2**, keep `coop.c`'s field-swap update,
   but source P2's word from the **high bits of `merged`** instead of the keyboard:
   `uint16_t p2 = ((merged>>9)&0x7F)` re-expanded to low-bit layout
   (SHOOT2→SHOOT, …). i.e. replace `ReadP2InputLocal()` with "unpack P2 from the
   netcode merged word."
3. Seed sync per §3a in a `FUN_00442c60` detour.

**Net effect:** P1 = host low bits (native), P2 = guest input unpacked into coop.c's
existing P2 entity. Both machines compute the identical `merged`, so both render the
same two players. Determinism is gated by seed sync + the harness counter check.

### Open design choices that NEED the user / a live test (do not guess)
- **Per-player vs shared cherry/border** (handoff §4) — cherry feeds shared item-drop
  RNG (`Rng_Next32()%100 <= cherryRatio`, lines ~10445/10495), so per-player cherry is
  inside the determinism surface. Decide shared-pool (safe) vs per-player (needs
  byte-identical duplicated state) once the mechanic is visible.
- **P2 HUD** — P2's lives/bombs/power are currently only logged to `coop_log.txt`.
- **Resurrection / revive-by-graze** — deferred in coop.c; needs ghost-mode + 1up.
- **Boss HP ×2 for double DPS** (Tier 1) — not yet located; likely ECL/enemy HP-init.

---

## 5. Quick reference — addresses used by the existing DLLs (sanity-checked vs decomp)

| coop.c macro | Value | Decomp confirmation |
|---|---|---|
| `ADDR_PLAYER_BASE`   | `0x004bdad8` | `DAT_004bdad8`, used as player static (27498/27510) |
| `ADDR_PLAYER_UPDATE` | `0x00441fb0` | `FUN_00441fb0` def 27207, registered as task 27507 |
| `ADDR_PLAYER_DRAW`   | `0x004420b0` | `FUN_004420b0` def 27243, registered as task 27508 |
| `ADDR_COLLIDE_BULLET`| `0x0043e260` | `FUN_0043e260` `__thiscall` def 25986 |
| `ADDR_COLLIDE_LASER` | `0x0043e6b0` | `FUN_0043e6b0` (laser collision) |
| `ADDR_INPUT_GAMEPLAY`| `0x004b9e50` | `DAT_004b9e50`, written by `FUN_00442cd0` |
| `ADDR_RES_PTR`       | `0x00626278` | resource struct ptr (score-manager `0x626270`+8) |

(All addresses are build-specific to ver 1.00b; re-pin if the binary changes.)

# Fork A — Netcode ↔ th07 integration seams (verified against PCBdecomp.c)

This document pins the exact in-game hook points that wire the engine-agnostic
netcode core (`src/netplay/`) into th07.exe. Every address/offset/line below was
read directly out of `PCBdecomp.c` (the Ghidra dump, same db the handoff uses),
not inferred. It is the concrete recipe the integration DLL needs.

**Binary:** th07.exe ver 1.00b, SHA256 `35467EAF…E80CA` (see handoff §0). All
addresses are build-specific to this hash.

> **Naming note:** `PCBdecomp.c` was exported *after* the user renamed one symbol
> in Ghidra — `FUN_0042fd60` is spelled **`GameUpdate`** in the dump (def line
> 18943, called at 21133). Every other function is still `FUN_<addr>` /
> `DAT_<addr>`, and the hex in the name **is** the VA.

---

## 1. The replay subsystem is the injection surface

PCB muxes gameplay input through its **replay record/playback** tasks. Two input
globals (handoff §1):

| Addr | Symbol | Meaning |
|---|---|---|
| `0x004b9e4c` | `DAT_004b9e4c` = `g_InputMenu` | raw keyboard poll (menus read this) |
| `0x004b9e50` | `DAT_004b9e50` = `g_InputGameplay` | **what gameplay reads (low 9 bits)** |
| `0x004b9e58` | `DAT_004b9e58` = `g_InputGameplayPrev` | previous-frame gameplay (edge detect) |
| `0x0049fe20` | `DAT_0049fe20` = `g_RngState.seed` (u16) | RNG seed |
| `0x0049fe24` | `DAT_0049fe24` = `g_RngState.call_counter` (u32) | RNG call counter (desync oracle) |
| `0x004b9e48` | `DAT_004b9e48` | replay-manager object pointer |

### Mode flags live in `DAT_0062f648`
- **bit 0x4** (`>>2 & 1`): *recording active* — input is being captured this frame.
  Set during normal play. This is the gate inside `FUN_00442cd0`.
- **bit 0x8** (`>>3 & 1`): *playback mode* — reading input from a replay file.
  Selected at stage load (PCBdecomp.c:18445 / 18465).

**Both netplay machines run in RECORD mode** (bit 0x4 set, bit 0x8 clear), so the
record task `FUN_00442cd0` runs every logic frame — that is our seam.

---

## 2. `FUN_00442cd0` — ReplayRecord — THE per-frame input injection point
PCBdecomp.c:27593, `__fastcall(int *param_1)` (ECX = the record task object).

```c
uVar1 = DAT_004b9e4c;                       // g_InputMenu
if ((DAT_0062f648 >> 2 & 1) != 0) {         // recording active
    DAT_004b9e58 = DAT_004b9e50;            // g_InputGameplayPrev = g_InputGameplay
    DAT_004b9e50 = DAT_004b9e4c;            // *** g_InputGameplay = g_InputMenu  (INJECT HERE) ***
    if ((*(char *)(DAT_00626274 + 0x25) == '\0') && ((DAT_00575adc >> 3 & 1) == 0)) {
        ...
        *param_1 = *param_1 + 1;            // frame counter advances (line 27618)
    }
}
```

**Facts the integration relies on:**
- `*param_1` (i.e. `param_1[0]`) is the **netcode logic-frame index**. It advances
  once per *recorded* frame and is paused exactly when a replay would pause
  (the inner `DAT_00626274+0x25 == 0` gate = "not paused/not in sub-menu"). Use it,
  NOT the displayed-frame counter `0x0135e1f8` (handoff §1).
- The write at line 27602 (`DAT_004b9e50 = DAT_004b9e4c`) is unconditional within
  the recording branch — it runs in stages, the inner pause gate does not affect it.

**Hook recipe (A2, gameplay-only lockstep):**
```c
int __fastcall Hook_442cd0(int *self) {
    int r = orig_442cd0(self);              // runs ZUN's record: g_InputGameplay = g_InputMenu
    if (Netcode_IsConnected()) {
        int ctrl = 0;
        unsigned short merged = Netcode_GetInput_Net(*self /*frame*/, /*is_in_UI=*/false, ctrl);
        *(volatile uint16_t*)0x004b9e50 = merged;   // override with the lockstep-merged word
        // g_InputGameplayPrev (0x004b9e58) was already set to last frame's merged value
        // by ZUN's line 27601, because last frame we wrote merged into 0x004b9e50.
    }
    return r;
}
```
- `Netcode_GetInput_Net` does the UDP exchange + 5 s lockstep stall internally
  (exactly th06's model). Calling it inside the per-frame task is correct.
- `readLocalInput` callback → read `*(u16*)0x004b9e4c` (g_InputMenu, the raw poll).
- `readRngSeed`   callback → read `*(u16*)0x0049fe20`.

---

## 3. `FUN_00442c60` — game-start init — THE seed-sync point
PCBdecomp.c:27578, `__fastcall(int param_1)` (registered as a priority-6 task by
`FUN_00443aa0` in record mode; see §5).

```c
*(undefined2 *)(param_1 + 0xd6) = 0;
*(undefined2 *)(param_1 + 0xd4) = DAT_0049fe20;   // save seed into replay obj +0xd4
DAT_0049fe24 = 0;                                 // reset RNG call counter
if (DAT_0062f640 != 0)                             // 2-player/PVP start flag
    *(ushort *)(param_1 + 0xd6) |= 0x100;
DAT_0062f640 = 0;
```

This runs once at game/stage start and **reads `DAT_0049fe20`** (the seed) to snapshot
it, then zeroes the counter. To make both machines deterministic from the same seed:

**Hook recipe (seed sync):**
```c
int __fastcall Hook_442c60(int self) {
    if (Netcode_IsConnected())
        *(volatile uint16_t*)0x0049fe20 = Netcode_GetInitSeed();  // host-chosen seed
    return orig_442c60(self);   // saves the (now forced) seed, zeroes counter
}
```
The host picks the seed and sends it in `CtrlPack.init_setting.rng_seed_init`
(already wired in `Connection.hpp`); both call `Netcode_SetConnected(true, delay, seed)`
so `Netcode_GetInitSeed()` returns the same value on both ends.

**Cross-check — the existing replay seed-restore** (PCBdecomp.c:27909, inside the
playback-load path): `DAT_0049fe20 = *(undefined2 *)(iVar1 + 0x20);` — ZUN restores
the seed from replay-file offset +0x20 on playback. Our seed-force mirrors this exact
mechanism; it is known-safe because the engine already supports externally-supplied
seeds.

---

## 4. `FUN_00442ee0` — ReplayPlayback (reference only)
PCBdecomp.c:27648. Feeds `DAT_004b9e50` from the replay buffer
(`DAT_004b9e50 = *(short *)param_1[0x21]`) and advances `*param_1`. Not hooked for
netplay (we are always in record mode), but it documents the symmetric read side —
useful if a future "spectator"/replay-of-netgame feature is wanted.

---

## 5. `FUN_00443aa0` — replay-manager setup (how the tasks get registered)
PCBdecomp.c:27950, `__fastcall(int param_1, undefined4 param_2)`. `param_1` selects mode:

- **`param_1 == 0` (RECORD):** registers `FUN_00442cd0` (per-frame record) at
  `mgr+0xc4`, and `FUN_00442c60` (start init) at `mgr+0xd0`, priority 6.
- **`param_1 == 1` (PLAYBACK):** registers `FUN_00442ee0` + `FUN_00442e50`.

Manager object = `DAT_004b9e48` (`operator_new(0xd8)`); it stores the frame-counter
task at `+0xc4`. Called from the stage-load function at PCBdecomp.c:18447 (playback)
and 18466 (record). The "random seed %d %d" log is at 18492 — a convenient breakpoint
to confirm the seed actually took on both machines.

**Implication for hooking:** because the per-frame counter `*param_1` resets each new
game (the task object is re-`new`'d), the DLL should call `Netcode_Reset()` when it
detects the frame index jump backwards / a fresh `DAT_004b9e48`. (th06 mod does the
analogous reset on `calcCount` reset — handoff §2 "On new game it detects calcCount
reset and clears the rcved maps".)

---

## 6. Open: menu lockstep (Fork A1 vs A2)

`FUN_00442cd0` only runs **in-stage**, so the §2 hook locksteps gameplay but NOT the
pre-stage menus (char/difficulty select). Two options (handoff §3b Fork A):

- **A2 (minimal, recommended first):** lockstep gameplay only (§2 hook) + force the
  seed (§3 hook). Players coordinate char/difficulty manually. Smallest surface;
  enough to validate the whole lockstep premise end-to-end in a real stage.
- **A1 (clean):** also hook an *always-runs* per-frame seam and overwrite `g_InputMenu`
  (0x004b9e4c) with the merged word (`is_in_UI=true` → `self|rcv`), so menus navigate
  together.

  **A1 seam resolved to `FUN_00437c70` (0x00437c70)** — the per-scene-tick input
  function (`__fastcall(int param_1)`, param_1 = supervisor/scene object). At
  PCBdecomp.c:22804–22805 it does the menu poll:
  ```c
  DAT_004b9e54 = DAT_004b9e4c;          // g_InputMenuPrev = g_InputMenu
  DAT_004b9e4c = FUN_00430b50();         // g_InputMenu = Input_Poll()
  ... autofire counter on 0x4b9e60 ...
  ```
  then dispatches scene transitions (it logs `"scene %d -> %d"`). The handoff's
  "scene-tick callers of Input_Poll" at 0x00437d80 / 0x00437dfc are NOT separate
  functions — they are the two `FUN_00430b50` **call sites inside** `FUN_00437c70`
  (0x437d80 ≈ line 22805, 0x437dfc ≈ line 22819; the fn starts at 0x437c70).

  **A1 hook recipe:** detour `FUN_00437c70`; after the original sets `g_InputMenu` from
  the poll, overwrite it with `Netcode_GetInput_Net(frame, /*is_in_UI=*/true, ctrl)`.
  Use a DLL-owned per-logic-frame counter for `frame` here (this seam has no replay
  frame counter of its own, unlike `FUN_00442cd0`). Verify in-game that `FUN_00437c70`
  ticks exactly once per logic frame before relying on it for the frame index.

  **Registration evidence (overnight session, verified):** `FUN_00437c70` is
  registered as a per-frame supervisor task via `FUN_00430090(FUN_00437c70)` at
  PCBdecomp.c:23513 — it ticks every logic frame in menus AND gameplay, making it
  th07's analog of th06's `Supervisor::OnUpdate`. Also note: because the detour
  overwrites `g_InputMenu` *every* frame, next frame's `DAT_004b9e54` (prev) is the
  *merged* value too, so menu edge-detection stays self-consistent.

  **⚠️ Second menu poll site:** `FUN_0045bf15` (line 38341) also does
  `DAT_004b9e4c = FUN_00430b50()` (line 38409) on a different scene path (likely a
  specific submenu). The `FUN_00437c70` overwrite covers the common case; if a
  particular menu desyncs, check whether this site clobbered the merged word.

  **Caveat — the dump is partial (CORRECTED 2026-06-12):** `GameUpdate`
  (`FUN_0042fd60`) **IS in the dump** — under its Ghidra rename `GameUpdate`
  (line 18943), which is why a `FUN_0042fd60` search comes up empty. Still genuinely
  absent: the frame governor `FUN_004346e0` and the task-chain manager `0x626218`
  (the export covers ~94% of functions but omits some top-level loop functions —
  the fuller db is the user's Desktop `th07.exe.c`). With the `FUN_00437c70`
  registration evidence above, A1 has a verified seam; until it's validated
  in-game, A2 remains the safe path.

---

## 7. Integration checklist (for the DLL that ports the netcode in)

1. On attach: read role/peer/port/delay from a config file (stand-in for the
   ConnectionUI in `reference/`), `Netcode_StartHost/Guest`, set callbacks
   (`readLocalInput`→0x4b9e4c, `readRngSeed`→0x49fe20).
2. Handshake → `Netcode_SetConnected(true, delay, seed)` (host picks seed).
3. MinHook `FUN_00442cd0` (0x00442cd0) — §2 input injection.
4. MinHook `FUN_00442c60` (0x00442c60) — §3 seed sync.
5. Detect new-game (frame index reset / new `DAT_004b9e48`) → `Netcode_Reset()`.
6. (A1, later) hook the once-per-logic-frame seam for menu lockstep.
7. Fork B (P2 entity, `src/coop/coop.c`) is independent and already drives a second
   player off high-bit input; once the netcode feeds high bits into 0x4b9e50, the two
   forks meet. NOTE: coop.c currently reads its own local keyboard for P2
   (`ReadP2InputLocal`); under netplay that source is replaced by the merged word's
   high bits.

All hook addresses verified present in PCBdecomp.c at the lines cited above.

---

## 8. Wiring the netcode INTO coop.c (the single-DLL integration)

(From the 2026-06-12 overnight session.) Goal: replace `coop.c`'s
`ReadP2InputLocal()` (keyboard) with the netcode's merged P2 bits, so the existing,
already-working P2 entity is driven over UDP — one DLL instead of the separate
`th07_coop_net.dll`.

**Build:** `coop.c` is C, the netcode is C++. The C-callable seam exists:
**`src/netplay/netcode_c_api.h` / `.cpp`** wrap the `Netcode_*` API with plain C
linkage + C-friendly types (`Nc_StartHost`, `Nc_GetInputNet(frame, is_in_UI,
&ctrl)`, etc.). To make the integrated DLL, compile `coop.c` and link the netcode
TUs (`netcode.cpp`, `Connection.cpp`, `merge.cpp`, `netcode_c_api.cpp`) +
`-lws2_32` into `th07_coop.dll`; `coop.c` then `#include "netcode_c_api.h"` and
calls `Nc_*`.

**One hook owns the frame counter and both input globals:**

1. In the **`FUN_00437c70` detour** (runs every logic frame — §6 A1):
   - `int frame = ++g_netFrame;` (DLL-owned, single source of truth).
   - `uint16_t merged = Nc_GetInputNet(frame, is_in_UI, &ctrl);` where
     `is_in_UI = (not in an active stage)` — derive from the same
     `DAT_0062f648>>2 & 1` flag that gates `FUN_00442cd0`, or from scene state.
   - Write `g_InputMenu = merged` (UI) so menus lockstep.
   - `readLocalInput` callback returns the local poll (`*(u16*)0x4b9e4c` BEFORE
     the overwrite, or call `Input_Poll` directly); `readRngSeed` returns
     `*(u16*)0x0049fe20`.
2. In-stage, `FUN_00442cd0` copies `g_InputMenu`→`g_InputGameplay` for free, so P1
   (low bits) is already the merged P1. **For P2**, keep `coop.c`'s field-swap
   update, but source P2's word from the **high bits of `merged`**:
   `uint16_t p2 = (merged >> 9) & 0x7F` re-expanded to low-bit layout
   (SHOOT2→SHOOT, …) — i.e. replace `ReadP2InputLocal()` with "unpack P2 from the
   netcode merged word".
3. Seed sync per §3 in a `FUN_00442c60` detour.

**Net effect:** P1 = host low bits (native), P2 = guest input unpacked into coop.c's
existing P2 entity. Both machines compute the identical `merged`, so both render the
same two players. Determinism is gated by seed sync + the `0x0049fe24` counter check.

**⚠️ Determinism interaction to keep in mind:** coop.c's anti-tamper heal
`FUN_004012b0` **consumes 2 shared-RNG calls** per invocation (canaries) — see the
note in `th07_item_collect_credit.md` §4. Harmless under lockstep (both machines run
P2's identical sim), but it makes the co-op RNG stream differ from vanilla.

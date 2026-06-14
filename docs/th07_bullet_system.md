# TH07 (PCB) enemy-bullet (danmaku) system

Reverse-engineered 2026-06-14 (general mapping pass). Addresses are build-specific
to `th07.exe` ver 1.00b (SHA256 `35467EAF…E80CA`). This complements
`th07_player_struct.md` (player side) and the collision notes already in
`src/coop/coop.c` — it documents the ENEMY-bullet manager, the per-bullet struct,
and the spawn/update/collision flow. Useful for any future bullet-side co-op work
(per-player bullet clears, P2-specific danmaku interactions, debug tooling).

## 1. The bullet manager

| What | Address / value |
|---|---|
| Manager singleton (object base) | **`0x0062f958`** (`&DAT_0062f958`) |
| Update task fn | `FUN_00425a50` (@0x425a50), `__fastcall(ECX=mgr)`, task priority 0xc |
| Draw task fn | `FUN_00426f60` (@0x426f60), draw priority 10 |
| Aux fn (cleanup/clear?) | `FUN_00427620` (@0x427620) |
| Spawn one bullet | `FUN_00423730` (@0x423730), `__thiscall(mgr, spec, …)` → 0 ok / 1 no free slot |
| Init/registration | `FUN_004276a0` (@0x4276a0): sets the task fn ptrs and registers the manager (`_DAT_009a9ad8 = &DAT_0062f958`) |

The update is gated by `DAT_0062627c` (a pause/freeze byte) — when 0 it also runs
the item-update loop `FUN_00432990` first (that's the loop coop.c hooks as
`ADDR_ITEM_LOOP` for P2 item credit).

### The bullet array

- First slot at **`mgr + 0xb8c0`** (= `0x0063a218`).
- Slot **stride = `0xd68` bytes**.
- The update loop walks **1024 (`0x400`) slots** per block; on wrap it advances the
  slot pointer by `0x35a000` to the next block (so the pool is several 1024-slot
  blocks — exact block count not pinned here; the active count is tracked, below).
- `mgr + 0x37a128` = active-bullet count accumulator (recomputed each update).
- `mgr + 0x37a15c` = free-slot search cursor used by the spawner.
- A slot with `*(u16*)(slot+0xbfc) == 0` is **inactive/free**.

## 2. Per-bullet struct (offsets relative to a slot)

| off | type | meaning |
|---|---|---|
| 0xb7c | float[3] | collision SIZE (passed to the player collision leaves) |
| 0xb8c / 0xb90 / 0xb94 | float | position X / Y / Z |
| 0xb98 / 0xb9c / 0xba0 | float | velocity X / Y / Z (added to pos each move frame) |
| 0xbb0 | float | current speed (recomputed from angle) |
| 0xbbc | float | direction/angle (radians) |
| 0xbcc / 0xbd0 | int | accel/aim scratch |
| 0xbdc | int | AGE in frames — must be `> 0xf` before the bullet can graze/hit |
| 0xbf0 | int | spawn-delay countdown; the bullet only MOVES once this hits 0 |
| 0xbf4 | u16 | behavior-flag bitfield → triggers movement modifiers (below) |
| 0xbf6 | u16 | flags; bit `0x1000` = "no item drop when cleared" / invuln-to-clear |
| 0xbfc | u16 | STATE: 0 inactive, 1 active, 2 spawning/anim, 5 clearing→item |
| 0xbfe | i16 | off-screen despawn counter (counts toward `FUN_00417b20` free) |
| 0xc01 | u8 | **GRAZED flag** — set once the player grazes; GATES the hit test |
| 0xc0c | int | sound/extra handle (≥0 ⇒ a managed handle, freed on despawn) |
| 0x1e0 / 0x1e4 | ptr | anm sprite object; `*(slot+0x1e4)+0x2c/+0x30` = sprite px H/W |
| 0xd48 | float | base speed (copied into +0xbb0 on bounce/retarget) |
| 0xd5c / 0xd60 | int | lifetime counter / limit (clears a movement flag at limit) |

### Movement-modifier flags (`+0xbf4`)
Each bit dispatches a per-frame modifier fn (FUN_00425a50 inner switch):
`0x1`→`FUN_004250d0`, `0x10`→`FUN_004251a0`, `0x20`→`FUN_00425310`,
`0x40`→`FUN_00425400`, `0x100`→`FUN_00425580`, `0x80`→`FUN_00425700`,
`0xc00`→`FUN_004258a0` (the family handles accel, angular velocity, homing,
speed ramps, etc.). Bit `0x400` additionally enables **wall bounce** off the
bottom edge; the X/Y bounce reflection is at FUN_00425a50:14568-14576.

## 3. Spawn — `FUN_00423730`

`__thiscall(mgr, spec, …)`; finds a free slot via the `+0x37a15c` cursor, copies
the bullet template (`spec`, an ECL-built block: e.g. `spec+0x20..` = the 0x1e-dword
kinematics block copied to `slot+0x305`, `spec+0xc4` → `slot+0xbf6` flags), sets
state, and returns 0 (or 1 if the pool is full). Bullets are created by the enemy
ECL interpreter through this path. If `mgr+0x37a12c` (a "clear pending" latch) is
set and the bullet isn't clear-immune (`+0xbf6 & 0x1000`), the new bullet is born
already in the clearing state (`+0x2ff/0xbfc = 5`) — i.e. bullets spawned during a
bomb/clear are immediately swept.

## 4. Collision with the player (ties into Fork B)

Inside the update, once a bullet is active, old enough (`+0xbdc > 0xf`) and not yet
grazed (`+0xc01 == 0`):
1. **Graze test** `FUN_0043e3b0(pos=slot+0xb8c, size=slot+0xb7c)`
   (`__thiscall`, ECX = the P1 static base `0x4bdad8`, hardcoded). Returns
   `1` = just grazed → set `+0xc01`; `2` = bullet should clear.
2. Only after `+0xc01` is set does the **hit test** `FUN_0043e260(pos, size)`
   run on subsequent frames. Return `2` ⇒ bullet cleared + an item spawns
   (`FUN_004326f0(pos, DAT_004bfedc, 1)`).

This is the mechanism `src/coop/coop.c` relies on: the per-bullet hit test is
GATED behind `+0xc01`, so P2 (a second player base) only becomes hittable by a
bullet if P2 ALSO grazes it — hence coop.c re-invokes the graze leaf for P2 and
reports "grazed" if either player did. (See the coop.c header + `th07_player_struct.md`.)

## 5. Geometry / constants

- Playfield bounds used for despawn/bounce: **X ∈ [0, 384), Y ∈ [0, 448)** (matches
  the 384×448 PCB playfield). Off-field bullets count down `+0xbfe` then free via
  `FUN_00417b20`; "stay on screen" behavior bullets (`+0xbf4 & 0xdc0`) instead count
  UP and free at 0x80.
- `DAT_004bfedc` = the item type dropped by a cleared bullet (point/scratch item).
- `DAT_00575ac8` = a global bullet-speed scalar (slow-down / time-stop factor),
  applied as `speed * DAT_00575ac8` on retarget.

## 6. Open / not yet pinned
- Exact bullet-pool block count (≥1 block of 1024; the update jumps `+0x35a000`).
- The `spec` (ECL bullet template) full layout — only the fields copied in
  `FUN_00423730` are noted here. The ECL VM that builds it is a separate system.
- `FUN_00426f60` (draw) internals — uses the per-slot anm object at `+0x1e0/0x1e4`
  with the sprite-batch blit documented in `th07_hud_sprite_system.md`.

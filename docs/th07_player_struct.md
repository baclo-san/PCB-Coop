# PCB Player struct — verified offset map (for Fork B, the P2 entity)

Grafting a second player (`src/coop/coop.c`, Fork B) means cloning and driving the
Player object. This is the field map gathered from `PCBdecomp.c`, to spare the next
session re-deriving it. Base of the single static player = **`0x004bdad8`**; struct
size = **`0xb7e78`** (`memcpy` size used by coop.c, = the chardata-ptr offset, below).

Offsets are from the player base. "src" = where verified. Build-specific to th07.exe
ver 1.00b (`35467EAF…E80CA`).

| Offset | Type | Meaning | src (PCBdecomp.c) |
|---|---|---|---|
| `+0x000`–… | — | (header / task fields, not mapped) | |
| `+0x1b8` | u32 | color/tint (0xffffffff normal, 0xffff0000 / 0xff flicker during invuln) | FUN_00441330:26892; FUN_00441fb0:27283 |
| `+0x1c8` / `+0x1cc` | float | **screen** X/Y = world pos + camera (`_DAT_0062f864/868`) | FUN_004420b0:27260 |
| `+0x1d0` | u32 | (zeroed each draw) | 27262 |
| `+0x240a` | u8 | has-options flag: if !=0, sub-objects at `+0x24c` & `+0x498` are live | FUN_00441fb0:27229; FUN_004420b0:27264 |
| `+0x24c`, `+0x498` | obj | the two **option** sub-objects (own update/draw via FUN_00450d60/FUN_0044f9a0) | 27230-27231, 27273-27274 |
| `+0x2400` | int | a countdown timer (decrements while !=0; ticks FUN_00424740) | FUN_00441330:26871 |
| `+0x2408` | u8 | **player STATE**: 0=play, 1=enter, 2=dying, 3=respawn-invuln, 4=border | many (coop.c OFF_STATE) |
| `+0x2444` | obj[] | **shot array**: first slot; stride `0x364`, 96 slots; `+0x34a` u16 active | coop.c OFF_SHOTS |
| `+0x414`/`418`/`41c` | float | option-A world pos (set from `+0x9b4/9b8` + camera) | FUN_00441fb0:27267 |
| `+0x660`/`664`/`668` | float | option-B world pos (from `+0x9c0/9c4`) | 27270 |
| `+0x930`/`934`/`938` | float | **player world pos X / Y / Z** | coop.c; FUN_004420b0:27260; FUN_00442370 |
| `+0x9b4`/`9b8`, `+0x9c0`/`9c4` | float | option offsets (relative to player) | 27267-27271 |
| `+0x16a00` | u32 | invuln-counter target/sentinel (`0xfffffc19` = -999) for FUN_0043958d | FUN_00441330:26891 |
| `+0x16a04` | int | invuln-counter fractional carry | 26890; FUN_0043958d @26957 |
| `+0x16a08` | int | **invuln/border counter**: state-3 ends when this < 1 → state=0 | FUN_00441330:26883; FUN_00441fb0:27277 |
| `+0x16a20` | int | **bombing** flag (nonzero while a bomb is active) | coop.c OFF_BOMBING; FUN_004420b0:27251 |
| `+0x16a24` | int | bomb sub-state (selects which bomb-update fn ptr runs) | FUN_004420b0:27252 |
| `+0x16a40`, `+0x16a48` | code* | bomb update function pointers (called indirectly) | 27253-27256 |
| `+0xb7e68` | ptr | **death/"spirit" sub-object** ptr: during state 3 its `+0x24c/250/254` get the player pos; cleared (`+0x2cc=0`) when respawn completes | FUN_00441330:26876-26886 |
| `+0xb7e70` | ptr | **chardata** ptr (char speed table); `!=0` ⇒ a character is loaded | coop.c OFF_CHARDATA |
| `+0xb7e78` | — | end of struct (clone size) | coop.c PLAYER_SIZE |

## Resources are NOT in the player struct
Lives/bombs/power/score live in the **score-manager resource struct** reached via the
singleton at `0x00626270` (`*(0x626270+8) == *(0x626278)`), NOT off the player. coop.c
field-swaps P2's bombs (`+0x68`) / power (`+0x7c`) there, with the anti-tamper checksum
(`+0xb0`) re-healed via `FUN_004012b0`. See coop.c's header comment — it's the
authoritative, runtime-validated account of the resource side.

## The `+0xb7e68` "spirit" pointer — relevant to resurrection
The state-3 handler mirrors the player's position into the object at `+0xb7e68` and
releases it on respawn. This is the death/respawn visual (the drifting spirit). th06's
co-op **resurrection** mechanic (hold focus + release shot near a dead partner in
PLAYER_STATE_SPIRIT for 90 frames → spend a life to revive) keys off exactly this
death/spirit state. When porting resurrection to PCB, this pointer + state 3/2 are the
hooks to investigate. (Not yet traced further — flagged for the resurrection task.)

## Open / unverified
- Hitbox/graze radius offsets, focus-mode flag, and the power-shot level field are not
  yet located (collision uses the leaf primitives FUN_0043e260/6b0 that coop.c detours,
  which are param-relative — so P2 collision works without knowing the radius offset).
- Whether `FUN_00441fb0`'s sub-calls pass `ECX = the cloned P2` (vs reloading the P1
  static base) is the key open question for the clone's state machine — needs the
  disassembly (ECX is hidden in the decompiled C). See handoff §5b.

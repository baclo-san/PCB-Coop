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
hooks to investigate — and the death FSM below is where the revive injects.

## Death FSM & the resurrection seam (2026-06-12 overnight session, verified)

The player update `FUN_00441fb0` dispatches on the state byte `+0x2408`. State `2`
(dying) routes to the **death/respawn handler `FUN_00440cf0` @ `0x00440cf0`**
(def line 26731) — the seam for a "revive your partner" mechanic:

| Field / call | Meaning | PCBdecomp.c |
|---|---|---|
| `+0x16a08` | in state 2 = **deathbomb counter** — frames since the fatal hit; death COMMITS once `0x1d (29) < it` (the timer is state-multiplexed: invuln countdown in state 3, border countdown in state 4) | 26748 |
| `+0x23f8` | **respawn timer** — set on death, decremented each frame; hits 0 → respawn (restore control, drop power items) | 26778–26779 |
| `FUN_0042d5cd(-1)` | **lose a life** on death commit | 26763 |
| `FUN_0048b8a0()` | lives-remaining query; `>0` → respawn (`return 1`), `==0` → set game-over | 26761–26770 |
| `DAT_0062f64d = 1` | **game-over flag** (= coop.c `ADDR_GAMEOVER`) when out of lives | 26770 |
| `DAT_00626278 + 0x7c = 0` | **power reset** to 0 on respawn (confirms coop.c `RES_POWER` 0x7c) | 26787 / 26799 |
| `+0x930 / +0x934` | player X/Y reset to center on respawn (confirms `OFF_POS_X/Y`) | 26750–26751 |

**Resurrection design (mirrors th06's "hold focus + release shot near dead partner
for ~90 frames → spend a life to revive"):** while the partner is in state `2` inside
its deathbomb window (`+0x16a08 <= 29`), if the reviver holds the revive input for
the required frames, force the partner back to state `0`, clear `+0x16a08`/`+0x23f8`,
and **spend the REVIVER's life** (`FUN_0042d5cd(-1)` on the reviver) instead of
letting the partner's death commit. coop.c already intercepts the game-over flag for
P2 (ghost mode); resurrection replaces ghost mode by catching the partner *before*
the commit. The exact revive input, frame count, and whether revive should also work
mid-respawn (state 1/3 — i.e. from ghost mode, like the current F11) are tuning
calls for the live test.

> This section independently re-confirms coop.c's `OFF_STATE` (0x2408), `RES_POWER`
> (0x7c), `OFF_POS_X/Y` (0x930/0x934) and `ADDR_GAMEOVER` (0x62f64d).

## Open / unverified
- Hitbox/graze radius offsets, focus-mode flag, and the power-shot level field are not
  yet located (collision uses the leaf primitives FUN_0043e260/6b0 that coop.c detours,
  which are param-relative — so P2 collision works without knowing the radius offset).
  The graze box is at `+0x960/+0x964/+0x96c/+0x970` (cherry doc §7c).
- ~~Whether `FUN_00441fb0`'s sub-calls pass `ECX = the cloned P2`~~ ✅ **RESOLVED
  (2026-06-12, disasm + in-game):** all sub-calls (incl. movement `FUN_0043ee50`)
  pass ECX = the saved player, so they DO drive the P2 clone (handoff §5d).

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
| `+0x948`/`94c`/`950`/`954`/`958`/`95c` | float | **player HIT box edges** Xmin/Ymin/Zmin/Xmax/Ymax/Zmax (recomputed each frame = center `+0x930/4/8` ∓ the half-extents below) | built @26365-26370; tested by FUN_0043e260:25995 |
| `+0x990`/`994`/`998` | float | **HIT box half-extents** X / Y / Z (init `cfg[0xc]/2`, `cfg[0xc]/2`, `5.0` @27388-27390; cfg = `DAT_00575948`). Also used directly by the rotated-frame LASER test FUN_0043e6b0:26099. ⚠️ coop.c mislabels `0x994` as `OFF_SPD_UNF` (speed) — it's the hit half-Y. | 26099/26365-69; 27388 |
| `+0x960`/`964`/`96c`/`970` | float | **player GRAZE box edges** (recomputed = center ∓ the graze half-extent; the graze TEST adds a further +20px) | built @26372-26375; FUN_0043e3b0:26035 |
| `+0x99c`/`9a0` | float | **GRAZE box half-extents** (init `cfg[0x10]/2` @27391). ⚠️ coop.c mislabels `0x9a0` as `OFF_SPD_FOC`. | 26372-75; 27391 |
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

## Shot / laser / bomb subsystem (2026-06-14 mapping pass)

The player's own shots live IN the player struct and are created/moved/drawn
param-relative (so P2's shots live in P2's clone — coop.c relies on this).

| Field | Meaning | src |
|---|---|---|
| `+0x2444` + i·`0x364` | shot slot array, 96 slots | coop.c `OFF_SHOTS` |
| slot `+0x34a` | u16 active flag (0 = free) | FUN_0043c700:25369 |
| slot `+0x34e` | i16 **laser-slot index** into the owner's laser table | FUN_0043c700:25351 |
| slot `+0x350` | i16 option index (which option fired it) | 25377 |
| slot `+0x1c0` | render flags (bit0xd = needs anm rebind) | 25352 |
| slot `+0x1c6` | anm rebind request (same field as any anm sprite obj) | 25353 |
| `+0x169d0` + idx·`0x10` | **laser-slot pointer table** (owner-side; the slot ptr a live laser is bound through — moving the slot orphans it, which broke MarisaB until coop.c kept P2 shots home) | 25090-25368, FUN_0043d2f0 |
| `+0x169cc` + idx·`0x10` | per-laser life/timer (`<1` ⇒ retire the slot) | 25357-25368 |
| `+0x169c4` + idx·`0x10` | per-laser sub-fields (countdown sentinel `0xfffffc19`) | 25358-25366 |
| `+0x16a20` | bombing flag (nonzero while a bomb runs) | OFF_BOMBING; 25356 |
| `+0x16a24` | bomb sub-state (selects the bomb update fn ptr) | (struct table above) |
| `+0x16a40` / `+0x16a48` | bomb update fn pointers (called indirectly) | (struct table above) |

### Bomb bullet-clear = per-player CLEAR-REGION array `+0x17dc` (2026-06-16)
PCB's bomb clears bullets through **clear-region slots stored ON THE PLAYER** at
`player+0x17dc` — NOT a global sweep and NOT `+0x2400` (that's the cherry-border sweep
timer). The per-character bomb callbacks (PCBdecomp.c 5708–7700, `__fastcall(player)`)
register regions as their spell objects move.

| Field | Meaning | src |
|---|---|---|
| `+0x17dc` + i·`0x20` | clear-region slot, **96** of them | FUN_0043e0a0:25958 |
| slot `[0]/[1]` (x/y) | region center (world) | FUN_004418b0/00441800 |
| slot `[2]/[3]` (w/h) | box size (`[2]==0` ⇒ treat as circle) | FUN_0043e0a0:25970 |
| slot `[4]` | circle radius | FUN_0043e0a0:25965 |
| slot `[7]` | clear-type (written into `player+0x2404`) | 25966/25972 |
| slot `+0x18` (int) | region lifetime; decremented per frame, expires at <1 | FUN_00440940:26636 |

- **Register:** `FUN_004418b0` @0x004418b0 (circle: sets `[4]=radius`) and
  `FUN_00441800` @0x00441800 (box: sets `[2]=w,[3]=h`), both `__thiscall(ECX=player,
  pos*, …)`; scan `+0x17dc` for a free slot (`[2]==0 && [4]==0`) and fill it.
- **Test:** `FUN_0043e0a0` @0x0043e0a0 `__thiscall(player, bulletPos, bulletSize)`
  walks all 96; a bullet inside any region ⇒ returns **2**. It runs at the top of the
  bullet HIT test `FUN_0043e260` (25992): `if (FUN_0043e0a0(...) != 0) return 2` ⇒ the
  bullet update sets state 5 (clearing) + drops a scratch item.
- **Maintain:** `FUN_00440940` @0x00440940 `__fastcall(player)` decays each `+0x18`.
- **Co-op (B4):** all player-relative, and coop already re-invokes `FUN_0043e260` for
  P2 (reads `P2+0x17dc`). coop.c's `HookedAddClearCircle`/`HookedAddClearBox` force
  ECX→P2 while `s_inP2Update`, so P2's bomb populates `P2+0x17dc`.

- **`.sht` character shot config:** the per-character shot/bomb setup fns run at
  PCBdecomp.c 5932-7625 (one block per char×type; each clears `+0x16a20`). They
  install the shot-spawn callbacks + the bomb fn ptrs (`+0x16a40/48`) for the
  selected character — i.e. the data coop.c swaps when P2 picks a different char
  (`ApplyP2Selection`). The full `.sht` binary layout isn't mapped here.
- **Shot→enemy damage:** `FUN_0043d9e0(player, pos, size, &flag)` sweeps this shot
  array against an enemy box and returns the frame's damage — the function the enemy
  update calls (`th07_enemy_system.md` §3) and coop.c's boss-HP divisor targets.
- **8c (SakuyaA cross-char aim):** the aimed (non-homing) shot reads an aim target
  the enemy update fills keyed off the GLOBAL char id into static P1's block only;
  for a different-char P2=SakuyaA the source isn't per-player (coop.c mirrors P1's
  block each frame). A proper fix would give P2 its own aim-target fill — start from
  the enemy-update site that writes P1's aim block and the shot-spawn callback that
  reads it.

## Open / unverified
- **Hit/graze boxes LOCATED (2026-06-15):** the player collision is an AABB, not a
  radius — the bullet/contact leaf `FUN_0043e260` (25995-25998) compares the bullet
  against hit-box edges `+0x948`(L)/`+0x94c`(T)/`+0x954`(R)/`+0x958`(B); the graze
  leaf `FUN_0043e3b0` (26035-26039) uses the wider `+0x960/+0x964/+0x96c/+0x970`
  box. The edges are recomputed each frame (`FUN_00441fb0` body @26365-26375)
  from the player center (`+0x930/4/8`) ∓ the **half-extents**: hit = `+0x990`(X)/
  `+0x994`(Y)/`+0x998`(Z), graze = `+0x99c`/`+0x9a0`. Init @27388-27392 from the
  `DAT_00575948` config (`cfg[0xc]/2` for hit X&Y, `5.0` for Z, `cfg[0x10]/2` for
  graze). The rotated-frame LASER test `FUN_0043e6b0` (@26099) uses `+0x990/+0x994`
  directly. ⇒ the raw half-extent fields are now LOCATED; coop.c's old
  `OFF_HITBOX 0x23f8` was a death-timer mislabel (`th07_player_shot_bomb_system.md`
  §5). **⚠️ coop.c also mislabels `+0x994`/`+0x9a0` as `OFF_SPD_UNF`/`OFF_SPD_FOC`
  (speed)** — the box-build proves they're the hit/graze half-extents; the real
  movement-speed field is then unconfirmed (coop.c's full-clone SpawnP2 copies both
  regardless, so its behaviour is unaffected). Confirm before reusing those names.
- Focus-mode flag = `+0x240b` (✅, `th07_player_shot_bomb_system.md` §2); the
  power-shot level field is not yet located.
- ~~Whether `FUN_00441fb0`'s sub-calls pass `ECX = the cloned P2`~~ ✅ **RESOLVED
  (2026-06-12, disasm + in-game):** all sub-calls (incl. movement `FUN_0043ee50`)
  pass ECX = the saved player, so they DO drive the P2 clone (handoff §5d).

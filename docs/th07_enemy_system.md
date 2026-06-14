# TH07 (PCB) enemy system (manager, struct, ECL tick, damage)

Reverse-engineered 2026-06-14 (general mapping pass). Addresses build-specific to
`th07.exe` ver 1.00b (SHA256 `35467EAF…E80CA`). Pairs with
`th07_bullet_system.md` (the bullets enemies fire) and `th07_boss_hp_scaling.md`
(the damage lever co-op uses). Verified-vs-decomp facts are marked; deeper ECL VM
internals are flagged as a future target.

## 1. Enemy manager / update

| What | Address / value |
|---|---|
| Enemy update (per-frame task) | `FUN_00420620` (@0x420620), `__fastcall(ECX = param_1)` |
| ECL script VM tick (one enemy) | `FUN_00410520` (@0x410520) → returns `-1` when the script ends |
| Movement / kinematics step | `FUN_004203b0` (@0x4203b0), `FUN_0041e920` (@0x41e920) |
| Enemy death/cleanup | `FUN_004202d0` (@0x4202d0) |
| Per-shot DAMAGE vs this enemy | `FUN_0043d9e0` (@0x43d9e0) — returns frame damage (the co-op divisor target; see boss-HP doc) |
| On/off-screen box test | `FUN_0042d6d8` (@0x42d6d8) `(x, y, w, h)` |
| ECL set-life opcode | `FUN_00424290` (@0x424290) — writes the HP cap at enemy `+0xd30` |

`FUN_00420620`'s `param_1` is the **enemy-manager context base** — the whole
manager lives in the `0x009545xx…009547xx` global region, addressed as
`&DAT_009545bc + param_1`, `&DAT_00954600 + param_1`, `&DAT_009546fc + param_1`
(a frame/RNG-sync counter the anti-tamper `FUN_00404fe0` checks every 200 frames),
etc. `param_1` is the base offset for the active game context.

### The enemy array
- First slot at **`param_1 + 0x4f50`**; slot **stride `0x4f48`** bytes.
- The update iterates **up to 480 (`0x1e0`) slots** each frame; a slot whose
  `+0x2e28` high bit is clear is inactive and skipped (line 12687).
- Active-enemy count this frame accumulates at `&DAT_009545bc + param_1`.

## 2. Per-enemy struct (offsets relative to a slot)

Verified from `FUN_00420620` (12664-13099) unless noted.

| off | type | meaning |
|---|---|---|
| 0x1b8 | u32 | sprite tint 0xAARRGGBB (same field name as player/bullet) |
| 0x1e4 | ptr | anm sprite object; `*(+0x1e4)+0x2c/+0x30` = sprite px H/W (for the box test) |
| 0x250/0x254 | float | (on the `+0x2eb0` child obj) trailing position lerp targets |
| 0x2b0c / 0x2b10 / 0x2b14 | float | position X / Y / Z |
| 0x2b18 / 0x2b1c / 0x2b20 | float | previous/secondary position (snapshotted into the hitbox trail) |
| 0x2b3c | float[3] | primary collision box (passed to `FUN_0043d9e0` / shot damage) |
| 0x2b48 | float[3] | secondary collision box (a 2nd damage region when its [0] > 0) |
| 0x2b54 | u32 | trail extra (copied into the trail history) |
| 0x2bcc | u32 | flags (bit0 used in the damage-cap special-casing) |
| 0x2bd0 | u32 | saved tint scratch (restored around `FUN_00450d60`) |
| 0xd30 | int | **HP / life cap** (set by the ECL set-life opcode `FUN_00424290`) |
| 0x2e28 | u8 | slot-in-use byte — **high bit set ⇒ active** (clear ⇒ free slot) |
| 0x2e29 | u8 | state flags: bit0 alive, bit1 collidable-with-shots, **bit3 invuln/no-collide**, bit4 takes-damage, bit6 boss |
| 0x2e2a | u8 | touch flags: **bit3 = inside/over the playfield** (set by the box test), high bit = keep-alive-offscreen |
| 0x2e2b | u8 | misc flags: bit0 skip-update, bit3 = clear-on-bomb/timeout |
| 0x2eb0 | ptr | linked child object (option/familiar) followed with a 0.0625 position lerp |
| 0x2edc | int | a sub-update enable (`-1` gates the `FUN_0041ff80` extra tick) |
| 0x2f78 + i*0x1c | float[] | **hitbox-trail history** (multi-segment / laser-body bosses) |
| 0x424 + i*0x24c | i16 | per-sub-object (2×) state words (the `0x24c`-stride children at +0x24c/+0x498) |
| 0x4f30 | u8 | multi-segment hitbox ENABLED |
| 0x4f32 | i16 | hitbox segment count |
| 0x4f34 | i16 | total segment span (used for the spread damage loop) |

## 3. Per-enemy flow each frame (`FUN_00420620` inner loop)

1. Skip if inactive (`+0x2e28` high bit clear).
2. If flagged clear-on-bomb (`+0x2e2b` bit3) and a clear/bomb is active
   (`DAT_004d44f8 != 0 || DAT_004bfee0`): kill it.
3. **ECL VM tick** `FUN_00410520(enemy)` — runs the enemy's danmaku script
   (movement, bullet fire via `FUN_00423730`, set-life via `FUN_00424290`, etc.).
   Returns `-1` ⇒ script done ⇒ `FUN_004202d0` death/cleanup, slot freed.
4. Kinematics (`FUN_004203b0`/`FUN_0041e920`), child-object follow lerp (`+0x2eb0`).
5. Shift the **hitbox-trail history** (`+0x2f78`, stride 0x1c) and write the new
   head from the current position — this is how stretched/laser-body and
   multi-hit bosses get several collision points.
6. Off-screen test `FUN_0042d6d8(pos, sprite W/H)` → set `+0x2e2a` bit3 / despawn.
7. **Damage sweep** (when alive+takes-damage, `+0x2e29` bits 0|4):
   `dmg = FUN_0043d9e0(enemy+0x2b0c, enemy+0x2b3c, &flag)` (+ the secondary box
   `+0x2b48` if active). The result is scaled by rank (`iVar9 = 0x1e - local_14`,
   `local_14 = 2×stage-difficulty`, capped at 0x46) and subtracted from HP.
   **This `FUN_0043d9e0` return is exactly what `coop.c` divides by player count**
   for the Tier-1 boss-HP scaling (see `th07_boss_hp_scaling.md` §5).

## 4. Co-op relevance
- `FUN_0043d9e0` is `__thiscall(player, pos, size, &outflag)` swept over the
  player's shot array; both players' shots flow through P1's array, so dividing the
  return halves the doubled team DPS back to vanilla pacing.
- Bullets fired by enemies (`FUN_00423730`) gate the player hit test behind the
  bullet graze flag — see `th07_bullet_system.md` §4 and the coop.c collision hooks.

## 5. Open / next RE
- The **ECL VM** (`FUN_00410520` + the opcode table) — the scripting language that
  drives every enemy/boss pattern. **⚠️ NOT available in this dump:** Ghidra emits
  `Unable to decompile 'FUN_00410520'` (PCBdecomp.c:8840) — almost certainly a
  huge computed-goto opcode dispatch it couldn't lift. Mapping it needs either the
  user's fuller Desktop `th07.exe.c` db, a raw disassembly of 0x410520, or the
  public thtk/ECL format docs (PCB ECL is well-documented in the touhou RE
  community). Don't burn time trying to read it from PCBdecomp.c.
- The 8-byte gap between the first slot (`+0x4f50`) and the stride (`0x4f48`) is
  just header alignment — not re-derived here.
- Boss-specific fields (spell-card timer, name plate, the `+0x424`/`+0x498`
  sub-objects) only partially identified.

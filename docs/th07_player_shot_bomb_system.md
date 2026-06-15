# TH07 (PCB) Player Shot / `.sht` / Bomb System — RE map

Maps the player's own shot-fire path, the `.sht` shot-data buffers, the per-shot
slot struct, and the bomb / spell-card dispatch. For the co-op mod this underpins
per-player shots (already partly used in `src/coop/coop.c`) and the future
per-player bomb / char-over-wire work.

**Binary:** th07.exe ver 1.00b, SHA256 `35467EAF…E80CA` (handoff §0). All
addresses are build-specific to that hash. `FUN_<hex>` / `DAT_<hex>` are Ghidra
labels in `PCBdecomp.c`; the hex **is** the VA. Line numbers cite `PCBdecomp.c`.

**Confidence key:** ✅ = read + cross-checked this session (or already verified in
coop.c by the running mod); 🟡 = decomp-derived sketch, plausible but NOT
independently confirmed — verify before relying. The §6 appendix is entirely 🟡.

---

## 1. Anchors (✅ verified — from coop.c, exercised in-game)

| What | Addr / offset | Notes |
|---|---|---|
| Player object base | `0x004bdad8` | static P1 |
| Player UPDATE | `FUN_00441fb0` | `__fastcall(ecx=player)`; reads input, fires, moves |
| Player DRAW | `FUN_004420b0` | `__fastcall(ecx=player)` |
| `.sht` loader | `FUN_00442b70` | ECX = `&player+0xb7e70`; loads a `.sht` into a buffer |
| Unfocused `.sht` buffer ptr | player `+0xb7e70` | `void*` |
| Focused `.sht` buffer ptr | player `+0xb7e74` | `void*` |
| Baked unfocused speed | player `+0x994` | `= sht[+0xc] / 2` |
| Baked focused speed | player `+0x9a0` | `= sht[+0x10] / 2` |
| Baked hitbox/“power cap” field | player `+0x23f8` | `= sht[+0x8]` — **see §5 caveat** |
| Shot array first slot | player `+0x2444` | |
| Shot slot stride | `0x364` (868 B) | |
| Shot slot count | `0x60` (96) | |
| Per-slot active flag | slot `+0x34a` (u16) | `0` free, `1` active, others = special (see §3) |

`.sht` files on disk: `data/ply{00,01,02}{a,b}[s].sht` — char `00/01/02` =
Reimu/Marisa/Sakuya, type `a/b`, optional `s` = focused variant. The mod loads
them via `FUN_00442b70` and caches the pairs (`coop.c` `s_shtCache`).

---

## 2. Shot-fire input gate (✅ verified — PCBdecomp.c:26584, inside the player update)

```c
if (((DAT_004b9e50 & 1) != 0) && (FUN_0042ad66() == 0)) {   // shot bit + NOT bombing
    if (FUN_00404fe0() == 0)                                 // fire-cadence/state gate
        FUN_0043d990();                                      // ← shot-fire dispatch
    ...                                                      // (unfocused-aim relax below)
}
```
- `DAT_004b9e50` = `g_InputGameplay`; bit `0x1` = shot (handoff §1).
- **`FUN_0042ad66(player)` = the “is a bomb active?” predicate (✅ PCBdecomp.c:16632):**
  reads `*(player+8)` (the score/stat manager ptr) and returns `1` when
  `*(mgr+0x1fbac) < 0 && != -2`, else `0`. So shots are suppressed while bombing.
  (Called from many sites: 25355/25411/26667/27057/27632…)
- `FUN_00404fe0()` — fire-cadence / invuln-ish gate (🟡 exact predicate not read).
- **`FUN_0043d990` is the fire entry called here** (✅ call site 26587). Its
  Ghidra body at 25821 is small (resets `+0x169f4/8/c`), so the actual
  slot-spawn likely tail-calls / inlines into the surrounding fire code — treat
  `FUN_0043d990` as the *entry*, the spawn loop itself is 🟡 (see §6).

Focus vs unfocus selects which `.sht` buffer (+0xb7e70 vs +0xb7e74) and which
baked speed; the focus bit is `0x4` and the player tracks a focus state byte
(coop.c uses player `+0x240b` as the focus/shot-variant selector — 🟡 for shots).

---

## 3. Per-shot slot struct (player+0x2444, stride 0x364) — offsets ✅ via the damage sweep

`FUN_0043d9e0` (the player-shot **damage sweep**, `__thiscall(player, pos, size,
out)`, PCBdecomp.c:25836 — the fn coop.c divides for HP scaling) walks all 96
slots (`base = player+0x2444`) and reads these, so the offsets below are
**confirmed**:

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x24c` | f32 | shot position X (world) | ~25867 box test |
| `+0x250` | f32 | shot position Y (world) | ~25866 box test |
| `+0x318` | f32 | hitbox width  | ~25867 (`±width*0.5`) |
| `+0x31c` | f32 | hitbox height | ~25866 (`±height*0.5`) |
| `+0x344` | i32 | counter masked `&0x80000001` for type 4/5 | ~25874 (cadence on special shots) |
| `+0x34a` | i16 | **active flag**: `0` free, `1` normal-collidable; the sweep also accepts a slot whose `+0x34c==3` | ~25863 |
| `+0x34c` | i16 | **shot type**: `1`/normal vs `3` (also swept), `4`/`5` = special (homing/laser-ish, cadence-gated via +0x344) | ~25863, ~25873 |

The sweep’s damage value per slot is applied by the caller (the per-enemy update
`FUN_00420620` subtracts the sweep return from enemy HP — see
`docs/th07_boss_hp_scaling.md`). The slot’s **damage / angle / velocity / sprite**
fields are filled at fire time (🟡 offsets in §6: damage `+0x348`, angle
`+0x334/+0x338`, velocity `+0x324/+0x328`, sprite `+0x1d8`).

---

## 4. Bomb + cherry-border handler `FUN_004409f0` (✅ verified — PCBdecomp.c:26652)

ONE function handles both the spell-card bomb AND popping the cherry border
(matching the coop.c header note: “ZUN’s bomb handler casts a spell outside the
border, and DURING the cherry border it just pops the border early”).
`__fastcall(player)`:

```c
if (player+0x240d == 0 || player+0x16a20 != 0 || (DAT_004b9e50 & 2) == 0) {
    if (player+0x240d == 2) FUN_00441960();          // (border-start path)
    if (player+0x23fc != 0)  player+0x23fc -= 1;      // post-bomb cooldown ticks down
    if (player+0x16a20 == 0) {                          // not already bombing
        if (FUN_00404fe0()==0 && FUN_0042ad66()==0 &&  // gates + not-bombing
            player+0x23f8 != 0 && FUN_0048b8a0()>0 &&   // (power/stock) + bomb-count>0
            player+0x23fc == 0 && (DAT_004b9e50 & 2)) { // cooldown clear + bomb bit
            ... launch ...
        }
    } else { ... per-frame bomb tick ... }
} else {
    FUN_00441bd0(1);          // border ACTIVE + bomb pressed -> pop the border
}
```

- `DAT_004b9e50 & 2` = bomb bit. `player+0x240d` = border request/active flag
  (coop.c `OFF_BORDER_REQ`). `FUN_00441960` = border-start, `FUN_00441bd0` =
  border-break (both coop.c-confirmed).
- **Bomb launch (26671-26695) sets these player fields (✅ read):**

| Offset | Set to | Meaning |
|---|---|---|
| `+0x16a24` | `(int)player+0x240b` | focus index (0 = unfocused, 1 = focused) chosen for the bomb |
| `+0x16a20` | `1` | **bombing flag** (coop.c `OFF_BOMBING`; `FUN_0042ad66` reports it) |
| `+0x23dc` | `1` | frame-update flag |
| `+0x16a38` | `0` | bomb frame counter |
| `+0x16a34` | `0` | (frame component) |
| `+0x16a30` | `0xfffffc19` (-999) | prev-frame sentinel |
| `+0x16a28` | `999` | bomb duration (frames; char code may override) |
| `+0x23f8` | `+= 6`, capped at `*(DAT_00575948+8)` | power/stock field bumped on bomb — **see §5** |

- **Per-character bomb callbacks** dispatched by focus index `+0x16a24`:
  `+0x16a3c` (unfocused) / `+0x16a44` (focused) (✅ called at 26683/26686; also
  per-frame at 26712/26715 and in draw at 27253/27256). These pointers are
  installed at player init from a global table (🟡 `PTR_FUN_0049ec50..5c`).
- Side effects on launch: `FUN_0043b7a0(1)`, `FUN_0042d612(-1)` (clear bullets /
  sfx), and replay-flag `DAT_004b9e48+0xd6 |= 1`.
- `FUN_0048b8a0()` returns the available **bomb count** (the resource a bomb
  actually spends — separate from `+0x23f8`); `> 0` is required to bomb.

---

## 5. player `+0x23f8` is the death / deathbomb-window timer (✅ corrected this session)

`coop.c` labels `OFF_HITBOX = 0x23f8` (“= sht[+0x8]”), but reading the dying-state
update **`FUN_00440cf0` @ PCBdecomp.c:26730** shows `+0x23f8` is an **int
countdown**, NOT a float hitbox, and NOT the power counter (power is a float in the
resource struct at `res+0x7c` = coop.c `RES_POWER`, range 0..128):

- It is set to a **config max `*(DAT_00575948+8)`** at player init / respawn
  (26856/26984/27425). (So coop.c’s “= sht[+0x8]” is the wrong *source* too — the
  max comes from the `DAT_00575948` global config, not the per-player `.sht`.)
- While the player is DYING, `FUN_00440cf0` does `+0x23f8 -= 1` each frame (26778).
  When it hits 0 it **finalizes the death** (26779+): drops power to 0
  (`*(DAT_00626278+0x7c) = 0`), runs the heal canary `FUN_004012b0`, and spawns
  power items at the death spot via `FUN_004326f0(player+0x930, type 4, 2)` — i.e.
  this is the **partial-power-drop-on-death** the co-op phantom-spare path relies on.
- The bomb handler’s precondition `+0x23f8 != 0` (26668) is therefore the
  **deathbomb gate**: you may bomb while the window timer is still running; the
  `+= 6` on launch (26692, capped at the same config max) tops the window up. While
  alive the timer sits at max, so the gate is trivially satisfied.

**This agrees with `docs/th07_player_struct.md`**, which already lists `+0x23f8` as
the “respawn timer — set on death, decremented each frame; hits 0 → respawn (drop
power items)” citing the same 26778–26779. So the repo’s own ground truth already
had it right; only coop.c’s `OFF_HITBOX` *name* is misleading.

**Action for coop.c (cosmetic):** rename `OFF_HITBOX` → `OFF_DEATH_TIMER`. Its
*usage* is fine — `SpawnP2` sets P2’s `+0x23f8` to `sht[+8]` / copies P1’s
(26-1255/1284), i.e. max-inits a live P2’s death-window timer, which is correct.
The mod’s real player hitbox is a separate `.sht`-derived field read by the
collision leaves (`FUN_0043e260` family); confirm which offset before reusing
`0x23f8` for collision.

---

## 6. Decomp sketch — `.sht` format & extra slot/bomb fields (🟡 ALL UNVERIFIED)

From an assisted read of `FUN_00442b70` / `FUN_0043bcc0` / the fire path. Treat as
a starting hypothesis; each needs confirmation (the assist also mislabeled some
items, since corrected above). **Do not cite as ground truth.**

**`.sht` header** (buffer base): `+0x02` u16 = pattern-index count; `+0x34` =
array of `{threshold, pattern_index}` pairs selected by power/frame; per-pattern
chains of fire-entry records reached via 8-byte slots. Baked header fields
`+0x08`/`+0x0c`/`+0x10` are the only ones coop.c relies on.

**`.sht` per-fire-entry record** (stride ~0x1a, `<0` at +0x00 terminates):

| Off | Type | Guess |
|---|---|---|
| +0x00 | i16 | fire timing modulo (`frame % e[0] == e[1]`, 25071) |
| +0x02 | i16 | frame offset within pattern |
| +0x04 | f32 | spawn offset X |
| +0x08 | f32 | spawn offset Y |
| +0x0c | f32 | velocity X |
| +0x10 | f32 | velocity Y |
| +0x14 | f32 | angle → slot +0x334 |
| +0x18 | f32 | speed magnitude |
| +0x1c | i16 | damage → slot +0x348 |
| +0x1e | i8  | spawn source (0 base / 1 unfoc-rel / 2 foc-rel) |
| +0x1f | i8  | shot type/script → slot +0x34c |
| +0x20 | i16 | sprite/anim id |
| +0x22 | i16 | sound id (-1 none) |

**Extra shot-slot fields (🟡):** `+0x1d8` sprite id, `+0x254` z/scale, `+0x324`
vel X, `+0x328` vel Y, `+0x334`/`+0x338` angle, `+0x340` age, `+0x344` homing/
cadence counter (✅ used by the sweep), `+0x348` damage, `+0x34e` homing-array
index, `+0x354`/`+0x35c` update/collide callbacks, `+0x360` `.sht`-entry ptr.

**Homing / aim targets (player, ✅ offsets from coop.c §5j):** `+0x2428` homing
xyz, `+0x2434` aim xyz, `+0x2440` valid flag — filled by the enemy update for P1
and (in the mod) per-player via `BuildP2TargetBlock`.

**Spell-card declaration** (the on-screen portrait + spell name): created by
`FUN_0042868d` (✅ coop.c — sets portrait sprite + UV at set-time, `DAT_00575ab4`),
drawn by `FUN_0042c577` (✅ coop.c, PCBdecomp.c:17267). Face id window
`[0x4a0,0x4ad)`; `data/face_{rm,mr,sk}00.anm` (handoff §8b).

---

## 7. Next RE steps
- Read the actual shot-spawn loop (the body reached from `FUN_0043d990` @26587 /
  the surrounding fire code) to confirm the §6 slot fields and the `.sht`
  power→pattern selection.
- Find the real player hitbox field the collision leaves (`FUN_0043e260` family)
  read (now that `0x23f8` is confirmed to be the death timer, §5), and rename
  coop.c’s `OFF_HITBOX`.
- Map `FUN_0048b8a0` (bomb-count source) + where a bomb actually decrements the
  bomb stock — needed for the per-player bomb-count work.

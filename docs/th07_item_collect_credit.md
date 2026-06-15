# Item-collection credit map + per-player resource attribution (verified)

This pins exactly where collected items credit each resource, so per-player
**power / bombs / points** (and later cherry) can be attributed to whoever
collected the item. Read straight from `PCBdecomp.c`. Build-specific to
th07.exe ver 1.00b (SHA256 `35467EAF…E80CA`).

It is the RE backing for the per-player-cherry decision (cherry doc §6) and
answers "is P2 power separate?" — **the counter exists, but item pickups still
credit the shared pool; this doc is how to fix that.**

---

## 1. The collect loop and the overlap test

- **`FUN_00432990` @ `0x00432990`** (`__fastcall(item_manager)`, PCBdecomp.c:20329)
  is the per-frame **item update + collect loop**. It walks up to 1100 item slots
  (`local_28` = item, stride **`0x288`**, active flag `+0x27d`), moves each item,
  then runs the player-overlap test and, on a hit, credits resources.
- **`FUN_0043e4e0` @ `0x0043e4e0`** (PCBdecomp.c:26056) is the **player-area overlap
  test** it calls per item (`__thiscall`, ECX = player; the decompiler hides ECX so
  it reads as `FUN_0043e4e0(item+0x24c, &size)`). Returns 1 if the item box overlaps
  the player's collect box (`+0x978/+0x97c/+0x984/+0x988`) and player state ∈ {0,3,4}.
  coop.c already detours this so a live P2's overlap also collects (for the team) —
  `HookedCollectOverlap`.

## 2. What each item type credits (the switch on `item+0x27c`, PCBdecomp.c:20448+)

Resource struct = `DAT_00626278` (the value at `0x626278`; reached by accessors as
`*(singleton 0x626270 + 8)`). Score/point fields are written **absolutely** here;
power/bombs/lives go through **accessors** (which carry ZUN's anti-tamper checksum).

| `+0x27c` | Item | Credit (all to the SHARED struct today) |
|---|---|---|
| 0 | Power (small) | `FUN_004325e0(1)` → power `+0x7c += 1`; if maxed, power=128 + point bonus → score `+0x04` |
| 1 | Point | cherry-scaled point value → score `+0x04`; point-item counters `+0x24 += 1`, `+0x28 += 1` (extend tracking) |
| 2 | Power (big) | `FUN_004325e0(8)` → power `+= 8` |
| 3 | Bomb | `FUN_0042d612(1)` (bomb-add accessor) |
| 4 | Full Power | power `+0x7c = 128.0`; score `+0x04 += 100` |
| 5 | 1-up / life | `FUN_0042d83a()` (extend) |
| 6 | (star/score) | point value → `+0x04` |
| 7 | Cherry item | point value (gated on `DAT_0062f888 <= DAT_0062f88c`) → `+0x04`; **only reads cherry, does not add to `+0x88`** |
| 8 | (sfx-only) | `FUN_0042f5a2/69f` |

**Accessors of interest** (each runs the anti-tamper check `FUN_00404fe0` at entry
and re-heals via `FUN_004012b0` — see coop.c's resource notes):
- `FUN_004325e0` @ `0x004325e0` — **power add**: `*(float)(res+0x7c) += amount`
  (`__thiscall`, ECX = singleton `0x626270`, stack arg = amount). PCBdecomp.c:20186.
- `FUN_0042d612` @ `0x0042d612` — **bomb add** (case 3).
- `FUN_0042d83a` @ `0x0042d83a` — **1-up / extend** (case 5).

## 3. CRITICAL: cherry is NOT credited by item collection

PCB has **three** cherry values — Cherry (`DAT_0062f88c`), Cherry Max
(`DAT_0062f888`), Cherry+ (`DAT_0062f890`) — each a delta from the shared base
`*(int*)(DAT_00626278+0x88)`. The border is **Cherry+**; the point-item value is
**Cherry**. Full model in `docs/th07_cherry_determinism.md` §0.

The collect loop only **reads** Cherry (to scale point-item values, cases 1 & 7); it
adds to **no** cherry value. The base `+0x88` is written only once (a reset, 18124),
and cherry-*gain* moves the `DAT_0062fXXX` raws (e.g. `DAT_0062f88c` decremented at
PCBdecomp.c:12491/26707/26821; Cherry+ recomputed from the per-player border timer
at 26914).

⇒ **Per-player cherry is a DIFFERENT attribution problem than per-player power/bombs.**
Power/bombs come from this item-collect loop. Cherry fill comes from the
`DAT_0062fXXX` sites (shot/graze path — **not yet traced**), and the border (Cherry+)
is per-player-timer-driven and clobbered by P2's piggyback update (cherry doc §0).
So **separate power/bombs is implementable now** from the map above; **separate
cherry display + a shared border** need the cherry-gain trace + the `DAT_0062f890`
reconciliation first.

## 4. Attribution design (power/bombs first) — determinism- & tamper-safe

The collector is known at the overlap test: `HookedCollectOverlap` runs P1's test;
if P1 missed but a live P2 overlaps, P2 collected it. The credit then happens in the
*same loop iteration*, right after the overlap returns. So:

1. In `HookedCollectOverlap`, set `s_curCollector = 1` (P1 hit), `2` (P2 hit), or
   `0` (neither). It stays valid until the next item's overlap test.
2. Detour the credit accessors (`FUN_004325e0` power, `FUN_0042d612` bomb). In each,
   when `s_curCollector == 2 && s_p2SepRes`, redirect the credit to P2 using the
   **same field-swap+heal pattern coop.c already uses** (proven, runtime-validated):
   save P1's value, write P2's into the struct, **`FUN_004012b0` to re-heal the
   checksum so the accessor's entry tamper-check passes**, call the original (it adds
   + re-heals for P2's value), capture back into `s_p2Power/Bombs`, restore P1's,
   heal again. Else call the original unchanged.
3. Points/score stay shared (team score). 1-up (case 5) → decide later (shared vs
   per-player lives interacts with ghost mode).

**Determinism:** `s_curCollector` derives from P2's synced position, so both netplay
machines set it identically and credit `s_p2*` identically; the shared struct also
changes identically. No desync. (Gameplay does change — P2's items feed P2, not the
team pool — but deterministically.)

**Anti-tamper:** `FUN_004325e0`/`FUN_0042d612` validate the checksum on entry and
fill a cap array with `0xffffffff` (→ crash) if it's inconsistent. The heal dance is
required (coop.c's existing pattern).

> **⚠️ Determinism note (2026-06-12 overnight session, verified):** the heal
> `FUN_004012b0` recomputes the checksum (`+0xb0`), a copy (`+0xac`), a derived
> guard float at `player+0x9614`, **and two RANDOM canaries (`+0x3c`, `+0x98`) —
> consuming 2 shared-RNG calls** (`Rng_Next32` at PCBdecomp.c:2868/2870). Every
> heal advances the RNG counter `0x0049fe24`. Under lockstep netplay this is
> harmless — both machines run P2's identical sim and consume the same calls —
> but the co-op RNG stream diverges from a vanilla one, and anything that makes
> P2's resource events differ *between machines* would desync exactly here.
> Validate with the counter oracle after the netcode is wired in.

> **✅ IMPLEMENTED & GAME-TESTED (2026-06-11).** coop.c does this via a whole-struct
> field-swap held across each P2-collected item's credit (see `HookedCollectOverlap`
> + `HookedItemLoop`), rather than per-accessor detours — that also covers the direct
> power writes. The user confirmed in-game: **P2's power is separate, grows only from
> P2's own pickups, and P2's shot-type levels up off its own power** — no anti-tamper
> crash. Bombs/lives ride the same swap. (Cherry/points stay shared.)

## 5. The item SPAWNER — `FUN_004326f0` (RE 2026-06-15, complements §1-2)

The other half of the item system: where items are CREATED (enemy deaths, the
death power-drop, the mod's 1up). `__thiscall`, PCBdecomp.c:20244,
VA `0x004326f0`:

```
FUN_004326f0(ECX=item_mgr, EDX, float pos[3], int type, int mode)
```
(coop.c models it `__fastcall`, so its `ADDR_ITEM_SPAWN(mgr, NULL/*EDX*/, pos3,
type, mode)` call is correct — `type` is the `+0x27c` value from the §2 table:
0/2 power, 1 point, 3 bomb, 4 full-power, **5 1-up**, 6 star, 7 cherry, 8 sfx.)

**Pool + cursor:** items live in the manager at `mgr + cursor*0x288`, cursor at
`mgr+0xae2e8`, scanned for a free slot (`+0x27d == 0`) up to 1100 slots, stride
**`0x288`** (matches §1). Cursor wraps; slots past index `0x44c` use the overflow
region at `mgr+0xae060` (a second pool — exact split not chased).

**Slot init (the fields the spawner writes):**

| Offset | Set to | Meaning |
|---|---|---|
| `+0x24c/+0x250/+0x254` | `pos[0..2]` | spawn world X/Y/Z |
| `+0x25c` | `0xc00ccccd` (-2.2f) | initial upward pop velocity |
| `+0x264/+0x268/+0x26c` | RNG (mode 2) | scatter TARGET x∈[48,336), y∈[-64,128) |
| `+0x270` | `0xfffffc19` (-999) | counter sentinel |
| `+0x27c` | `type` | **item type** (§2 switch reads this) |
| `+0x27d` | `1` | active flag |
| `+0x27e` | `1` | alive |
| `+0x27f` | `mode` (3→1, 4→0) | spawn/motion mode |
| `+0x1d8` | `type + 0x2c4` | sprite/anm id (script from `DAT_004b9e44+0x28ef0 + (type+0x2c4)*4`) |
| `+0x1b8` | `0xffffffff` | tint (opaque) |

**Modes (`param_4`):** `0` = plain pop-and-fall; `2` = **scatter** (random target
via two `FUN_00431900` (Rng_NextFloat) calls, then homes there — used by the
on-death power drop `FUN_00440cf0` and big enemy clears); `3`/`4` just preset the
`+0x27f` motion byte. The mod uses **mode 0** for its 1up (`Spawn1Up`).

**Max-power auto-convert (20255):** if `FUN_0048b8a0() > 0x7f` and `type ∈ {0,2}`
(power items), the type is rewritten to **7** (cherry/point item) — so power
pickups past max (power caps at 128) turn into points. **`FUN_0048b8a0` is a
float→int ROUND intrinsic** (PCBdecomp.c:81531, `ulonglong(void)` reading the x87
`ST0` register, ≈ `lroundf`), NOT a stored counter — the caller leaves the current
power float on the FPU stack first, so here it rounds power. That it reads as
`(void)` is the decompiler hiding the implicit x87 argument.

> **⚠️ Determinism (netplay):** mode-2 spawns consume **2 shared-RNG calls** each
> (the two `Rng_NextFloat` for the scatter target), advancing the counter
> `0x0049fe24`. The on-death power drop is mode 2, so deaths perturb the RNG
> stream — fine under lockstep (both machines run the identical sim) but a place
> to watch with the counter oracle once the netcode is live.

# TH07 (PCB) HUD / score-manager + anm sprite-draw pipeline

Reverse-engineered 2026-06-14 while implementing the §8a P2 icon HUD. All
addresses are build-specific to `th07.exe` ver 1.00b (SHA256 `35467EAF…E80CA`).
This is the ground truth for drawing engine sprites/text from injected code
(the P2 HUD, the §8b bomb portrait, any future sprite injection).

## 0. TL;DR for drawing from a DLL

- **Text** → `FUN_00402060(mgr=0x0134ce18, float pos[3], fmt, …)` (`__cdecl`
  varargs). Queues on the global ascii manager whose draw task runs **every
  scene** (menu + in-stage). Backbuffer, render-state-independent. Safe anywhere.
- **A baked anm sprite** → set its screen pos and call `FUN_0044f770(obj)`
  (`__cdecl`, self-validating → no-ops on an unbound object, so crash-safe).
  It APPENDS a quad to the anm sprite batch; the batch is flushed by
  `FUN_0044f5c0`. Sprites go to **whatever render target is bound in the current
  pass** — so to match the HUD you must draw inside the HUD pass (hook
  `FUN_0042b603`), not the playfield pass (the playfield has a clipped viewport).

## 1. The score-manager / HUD draw — `FUN_0042b603` (@0x42b603)

`void __fastcall FUN_0042b603(int singleton)` — the per-frame sidebar/HUD draw.
- `singleton` = the score-manager singleton at **0x626270**.
- `scoreStruct = *(singleton+8) = *0x626278` — a LARGE data block (~0x20a30
  bytes, zeroed at score-manager init). The resource VALUES live here at the
  offsets coop.c already uses: **lives +0x5c, bombs +0x68, power +0x7c** (floats),
  score +0x04, graze +0x14/+0x18, point-items +0x28/+0x30, checksum +0xb0.
  (The old "operator_new(200)" note in coop.c is a misread — the struct is big.)
- `*(uint*)(singleton+4)` = a **dirty bitfield**, 2 bits per HUD section:
  `[0:1]` lives, `[2:3]` bombs, `[4:5]` spell-practice counter, `[6:7]` power,
  `[8:9]` point-items. A section redraws only while its field is nonzero;
  each frame the drawn sections decrement by 1 (lines 17244-17259). Value
  accessors set the field to 2 on change.

### Persistent-surface model (important)

The HUD is composited on a **persistent surface**, not re-cleared each frame:
- The dark background tiles redraw only when the "full-dirty" condition holds
  (line 16943): `(DAT_00575a9c>>0xc & 1) || *(scoreStruct+0x1d70) != 0 ||
  DAT_00575ab4 != 0`. The last term is the bomb-portrait flag, so a bomb forces a
  full HUD repaint. The bg block paints tiles at **x 0 + x 416..624, y 16..464**
  (covers the whole right sidebar) and then sets every section's dirty field to 2.
- **`scoreStruct+0x1d70` is the "force full redraw" lever** — set it nonzero and
  the next `FUN_0042b603` repaints the entire sidebar. Used by §8a to erase a
  dropped P2 icon from the persistent surface.

### Lives / bombs icon rows

```c
// lives (gated by dirty[0:1]); bombs identical at +0x16f8, Y=112
obj = scoreStruct + 0x14ac;                 // baked life-icon sprite object
for (x = 496.0; i < count; i++, x += 16.0) {
    *(float*)(obj+0x1c8) = x;                // screen X
    *(float*)(obj+0x1cc) = 96.0;            // screen Y  (bombs: 112.0)
    *(u32  *)(obj+0x1d0) = 0x3eeb851f;       // z-depth (~0.46)
    FUN_0044f770(obj);                       // append one icon quad
}
```
- Life-icon object: `scoreStruct+0x14ac`. Bomb-icon object: `scoreStruct+0x16f8`.
  Both are pre-baked (already bound to the right `front.anm` id + scale), so you
  only set position; the SIZE comes from the object's own scale (+0x18/+0x1c) ×
  the sprite's pixel dims and is left untouched.
- `count` comes from `FUN_0048b8a0()` (the per-row count getter). For an injected
  P2 row, just loop your own count instead.
- Other baked objects in `scoreStruct`: `+0x1b90` 32px bg tile, `+0x1ddc` row
  label/divider, `+0x498/+0x6e4/+0x930/+0xb7c/+0xdc8/+0x1014/+0x1260` the sidebar
  frame decorations (drawn at 16982-16990), and `*(scoreStruct)` the main frame.

### Power / point-items / score (text)

All via `FUN_00402060` (ascii) at x=496: score (496,64), hi-score (496,48),
**power (496,160)**, **point-items "%d/%d" (496,176)** — the lowest line; §8a
places P2's rows just below it (Y=192/208/224).

`front.anm` is loaded into **anm slot 0x15 at id base 0x600** (line 15827,
`FUN_0044df90(0x15,"data/front.anm",0x600)`), alongside the char face anm
(`face_{rm,mr,sk}00.anm` @ slot 0x19 base 0x4a0) and a loading anm (slot 0x17).

## 2. The sprite blit — `FUN_0044f770` (@0x44f770)

`undefined4 __cdecl FUN_0044f770(int obj)` — builds one textured quad from a
sprite object and appends it to the batch. Returns -1 (no draw) when:
- `*(u32*)(obj+0x1c0) & 1 == 0`  (not visible), or
- `*(u32*)(obj+0x1c0) >> 1 & 1 == 0`  (not ready/bound), or
- `*(char*)(obj+0x1bb) == 0`  (disabled).

So calling it on a stale/unbuilt object is harmless. Quad geometry:
`halfW = anmSrc[+0x30]/2 * obj[+0x18]`, `halfH = anmSrc[+0x2c]/2 * obj[+0x1c]`,
where `anmSrc = *(obj+0x1e4)`. Anchor: `obj+0x1c0` bit10 (X) / bit11 (Y) select
center vs top-left. Corner verts are stashed in the `DAT_004b9f**` globals, then
`FUN_0044efb0(obj,1)` does the actual batch append.

### anm sprite-object layout (partial, byte offsets)

| off | meaning |
|---|---|
| 0x18 / 0x1c | X / Y scale factors |
| 0x1b8 | color 0xAARRGGBB |
| 0x1bb | enable byte (0 ⇒ blit skips) |
| 0x1bc | sprite/frame id (selected from the bound anm) |
| 0x1c0 | render flags (bit0 visible, bit1 ready, bit10/11 anchor) |
| 0x1c8 / 0x1cc / 0x1d0 | screen X / Y / Z(depth) |
| 0x1e4 | anm source (holds the sprite's pixel W/H at +0x30/+0x2c) |

## 3. The sprite batch + flush — `FUN_0044f5c0` (@0x44f5c0)

`void __fastcall FUN_0044f5c0(int anmMgr)` flushes the accumulated quad batch:
when the batch count `*(anmMgr+0x2e530) != 0` it issues the D3D draw of
`count*2` triangles from the vertex buffer at `*(anmMgr+0x17e534)`, then resets
the count to 0. `FUN_0044f690` (@0x44f690) is the per-quad APPEND (copies a 0xa8
-byte vertex run, bumps the count). `anmMgr` is the global anm manager
(`*0x4b9e44` region — the same base coop.c uses for the 0x28ef0 script / 0x60
sprite tables). `FUN_0044f770` calls into this append path via `FUN_0044efb0`.

Trailing `FUN_0044f770` calls that ZUN makes without a following flush (e.g. the
power-gauge section) are flushed by the next `FUN_0044f5c0` in the frame — so
injected quads appended after `FUN_0042b603` returns ride the same end-of-frame
flush and render correctly without you flushing yourself.

## 4. The ascii text subsystem — `FUN_00402060` (@0x402060)

`void __cdecl FUN_00402060(void *mgr, float pos[3], const char *fmt, …)` —
vsnprintf into a local buffer, then queue glyph quads on the ascii manager
(`0x0134ce18`). 16px line height; the queue is flushed by the manager's own draw
task. The subsystem is set up once at process init: `FUN_00401e30` →
`FUN_00401d70` loads `data/ascii.anm` into **anm slot 1** and registers the draw
task globally (`FUN_0042fca0(&DAT_0134cdf4,0x10)`), so it renders in **every
scene** — this is why the §8d "P2 SELECT" prompt is visible on the menu screen.

Color/scale for ascii can be set via `_DAT_013542dc`/`_DAT_013542e0` (RGB scale
multipliers; ZUN sets them around the big-number score draw, then restores 1.0).

## 5. How §8a uses all this (reference implementation)

`HookedHudDraw` (coop.c) hooks `FUN_0042b603`:
1. before orig: `*(int*)(scoreStruct+0x1d70) = 1` → force the full sidebar
   repaint so a dropped P2 count can't leave a stale icon;
2. call orig (paints P1's full HUD);
3. after orig: loop `FUN_0044f770` over P2's lives icon (`scoreStruct+0x14ac`)
   and bombs icon (`scoreStruct+0x16f8`) at Y=192/208, then `FUN_00402060` for a
   `2P` marker and the power number. Reusing P1's icon objects is safe because
   the blit reads position per call.

Open/untested: whether the persistent surface needs the forced redraw at all
(depends on whether the HUD RT is cleared per-frame — the forced redraw is the
safe superset either way); exact icon appearance/position (tune `HUD_*` defines).

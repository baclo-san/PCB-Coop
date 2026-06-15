# Night-shift goals (unsupervised)

Canonical context: `docs/th07_netplay_handoff.md`. Build: relink coop.dll directly
(execution-policy blocks build.ps1) — see the compiler invocation in chat history /
`build.ps1` lines 86-90. Verify DLL stays 32-bit (machine 0x014c). Commit
incrementally, clear messages, keep tree clean. Co-op game side is the confirmed-good
baseline; do NOT regress it.

## ✅ DONE 2026-06-15 — netplay gameplay lockstep wired (see handoff §5k)
The PRIMARY task below is now implemented for GAMEPLAY: the netcode C API is
linked into `th07_coop.dll`, P2's input comes from the wire, the `FUN_00437c70`
seam locksteps input + `FUN_00442c60` syncs the seed, all behind
`coop.ini [net] enabled=1` (default off, baseline preserved). Compile +
native-merge-test verified; needs a two-machine network test. STILL OPEN:
per-player char/type over the wire (menu FSM bypassed under netplay → P2 clones
P1's char) and a real host→guest seed handshake. See handoff §8e.

## 1. Start proper netplay integration (PRIMARY)
The input seam is already in place: P2 gameplay input via `ReadP2InputLocal()`, menu
input via `ReadP2MenuInput()` — the netcode replaces these with the remote word
(handoff §8e). `src/netplay/` already has `netcode.cpp`, `Connection.cpp`, `merge.cpp`,
`netcode_c_api.cpp` + `tests/netloop_test.cpp` (builds today). **Rip what you can from
the EoSD mod `d:/PCB Co-op/th06_multi_net`** (`src/Connection.cpp`, `ConnectionUI.cpp`,
`Supervisor.cpp` netplay paths) — same engine generation, real C++ types. Goal: wire
the C API into coop.c behind a flag so P2's input comes from the wire; sync the menu
FSM (P2 char/type) over the wire too. Lockstep + the existing record/replay desync
diff. Keep local-keyboard P2 as a fallback. **Define "whose instance am I" early**
(host=P1 / guest=P2) — needed for #2.

## ✅ PROTOTYPED 2026-06-15 — proximity transparency (#2 below)
Implemented as `ApplyProximityFade` in coop.c, gated behind `coop.ini [coop]
proximity_fade=1` (default off). Asymmetric + per-instance as specified: under
netplay the host fades P2 and the guest fades P1 (keys off `Nc_IsHost()`);
single-machine fades P2 near P1 (the prototype). Alpha ramps with the P1↔P2
squared distance (PROX_NEAR2/PROX_FAR2), floored at PROX_FLOOR so the faded
player stays trackable; applied after the player update, skipped while either
player is a ghost / game-over. NEEDS A VISUAL LOOK to tune the ramp + floor, and
the real asymmetry needs a netplay test (#1). Build + compile verified.

## 2. Proximity transparency (needs #1 first; only netplay-testable)
When the two players are close, fade the OTHER player out so you can see your own.
**Asymmetric, per-instance:** on P1's machine, P2 turns transparent; on P2's machine,
P1 turns transparent — so it keys off the local-instance identity from #1 (host vs
guest), NOT a fixed player. Drive the player sprite alpha (tint `0xAARRGGBB` at the
player/sprite obj; enemy/bullet use the same field — see `th07_enemy_system.md` §2)
by distance: full alpha far, ramp to ~transparent within ~N px. Local player always
fully opaque. Until netplay lands, the "local identity" is just P1, so you can
prototype "P2 fades near P1" single-machine, but the real asymmetry needs #1.

## Done this session (verify if convenient, else trust)
- P2 bomb portrait suppression (different-char P2): hides wrong face, keeps spell
  name — committed, user-confirmed.
- Damage modifier: halving -> **0.6** (`HookedDamage`, `r = (int)(r*0.6f)`).
- Revival graze range: 24 -> **32px** (`REVIVE_RADIUS`).

## Stretch (when above are done)
- **3P expansion:** generalize the P2 piggyback to P3 (clone #2, own input word,
  own res field-swap, own anm overlay slot, own target block). Damage modifier
  becomes /N (3P = 0.4-ish). The /2 and `s_p2*` pairs are the template.
- **Total RE:** finish the ECL VM (`FUN_00410520` opcode table — see
  `th07_enemy_system.md` §5; use thtk/public ECL docs, not PCBdecomp), then map
  remaining boss-specific fields + the anm chained-slot/texture internals (§8b).

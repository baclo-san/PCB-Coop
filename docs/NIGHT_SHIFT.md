# Night-shift goals (unsupervised)

Canonical context: `docs/th07_netplay_handoff.md`. Build: relink coop.dll directly
(execution-policy blocks build.ps1) — see the compiler invocation in chat history /
`build.ps1` lines 86-90. Verify DLL stays 32-bit (machine 0x014c). Commit
incrementally, clear messages, keep tree clean. Co-op game side is the confirmed-good
baseline; do NOT regress it.

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

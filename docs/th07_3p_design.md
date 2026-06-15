# TH07 (PCB) Co-op — 3-Player (P3) Design

> **Status:** design draft on branch `coop-3p`. 2P is the shipped product and stays on
> `main`, comprehensible and untouched. 3P is an opt-in variant for people who want it.
> Decisions locked with the user (2026-06-15): **star / host-relay topology** (reassess
> performance after a live test); 3P is a "nice extra," 2P is the real product.

---

## 0. Guiding constraint — don't poison the 2P product

`main`'s 2P path is proven end-to-end (menu select, different chars, lasers, bombs,
retry, ghosts/resurrection) and is the thing users get. **N-player abstractions must NOT
leak back into `main`'s hot paths.** 3P diverges *here*, on `coop-3p`. This branch should
periodically rebase on `main` to absorb 2P bug-fixes, never the reverse — until/unless 3P
proves out and we decide to unify behind a runtime player-count.

Where a change is genuinely N-agnostic and *simplifies* 2P (e.g. `Nc_GetLastSplit`
growing a third out-param that's simply ignored when no P3 connects), it can land on
`main` too. The test: would a reader of the 2P code be confused? If yes, it stays here.

---

## 1. Cost re-score (vs. the original §4 "3-player verdict")

The handoff §4 verdict (2026-06-10) predates the entire P2 entity graft. Re-scored against
today's code:

| Bucket | §4 estimate | Reality now |
|---|---|---|
| Input transport | "word is full, hard" | **Small.** P3 never touches the 16-bit engine word; it rides the netcode split. `Bits<32>` already exists in [Connection.hpp](../src/netplay/Connection.hpp). |
| Entity graft | "≈2× the work" | **Large but replication, not RE.** Every P2 mechanism (collision/graze/shot/bomb/aim/ghost/border-shadow/face) is a proven template to clone into `g_Player3`. |
| Network topology | "the real new problem" | **Still the one genuinely new architecture.** But star means only `Host` becomes 1:N; `Guest` is unchanged. |

---

## 2. Transport / wire format

**Key fact:** P2 only lives in the engine input word's high 7 bits for two *incidental*
reasons — free replay capture (§8k) and the natural th06 port. P3 is a DLL-owned entity
just like P2 and **does not need to live in the engine word at all.**

- Engine word (`g_InputMenu` / `g_InputGameplay`) stays exactly as on `main`:
  **P1 = low 9 bits, P2 = high 7 bits.** Replay capture and engine/render compat preserved.
- **P3 input travels only in the netcode split.** The seam already exists: §5n added
  `Nc_GetLastSplit(&p1,&p2)` (de-merges a frame to player identity, identical on both
  machines). Extend to `Nc_GetLastSplit(&p1,&p2,&p3)`. `coop.c` reads P3 from `p3` into
  `g_Player3`; P1+P2 keep flowing through the engine word untouched.
- **Wire payload:** simplest correct shape is to stop bit-cramming and store *one raw
  16-bit local word per player per frame*. Host bundles three 16-bit words; `Bits<32>`
  (already defined, [Connection.hpp:82](../src/netplay/Connection.hpp#L82)) or a small
  `unsigned short keys[3]` per frame both work. Prefer an explicit `keys_by_slot[3]` —
  readability over packing; 15-frame batches × 3 words × 2 bytes is negligible bandwidth.

**Replay (deferred):** §8k stores P2's char in the header `0x58–0x5d` gap (uses 4 of 6
bytes: `C2 07 <p2sel> <diffchar>`). A P3 tag needs the last 2 bytes (`<p3sel>` +
diff-char) — verify the loader/consumer never touches `0x5c–0x5d` first. P3 per-frame
input is *not* in the 16-bit engine word, so a 3P replay needs a side-stream and is out of
scope until 2P two-player playback exists at all (§8k still "planned").

---

## 3. Topology — star / host-relay

Decided: **star.** Why it fits this codebase specifically (not just generic preference):

- **Only `Host` grows to 1:N.** Guests in a star talk *only* to the host, so the `Guest`
  class ([Connection.hpp:231](../src/netplay/Connection.hpp#L231)) is **unchanged** — still
  one socket, one peer. That sidesteps the single-peer assumption baked into
  `ConnectionBase` (the static Winsock refcount note in §3b) for two-thirds of the peers.
- The existing model is **already host-authoritative**: host mints the seed and the guest
  adopts it (§5o handshake), and host drives `Ctrl_Try_Resync` (desync recovery). Star
  extends both verbatim to N guests. Full mesh would make seed/resync authority ambiguous
  and force every peer to be 2-socket + 2× NAT traversal — strictly more friction here.
- Cost: guest↔guest input is **2 hops** (guestA → host → guestB). Hidden by the delay
  buffer; may want `delay=2` instead of 1. Acceptable for co-op (not frame-tight versus).
  **This is the thing to measure** in the live test before judging mesh worth it.

### Per-frame flow (star)
1. Each **guest** reads local input + snapshots its RNG seed → sends to host (as today).
2. **Host** collects its own input + both guests' for frame `F`.
3. Host packs all three into one broadcast `Pack` → sends to **both** guests.
4. Each machine (host + both guests) now has all three slots for `F` and runs the **same**
   N-way merge → identical `(p1, p2, p3)` everywhere.
5. Lockstep: advance `F` only once all three inputs for `F − delay` are in hand;
   5 s timeout → drop policy (§6).

### Slot assignment
Host assigns slots by **connect order**: first guest = **P2**, second guest = **P3**
(host = **P1**). Communicated in the handshake (add a `player_index` field to
`Ctrl_Set_InitSetting` / the HELLO). Deterministic and agreed by all three → the merge and
the entity wiring (`g_Player2` vs `g_Player3`) line up on every machine.

### N-way merge
Generalize [merge.cpp](../src/netplay/merge.cpp) `MergeKeys` from the host/guest 2-branch
into a slot-indexed reconstruction: for the engine word, P1 = host's low 9, P2 = slot-1
guest's bits mapped to high 7; P3 is emitted to the *split* only (not the engine word).
The UI-union path (`self | rcv` when `is_in_UI`) becomes the OR of all three for menu
co-navigation.

---

## 4. Handshake (extend §5o port)

- `Host` waits for **two** PONGs (both guests present) before flipping `s_netActive`.
- Host pushes `delay` + `seed` + **`player_index`** to *each* guest; both adopt
  (guest ini values ignored, as today).
- `coop.ini` needs an **expected-peer-count / session-mode** so the host knows to wait for
  two guests (vs the 2P "wait for one"). Launcher gains a "3-Player Host" affordance (or a
  player-count field) — host-side only; guests just "Connect" as now.
- Version gate (`MULTI_NET_VER`) unchanged — all three must match.

---

## 5. Determinism — what carries over unchanged (the good news)

The hard-won 2P determinism work is **per-machine** and generalizes for free:

- **§8j scene-start barrier** — re-zero `s_netFrame` + `Nc_Reset()` at every scene boundary
  (PCB scene id `self+0x154`). Per-machine; identical on three machines. No change.
- **Seed oracle** — currently `self.seed == rcv.seed`. Generalize to "all three agree"
  (`p1seed == p2seed == p3seed`). Host drives `Ctrl_Try_Resync` to *both* guests; all reset
  RNG + clear rcv buffers at the chosen frame. The infra in `netcode.cpp` is already there.
- **§8h telemetry** — the `NET … SYNC/WAIT/LOST` overlay extends to show two peers
  (`F#### d# | P2:sync P3:wait###ms`). Diagnostic only.

---

## 6. Drop / disconnect policy (new decision for 3P)

2P today: a mid-game drop **stays local** (no reconnect; the pump doesn't restart). With
three peers a drop is both likelier and more disruptive — and lockstep *cannot* simply
continue, because the dropped player's input stream stops (every surviving machine would
stall on the missing frame).

Candidate policies, simplest-correct first:
- **A (ship first):** any peer drop → **whole session falls to local** (mirror 2P). Safe,
  trivial, determinism-preserving. The two survivors lose netplay too — crude but correct.
- **B (later):** dropped player is **frozen into a ghost** and the host **synthesizes
  neutral input** for that slot so the remaining two stay in lockstep. Determinism holds
  (the synthesized stream is agreed by host broadcast). This is the "good" experience but
  needs the host to detect the drop and start filling, and a UX for "P3 disconnected."

Recommend A for the first playable, B as a follow-up. **User decision** on whether B is
worth it.

---

## 7. Entity graft — `g_Player3` (deferred; design only)

Pure replication of the proven P2 path, P2→P3:

- State: `s_p3Lives` / bombs / power (the field-swap + checksum-heal pattern, §3a/§5f).
- Collision + graze: re-invoke `FUN_0043e3b0` (graze, unlocks bullet hit-test) **and**
  `FUN_0043e260` (hit) with `ECX = P3`, exactly as the §5d P2 fix did.
- Shot / bomb / aim: clone `BuildP2TargetBlock` → `BuildP3TargetBlock` (per-player homing
  + SakuyaA cone off P3's own char, §8c).
- Border: a **third ringless shadow** (the single ring slot 404 stays P1's; §5d).
- Ghost / resurrection: see §8 — needs N-player rules, not a straight clone.
- Boss-HP divisor: already player-count based (`HookedDamage`); becomes `1 + P2 + P3`.
  One-line generalization.
- Face / portrait overlay: the §8b lead, still LOW priority / needs live iteration.

---

## 8. N-player gameplay rules that aren't a straight clone

- **Resurrection / life-share:** the §5f mechanics assume a 1:1 pairing (survivor revives
  the one ghost). With three: *any* live player can graze-revive *any* ghost; life-share is
  donor→recipient among any live pair. The **phantom-spare** system (§5f, "game-over only
  when all down") generalizes: GAME OVER fires only when **all three** are ghosts.
- **Character select:** the §5n two-pass FSM (P1 pass, P2 pass) becomes **three passes**
  (`CM_P3_CHAR` / `CM_P3_SHOT`). Title/difficulty still co-navigate (UI-union of all three).
- **HUD real estate:** PCB playfield is 384px, centered; off-field space = left margin +
  right sidebar (P1 native). P2 already added a panel. P3 needs a third slot — likely stack
  P2/P3 on the left margin or compress the rows. **User's call** (PCB layout expertise);
  decide when it's visible on screen.

---

## 9. Sequencing on `coop-3p`

1. **Network N-shaping** (this is "the network" the user asked for next):
   `Host` → 1:N (accept 2 guests, bundle, broadcast); slot assignment + `player_index` in
   handshake; `Nc_GetLastSplit` → 3 out-params; `MergeKeys` → slot-indexed; seed oracle →
   3-way. **Validate by compile + a 3-process `netsim`** (extend `tests/netsim.cpp` /
   `run_netsim.sh` to host + 2 guests; today it's host + 1 guest).
2. **`g_Player3` entity graft** (P2-pattern replication, §7).
3. **N-player rules** (§8): resurrection, three-pass select, HUD.
4. **Drop policy A**, then maybe B (§6).
5. **3-PC live test** — and the latency/perf read that decides whether star is good enough
   or mesh is worth exploring.

Steps 1 (and its 3-process netsim) need **no game and no third PC** — pure logic, testable
on one machine in-process. That's the natural first chunk of "the network" to build.

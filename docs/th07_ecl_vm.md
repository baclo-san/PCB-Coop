# TH07 (PCB) ECL danmaku VM — opcode reference

The ECL VM is the bytecode interpreter that drives **all enemy/boss behaviour**
(movement, bullet patterns, lasers, HP, life/timer callbacks, item drops, the
cherry gauge). It is the single function Ghidra could **not** decompile in this
dump — `Unable to decompile 'FUN_00410520'` (PCBdecomp.c:8840), almost certainly
a huge computed-goto opcode dispatch. This doc supplies the opcode table that the
decomp can't, and grounds it in this binary's VM interface.

Build-specific notes are anchored to th07.exe ver 1.00b
(SHA256 `35467EAF…E80CA`). The **opcode numbers/names** are th07-specific and come
from a public source (see "Sources"); they are the standard Touhou-modding
mnemonics, not derived from PCBdecomp.c.

---

## 1. VM interface (verified from the decomp callers)

- **`FUN_00410520(ECX = enemy_ctx)` → int** — runs the enemy's ECL script for one
  tick; returns **-1 when the script ends** (the caller then clears the enemy's
  active bit `+0xb8a & 0x80`). Callers: `FUN_0041f220:12070`, `:12128`,
  the enemy update `:12695`.
- **The ECL context IS the enemy slot.** Enemy slots live at
  `enemy_mgr + 0x4f50 + i*0x4f48` (stride `0x4f48` = `0x13d2` dwords; see
  `th07_enemy_system.md` §1). So `FUN_00410520` reads/writes the same per-enemy
  struct the rest of the enemy system uses — the ECL "variables" (§3) are windows
  into that struct + globals.
- **Per-instruction binary layout** (the container format is shared th06–08;
  PCB = "ECL v1.1", confirmed against PyTouhou's `formats/ecl.py`, see enemy
  doc §6):
  ```
  u32 time;      // frame this instr fires (relative to sub start)
  u16 opcode;    // dispatched by FUN_00410520 (table below)
  u16 size;      // total instr size in bytes (advance by this)
  u16 param_mask;// per-arg "is this a variable ref?" bitmask
  u8  rank_mask; // difficulty gate (which ranks run this instr)
  u8  param_cnt;
  <args...>      // typed per opcode: i32 / f32 / u16; strings Shift-JIS
  ```
  A sub ends at `time == 0xffffffff` / `opcode == 0xffff`. The VM each tick:
  fetch instr at the current PC → if `time` reached and rank-gate passes →
  dispatch on `opcode` → advance `size`; a `wait`(45) yields the tick.

> **Why map it at all:** every boss pattern, the HP/phase structure the co-op
> damage divisor scales, the per-enemy item drops the determinism work tracks, and
> the cherry gauge all flow through these opcodes. Several already have RE'd
> handlers in this repo (see §4).

---

## 2. Opcode table (th07, 0–161)

`__` prefix = unnamed/uncertain in the source map. Grouped for readability; the
number is authoritative.

### Control flow / VM (0–45)
| # | name | notes |
|---|---|---|
| 0 | nop | |
| 1 | delete | end this enemy/sub |
| 2 | jump | unconditional (to offset+time) |
| 3 | jump_dec | decrement a counter var, jump if >0 (loop) |
| 41 | call | call an ECL sub (push return) |
| 42 | ret | return from sub |
| 45 | **wait** | yield the tick for N frames (the VM's frame gate) |

### Variables / RNG / math (4–40, 51–52, 159)
| # | name | notes |
|---|---|---|
| 4 | set_int | `var = imm` |
| 5 | set_float | |
| 6 | set_int_rand_bound | `var = rand % b` |
| 7 | set_int_rand_bound_min | `var = min + rand%range` |
| 8 | set_float_rand_bound | |
| 9 | set_float_rand_bound_min | |
| 10 | set_int_rand_sign | random ± |
| 11 | set_float_rand_sign | |
| 12–16 | math_int_{add,sub,mul,div,mod} | |
| 17 | math_inc | |
| 18 | math_dec | |
| 19–23 | math_float_{add,sub,mul,div,mod} | |
| 24 | math_sin | ⚠ x87 transcendental (see netplay FP note, §5) |
| 25 | math_cos | ⚠ x87 transcendental |
| 26 | math_atan2 | ⚠ x87 transcendental |
| 27 | float_time | timer as float |
| 40 | math_norm_angle | wrap to (-π,π] |
| 51 | __math_rand | |
| 52 | __math_rand_rad | |
| 159 | __math_float_unknown | |
| 151 | math_circle_pos | point on a circle |

### Conditional jumps (28–39)
`jump_{int,float}_{equ,neq,lss,leq,gre,geq}` = 28–39 (int/float pairs). Standard
compare-two-vars-and-branch.

### Boss/sub params (43–44)
| # | name | notes |
|---|---|---|
| 43 | set_int_from_boss | read a value from the boss object |
| 44 | set_float_from_boss | |

### Movement / position (46–63)
| # | name | notes |
|---|---|---|
| 46 | move_position | |
| 47 | __move_unknown | |
| 48 | move_angular_velocity | |
| 49 | move_speed | |
| 50 | move_acceleration | |
| 53 | move_at_player | aim movement at the player |
| 54 | move_dir_time | |
| 55 | move_point | move to a point over time |
| 56 | __move_circle_abs | |
| 57 | set_orbit_distance | |
| 58 | set_angle | |
| 59 | __move_change_1 | |
| 60 | __move_change_3 | |
| 61 | __move_change_2 | |
| 62 | move_bounds_set | clamp to a box |
| 63 | move_bounds_disable | |

### Bullets (64–81)
| # | name | notes |
|---|---|---|
| 64 | bullet_fan_aimed | fan aimed at player |
| 65 | bullet_fan | |
| 66 | bullet_circle_aimed | ring aimed at player |
| 67 | bullet_circle | |
| 68 | bullet_offset_circle_aimed | |
| 69 | bullet_offset_circle | |
| 70 | bullet_random_angle | |
| 71 | bullet_random_speed | |
| 72 | bullet_random | |
| 73 | shoot_interval | autofire period |
| 74 | shoot_interval_delayed | |
| 75 | shoot_disable | |
| 76 | shoot_enable | |
| 77 | shoot_now | |
| 78 | shoot_offset | spawn offset |
| 79 | bullet_effects | spawn anim/sfx flags |
| 80 | bullet_cancel | |
| 81 | bullet_sound | |

### Lasers (82–89, 134, 152, 156–158)
| # | name | notes |
|---|---|---|
| 82 | laser_create | |
| 83 | laser_create_aimed | |
| 84 | laser_index | bind to a laser slot |
| 85 | laser_rotate | |
| 86 | laser_rotate_from_player | |
| 87 | laser_offset | |
| 88 | laser_test | |
| 89 | laser_cancel | |
| 134 | laser_clear_all | |
| 152 | laser_angle | |
| 156 | __laser_unknown_byte | |
| 157 | laser_end_length | |
| 158 | laser_start_length | |

### Enemy / boss state, HP, callbacks (90–116, 126, 130, 132–133, 142, 145, 147–148, 153, 161)
| # | name | notes |
|---|---|---|
| 90 | spellcard_start | declares a spell card (anti-cheese gate; cf. bug B5) |
| 91 | spellcard_end | |
| 92 | enemy_create_abs | spawn an enemy at absolute pos |
| 93 | enemy_create_rel | spawn relative |
| 94 | enemy_kill_all | |
| 95 | anm_set_main | |
| 96 | anm_set_poses | |
| 97 | anm_set_slot | |
| 98 | anm_death_effects | |
| 99 | boss_set | mark this enemy as the boss |
| 100 | spellcard_effect | |
| 101 | enemy_set_hitbox | |
| 102 | enemy_flag_collision | body-contact damage on/off |
| 103 | **enemy_flag_invulnerable** | the invul flag (cf. B5 boss-during-spell) |
| 104 | enemy_flag_can_take_damage | |
| 105 | effect_sound | |
| 106 | enemy_flag_death | |
| 107 | death_callback_sub | run a sub on death |
| 108 | enemy_interrupt_set | |
| 109 | enemy_interrupt | jump the script to an interrupt label |
| 110 | **enemy_life_set** | sets HP — handler `FUN_00424290`, writes cap `+0xd30` (boss-HP doc) |
| 111 | boss_timer_set | spell/phase time limit |
| 112 | life_callback_threshold | fire a sub when HP drops below X (phase end) |
| 113 | life_callback_sub | |
| 114 | timer_callback_threshold | |
| 115 | timer_callback_sub | |
| 116 | enemy_flag_interactable | |
| 126 | boss_set_life_count | number of phases/bars |
| 130 | enemy_flag_disable_call_stack | |
| 132 | enemy_flag_invisible | |
| 133 | boss_timer_clear | |
| 142 | enemy_flag_armored | takes reduced/no damage |
| 145 | boss_interrupt | |
| 147 | __enemy_manager_unknown | |
| 148 | life_callback_ex | |
| 153 | enemy_set_second_hitbox | |
| 161 | **enemy_invincible_time** | brief on-hit/timed invuln (cf. C2 "death fairy") |

### Effects / particles / anm (117–120, 128–129, 138–141, 149–150, 155)
| # | name | notes |
|---|---|---|
| 117 | effect_particle | |
| 118 | effect_create | |
| 119 | **drop_items** | on-death item drop (item system / determinism) |
| 120 | anm_flag_rotation | |
| 128 | anm_interrupt_main | |
| 129 | anm_interrupt_slot | |
| 138 | trail_set | |
| 139 | set_life_bar | |
| 140 | __effect_unknown | |
| 141 | nop141 | |
| 149 | __spell_effect_position | |
| 150 | anm_rotate | |
| 155 | __set_float_rand_x_pos_dependent_idk | |

### Ex-instructions / misc (121–125, 127, 131, 135–137, 143–144, 146, 154, 160)
| # | name | notes |
|---|---|---|
| 121 | ex_ins_call | extended-instruction dispatch |
| 122 | ex_ins_repeat | |
| 123 | std_call | call an STD (background/camera) routine |
| 124 | drop_item_id | drop a specific item type |
| 125 | std_interrupt | |
| 127 | debug_watch | |
| 131 | bullet_rank_influence | difficulty scales bullet count/speed |
| 135 | spellcard_flag_timeout | |
| 136 | enemy_flag_grazeable | |
| 137 | enemy_flag_oob_immune | |
| 143 | bullet_cancel_radius | |
| 144 | call_repeat | |
| 146 | bullet_clear | |
| 154 | **drop_point_items** | scatter point items (cherry-scaled value) |
| 160 | **add_cherry** | adds to the cherry gauge — the ECL side of the cherry coupling (cherry doc) |

---

## 3. ECL variable registers (gvar ids)

ECL args with the param_mask bit set are **variable references** by these ids
(not literals). Many are live windows into the enemy/player/game state, so they
are the ECL-visible names for fields RE'd elsewhere.

| id | name | id | name |
|---|---|---|---|
| 10000–10003 | I0–I3 (int regs) | 10037–10044 | ARG_A..N (sub args) |
| 10004–10011 | F0–F7 (float regs) | 10045 | CIRCLE_ANGLE |
| 10012–10015 | I4–I7 | 10046 | CIRCLE_SPEED |
| 10072–10073 | F8, F9 | 10049 | DIST_ORIGIN |
| 10016 | DIFFICULTY | 10050–10052 | ORIGIN_X/Y/Z |
| 10017 | RANK | 10053 | SELF_ANGLE_VEL |
| 10018–10020 | SELF_X/Y/Z | 10054 | SELF_ANGLE |
| 10021–10023 | PLAYER_X/Y/Z | 10055 | RANDF2 |
| 10024 | PLAYER_ANGLE | 10056 | RANDF_RANGE |
| 10025 | SELF_TIME | 10057–10059 | TARGET_X/Y/Z |
| 10026 | PLAYER_DISTANCE | 10060 | RANDRAD |
| 10027 | SELF_LIFE | 10061 | LAST_FRAME_DAMAGE |
| 10028 | PLAYER_SHOT | 10062 | BOSS_ID |
| 10029–10036 | PARAM_A..N | 10063–10065 | UNUSED_X/Y/Z |
| 10047–10048 | VAR_47/48 | 10066–10069 | LIFE_THRES, LIFE_THRES2, MINIMUM_LIFE, MINIMUM_LIFE2 |
| | | 10070 | ITEM_REWARD |
| | | 10071 | SCORE_REWARD |

`PLAYER_X/Y` (10021/22) is the same world player position the rest of the engine
mirrors at `DAT_004be408/40c` — so an ECL pattern aiming at the player reads P1's
position. Co-op note: nothing makes ECL aim at P2; coop's `HookedAngleToPlayer`
redirects only the engine's `FUN_00442370` suction/aim, not in-ECL `move_at_player`
/ `bullet_*_aimed` (those resolve via `PLAYER_X/Y` and the boss-aim path the
project already documents in §8c / `BuildP2TargetBlock`).

---

## 4. Cross-reference to this repo's RE

| opcode | already RE'd here |
|---|---|
| 110 enemy_life_set | handler `FUN_00424290` writes HP cap `enemy+0xd30`; the ECL set-life path the boss-HP doc found only ever fires for popcorn (`docs/th07_boss_hp_scaling.md`) — boss/midboss HP is set another way, which is why co-op scales damage-side instead. |
| 90/91 spellcard_start/end | the spell-card lifecycle behind bug **B5** (boss invul during a bomb); `FUN_00429a4f` declares the boss spell, `+0x1fbac` = active spell index (`th07_coop_gameplay_bugs.md`). |
| 103 enemy_flag_invulnerable, 161 enemy_invincible_time | the invuln flags relevant to **C2** ("death fairy takes shots but no damage"). |
| 119 drop_items / 154 drop_point_items / 124 drop_item_id | the on-death item drop; the item spawner is `FUN_004326f0`, the drop roll is cherry-gated (`th07_item_collect_credit.md`, `th07_cherry_determinism.md`). |
| 160 add_cherry | the ECL side of cherry gain (the cherry gauge `DAT_0062f88c` etc.); the cherry doc notes cherry is RNG-coupled to item drops — `add_cherry` is where scripts feed it. |
| 24/25/26 math_sin/cos/atan2 | x87 transcendentals — see §5. |

## 5. Netplay determinism note (ties to §8o–§8q)

The VM uses x87 transcendentals (`math_sin`/`cos`/`atan2`, ops 24–26) and float
math throughout, and the per-enemy RNG ops (6–11, 51–52, 70–72) feed the **shared**
RNG stream. Lockstep netplay needs bit-identical results across both machines, so:
- The **x87 precision control word** must match on both ends — the live desync hunt
  (handoff §8q) traced the residual desync to D3D8 leaving the FPU at 24- vs 53-bit
  precision per machine; the fix pins it (`PinFpuForNetplay`). The ECL float math is
  exactly the surface that diverges when the CW differs.
- Any per-machine perturbation of the FPU stack/CW before the VM runs forks these
  ops (handoff §8p `fpu_guard`).

---

## Sources

ECL container format & semantics cross-checked against PyTouhou
(`pytouhou/formats/ecl.py`, EoSD-era) and this binary's VM callers. The
th07-specific opcode numbers/names + variable ids are from **ExpHP/`truth`**'s
prepackaged `map/th07.eclm` (itself based on Priw8's eclfiles), the standard
Touhou-modding mnemonic mapping:
- https://github.com/ExpHP/truth — `map/th07.eclm`
- https://github.com/thpatch/thtk — ECL container tooling
- Touhou Wiki ECL format spec (User:Mddass) — opcode semantics (PCB v1.1)

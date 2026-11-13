# Jev prompt changelog

One entry per prompt change tried with `tools/jev_suite.py` or a game batch: what changed, the scores before and
after, kept or reverted. Newest first. The suite's margin varies by about ±0.03 between runs of the same prompt.

## 2026-11-13: next bundle, not yet tested in a batch (air defense, interceptors, defend on first contact)

From the j7-groups4 logs: classic's first aircraft came on turns 85-95 (an AWAC, then one Ground Attack Plane, which hit
Jev 12 times in w0 r2 and w9 r1); Jev saw them at turns 85-98, was offered an Anti Aircraft among other builds and
built 1 in 8 games, and had a Mobile Anti Aircraft in one game. 95 of green's 226 hits on Jev landed on unarmed units.

- Air defense without a question once enemy aircraft or an enemy Air Units Plant are seen (also aircraft spotted
  during the enemy's turn): builders build up to 2 Anti Aircraft spread among the base's buildings, factories that
  can produce a Mobile Anti Aircraft do (1 per Defend group, 2 from size 8). Prompt: the air place says "has N of M".
- The Defend group keeps at least 4 from the first enemy units seen, not from the enemy base found.
- Interceptors: once enemy units are seen, 2 Scouts beyond the Scout group join the Defend group, wait in the base
  and chase enemy units seen near the base or within 6 cells of a builder; Jev decides on contact (attack, charge,
  hold). Prompt: a new production place "interceptor place in the Defend group".
- Charge rebuilt (the user: a desperation move without vision; with vision good whenever the task group can win, a
  Tank against an uncovered Scout above all): a charge closes in on an enemy out of reach, by next turn, into ground at
  least 80% in sight within max(4, the target's range). A fight estimate (`EstimateFight`) simulates up to 3 turns:
  the unit and the team's armed units and defenses that reach the target this turn or next, against the target and
  the known enemies that can fire on the charge's end cell (cover); each fires its rounds, attack minus armor per shot,
  ours at the target first, theirs at the charging unit; the cover fires first while the unit closes in; unseen ground
  scales the enemy's damage up. Offered when the target is destroyed, the unit survives and the materials destroyed
  exceed those lost, or as a last resort (an outranging enemy near the base that nothing else of the team can
  answer), with the estimate in the text. j7-groups4 offered 179 charges in 8 games.
- Counter-battery: a long range unit with an enemy that outranges the front line in range fires at it without a
  question, the one nearest destruction first; attack options against such enemies say so.
- Cheaper construction questions: a builder that can only refill or stay refills without a question (Jev chose the
  refill 357 of 358 times in j7-groups4, 32% of construction questions); a builder that chose to stay is not asked
  again until its kinds of choices or its load change, as idle factories are.
- Fix: counting units in production asserted on a builder that just finished a building (empty build list).
- Mock checks: m-aa1 (world 0) aircraft seen turn 90, Mobile Anti Aircraft ordered turn 92, Anti Aircraft started
  turn 93, the Engineer was killed on the way; m-aa2 (world 9) AWAC seen turn 92, Anti Aircraft started turn 93. Both
  ran to turn 100 without assertions. Harness: `jev_match.py --visible` shows the window; `v1-visible10` (10 turns, Jev
  vs classic, world 0) finished in 2.7 minutes, 16 questions, no errors.

## 2026-11-13: all-up bundle j3-allup (fast hidden games, one batch for fixes and improvements)

Baseline `j1-base` (dev set, 12 maps, 100 turns, jev-1.14.0, exe 68b14d7-hidden): Jev rank 1 on 8 of 12, worth below
green on 11; eliminated on worlds 10 and 14, collapsed on 3, 9, 20. Seat baseline `c1-seat` (classic as red, 2 repeats):
rank 1 on 4.5 of 12, classic almost never scores points.

Bundle `j3-allup` (exe `runs/exes/allup1.exe`), tested together per the all-up procedure:
- Controller: moves offered only when reachable this turn along the pathfinder's own costs (40-65% of chosen moves
  failed before, 1-3% in test games); survey and refill destinations need a path; picket sentry in the executing
  state (an init-state sentry made every end of turn wait for good, world 18 from turn 91); attacks offered only when
  a firing cell is reachable this turn with a shot left, and ordered as an approach when out of range (the
  attack-approach patch, rejected on one batch on 2026-11-02, back as part of the bundle); a guard option for ground
  combat units: a post toward the enemy side of the base, held on sentry. Logs: the classic purchase, and every team's
  assets by type at game over.
- Prompts: `choose()` decides construction and production questions in two steps: all build (or produce) options
  together against each other option, then the most likely of them. On the 12 baseline logs it changes 897 of 3947
  construction decisions from stay to build and 190 of 468 production decisions from nothing to produce; Jev put
  most of its probability on building, split over up to 24 near-identical options. Military questions unchanged.
- Replay, guard option: a wandering Tank (j1-base-w18 question 263) picks guard 2 of 3 times, a tie the third.
- Dropped before the batch: a base threat fact with a defense-first rule. Production under threat already picked
  combat units (Tank 37 of 46), and no question offers Eco-spheres and turrets together, so no evidence of need.
- Result: lost to the harness. The Jev account ran out of credits at world 6 turn 77 (HTTP 402 on every later
  question); worlds 1, 9, 10, 12, 14, 16, 18, 20, 22 had no answers at all. Valid: world 0 (rank 1, 110 points against
  38 in the baseline, 3 lost against 7), world 3 (eliminated on turn 85; the baseline game ended at worth 12), world 6
  to turn 76 (72 points against 0). Bridge, match and batch now stop on a billing error.
- Found in world 0: 23 Gun Turrets walled in the factory exit; the controller's exit clearing chose unreachable cells
  (112 of 117 failed moves) and the base built 18 units against 48. Exit clearing now uses the reach map; defenses
  are not offered next to a factory exit or where three defenses reach already. Next exe: `runs/exes/allup2.exe`.

## 2026-11-13: j7-groups4, first bundle ahead of classic as red (exe groups4)

Quick set, 2 games per map, all valid, 2.30M input tokens, $0.096 ($0.012 per game; j1-base $0.103 per game).
- First rank in 6 of 8 (classic as red 5 of 8 in c1-seat; j6-groups3 2, j5-groups 4). No elimination. Points in 6
  games: w0 80 and 108, w3 52 and 0, w6 81 and 62, w9 18 and 0. Eco-spheres 0-2 per game. Promotion criteria met.
- Hit log: the engine's attacks_landed counts hits on alien units; classic's Scouts shoot alien derelicts (w6:
  110-129 hits per game). Between the teams: green on red 0-49 per game, red on green 1-44 (w3 game 1: 49 against 44).
  Of green's hits on Jev: outranged 96, unarmed targets 95 (Constructors, Surveyors, mines), could fire back 35. Ground
  Attack Planes hit Jev 12 times in w0 game 2 and in w9 game 1; Jev had a Mobile Anti Aircraft in one game only.
- Tokens: construction 1.52M (66%), military 0.44M, production 0.30M, command 0.03M.

## 2026-11-13: j6-groups3 (exe groups3: event commander, auto survey, long range half, edge posts, charge, gated Eco-spheres)

Quick set (worlds 0, 3, 6, 9), 2 games each, 5.41M input tokens, $0.227 ($0.028 per game: cost target met). All valid.
- First rank in 2 of 8 (j5-groups 4, classic as red 5). World 3 not eliminated for the first time (rank 1 with 26
  points, rank 2); world 9 game 2 eliminated on turn 100 after the commander shrank the Defend group to 2 on turn 50
  while green grew from 12 to 38 units (no charges in that game). Losses fell on most maps (w0 5/2, w6 3/1).
- Points collapsed: Eco-spheres built 0-1 per game against 1-6 in j5-groups (w6: 150 and 30 points -> 0 and 0).
  Cause: the new rule "Eco-sphere only when it can run" removed the option from turn 29 to 85 on world 6 (no spare
  power), and Jev never built the power that would bring it back. In j5-groups Eco-spheres were built on turns 28-36
  and scored once power followed. Reverted for the next batch.
- Charge chosen 14 times (Tanks against Assault Guns and Rocket Launchers at range 7). Commander questions: 2-5 per
  game (was ~12). Construction tokens 1.36M (was 3.66M) with Surveyors on auto survey.

## 2026-11-13: j5-groups, task groups with Jev as commander (exe groups1)

Worlds 0, 3, 6, 9, 2 games each (cut from 24 by the user), jev-1.14.0, 7.9M input tokens ($0.33, $0.041 per game;
j4-allup2 $0.060, j1-base $0.103). All 8 valid: no abandoned waits, assertions or API errors; failed moves 0-8%.
- Ranks: Jev first in 4 of 8 games (w0 2/1, w3 eliminated twice on turns 95/96, w6 1/1, w9 1/2); classic as red
  first in 5 of 8 on the same maps. Points: w6 150 and 30, w9 118 (first points on w9 in any batch), w0 0 and 8
  (j1-base 38). Worth minus green per map, mean of the 2 games: w0 -521, w3 -994, w6 -350, w9 -890.
- Not promoted: fewer first ranks than classic as red; Eco-sphere turns without power 1-44 per game (w0 1 and 21
  against 40 in j4-allup2, w3 41 and 20 against 0); cost $0.041 per game against the $0.029 target.
- Commander: flips the Defend group between 2 and 4 at almost every trigger and rarely keeps the plan; it is asked
  without its previous choices. w3 lost 39 and 46 units with a Defend group of 2-4.
- Tokens: construction 3.66M (46%), military 3.05M, production 1.08M, command 0.09M. Surveyors alone 2.27M (1233
  questions, 1845 tokens each): surveying is the largest cost now.

## 2026-11-13: j4-allup2, the bundle rerun (exe allup2: plus exit clearing and turret limits)

Dev set, one game at a time, jev-1.14.0, 17.1M input tokens ($0.72; baseline 29.5M, $1.24). Per map against
`j1-base`: rank 1 on 7 of 12 in both; mean worth minus green -601 -> -510; points 268 -> 308 (w12 30 -> 106,
w16 54 -> 108, w22 14 -> 37; w0 38 -> 0, w1 69 -> 16); w10 eliminated -> rank 1; w3 eliminated on turn 10 (baseline
ended rank 2 at worth 12); w18 hit the 5M token cap per game on turn 95 (4145 questions) in both batches.
Not promoted by the rule in the plan: a new elimination (w3) and no clear gain.

- w3: the real loss rule (default script SC_L0002 in PATCHES.RES) keeps a team while it has any unit or building
  with ammunition, or a supplied factory, or a Constructor and an Engineer with materials. Jev's instructions say "no
  Constructor and no factory". An Assault Gun killed the Missile Crawler and both Engineers before the first factory
  stood: eliminated with its Constructor alive.
- w0: Eco-spheres off for 33 turns from turn 58, no fuel and no spare power, Mining Station and a generator off,
  29 Tanks produced. Grouped production turned 190 baseline "produce nothing" decisions into production; nothing
  was how Jev left power and materials to the Eco-spheres. Grouping stays a candidate for construction only.

## 2026-11-03: speed-specific forecasts, mine recovery, construction guards

- Recovered completed `b18-scout-wall` (100 turns, worlds/seeds 0:42, 3:7, 6:13,
  jev-1.14.0): ranks 1/2/2, points 82/0/0, worth 559/232/424. The role/scouting
  candidate has not established a strength gain. Missile Launcher: 2,067 offered
  option entries, zero picks; picket: four offered, zero picks.
- `b19-economy` adds forecasts for the actual factory speed, recovery of an empty
  stopped fuel-mine/generator network, and rejection of occupied construction
  footprints. Faster factory rows now state their actual turns/cost/use directly.
  Turn-41 world-6 replay: normal Tank 3/3 with the new facts (0.38–0.53), versus
  normal twice and x4 once with the original facts. Derived forecast requests
  are labelled as derived, not live game observations.
- Frozen b18/current suite: 14/21 → 15/21 acceptable, three forbidden in both
  (the known Scanner case); held-out 3/3 in both. No military instructions changed.
- `b19`, 60-turn economy check: ranks 2/2/1, points 0/0/2, worth 536/364/624,
  mines 4/3/2, zero abandoned mine sites and zero API errors. World 6's second
  mine stood on turn 39 and both mines stayed on past the former turn-42 blackout.
  The shorter horizon changes Jev's turns-left facts; this is not a controlled
  strength comparison with the 100-turn batches or the defense promotion gate.
- That batch exposed construction starting with insufficient cargo (world 3:
  6 carried, 24 cost, finished at -18), implicit doubling of normal-speed builds,
  and construction facts reading an isolated turret's empty complex. Follow-up
  source filters unaffordable builds, validates cargo again before execution,
  executes the advertised normal speed, and selects the economic network by
  live mine count then raw storage. Factories still describe their own network.
  These follow-ups pass 34 economy engine checks but have no paid game batch yet.
- Forecasts: 24 engine checks; mine links/sites: 19; exits: 23; Python tooling: 9.
  Frozen executables: b19-economy.exe SHA-256 7826e0d751ebce43…, b20-budget-base.exe c27bc99370b8df81….
  Six completed games and provenance: `tools/jev_experiments/2026-11-03-results.json`.

## 2026-11-02 late: expansion, surveying, power for mines, raw materials

Development maps 0:42, 3:7, 6:13, 100 turns, jev-1.14.0.

| Batch | Build | Ranks | Points | Worth (classic) | Mines (2nd) | Raw per turn | Raw lost |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `b11-macro` | `e9f0891` mine sites, connectors | 1 / 0 / crash | 95 / 0 / - | 585 / 20 / - | 2 (47) / 2 (62) / - | - | - |
| `b13-survey` | `83570ab` survey near base, unique ids | 1 / 2 / 1 | 64 / 0 / 43 | 785 (976) / 328 / 844 (819) | 4 / 5 / 4 | 13 / - / 16 | 241 / 308 / 392 |
| `b14-mine-power` | `d801f90` mines get power first | 1 / 2 / 1 | 88 / 0 / 70 | 591 / 204 / 821 | 3 / 3 / 2 | 15 / 16 / 12 | 363 / 396 / 324 |
| `b15-raw` | `9b00f11` raw-first sites, build speeds, storage | 1 / 1 / 1 | 74 / 36 / 79 | 500 / 846 / 804 | 5 / 5 / 3 | 20 / - / 16 | 50 / 74 / 0 |

- `b11`: mine sites offered had 4-5 resources and no far site: the ground near the base was never surveyed (on world 3
  only the starting mine's cells). Replayed Surveyor questions with base distances did not change a choice; a
  "survey around your base" option did (`b13`: best site 18-19). `b11` world 6 crashed on turn 9 (exit 3) after
  the GetSpriteFrame assertion; a logging build in `b12` (stopped, invalid) showed an Assault Gun with an Engineer's
  build order: saved sites were keyed by `UnitInfo::unit_id`, the per type number. Fixed with `GetId()`.
- `b13`: new mines stood switched off (no spare power, so no fuel for more generators). Fixed in `d801f90`.
- Raw materials: before `b15`, 15-40% of all raw mined was lost to full storage (192-396 per game). `b15` cut it to
  0-74 and won all three maps for the first time since `b4`; world 3 scored points for the first time since `b4`.
- Gun Turrets are built by Engineers and Eco-spheres by Constructors, so they never compete in one question; the
  Engineers turn the base's stock into turrets. Turret wording replays did not move choices.
- Kept as fact corrections in the prompts: an Eco-sphere holds its points, raw materials mined while storage is full
  are lost. `b16-spend` (`eb1df34`) adds factory speed rows, up to four plants of a type and a spending rule (suite
  13/18, was 12). Next candidate `9585dd4`: turret sites only for what no defense covers, halted factories asked
  again only when they can run.

## 2026-11-02 night: defense batch, macro gap, Eco-spheres hold their points

Development maps 0:42, 3:7, 6:13, 100 turns, jev-1.14.0. Defense scores from `tools/jev_defense.py`.

| Batch | Build | Ranks | Points | Worth | Defense score | Points lost |
| --- | --- | --- | --- | --- | --- | --- |
| `b9-exits` | `ffb37bb` exits | 1 / 0 (turn 91) / 1 | 66 / 0 / 139 | 595 / 29 / 712 | 86 / 0 / 40 | 54 / 56 / 0 |
| `b10-defense` | `122ec8c` turrets before contact | 1 / 0 (turn 92) / 1 | 139 / 0 / 139 | 730 / 9 / 704 | 94 / 0 / 100 | 0 / 69 / 0 |

- `b10` built its first turret on turn 5 on worlds 0 and 6; world 0 then chose a turret for 37 of 49 builds and
  stood 40 turrets with 6 Eco-spheres: too many, a choice to tune with the turret facts (turrets already there,
  coverage already reached). `b10` world 6 had a Tank stuck in a factory from turn 60 (the repeated move-aside bug,
  fixed in `0f0660b`); it did not change the rank.
- World 3/seed 7 is lost in 4 of 6 batches. In `b10` 13 turrets stood before contact and the base still fell: the
  classic AI took it apart over turns 70-89, about twice Jev's army.
- The engine takes away the points an Eco-sphere stored when it is destroyed (`units_manager.cpp`,
  `team_points -= storage`); `b10` world 3 went 69, 45, 26, 0. Kept as a correction of fact (`06de301`): the
  Eco-sphere role and the construction and military instructions say so. Suite 12/18, 3 forbidden (the known
  Scanner case), +0.18, unchanged; held out 2/3 (was 1/3).
- Macro: Jev built no second Mining Station in 12 of 14 games (classic 2-5). Mine sites were only offered within the
  Constructor's reach; offered sites had a median of 3-4 resources. Surveyor moves also counted unsurveyed cells
  (hidden information), now fixed. `0f0660b` offers the three richest surveyed sites within 14 cells of the base and
  `e9f0891` adds sites joined by up to six connectors that Engineers lay; engine checks `--suite jev-links`.
  `b11-macro` runs `e9f0891` with the prompts before the Eco-sphere correction.

## 2026-11-02 evening: exits, then the first defense candidate

- Found in the b3-b7 logs: 1,544 finished units could not leave the Heavy Vehicle Plant and 408 Engineers could
  not leave a Gun Turret ("no free cell around"). Packed sites and parked units took every exit, so factories
  stalled with a unit inside. This weakens the army and worth numbers of all earlier batches; their points wins
  stand. Fixed in `ffb37bb` (sites keep two exits, moves avoid factory exits, parked units are moved aside),
  engine checks `--suite jev-exits`.
- `b8-exits` was stopped after about 15 minutes, lost to a harness issue: the test games played sound. Games and
  engine checks now run with `SDL_AUDIO_DRIVER=dummy`. `b9-exits` reruns it with the saved exe
  `runs/exes/ffb37bb-exits.exe`.
- Defense candidate `122ec8c` (exe `runs/exes/122ec8c-defense.exe`): Gun Turrets before contact at up to three
  distinct sites, scored by base buildings covered and the way enemies come (from remembered enemy buildings or
  recent sightings to the base); one real exit is enough (the user's rule: never take the last exit of a factory or
  building site). Replays of logged Engineer questions: turn 1 picks the Power Generator (0.57), turn 9 with power
  in place picks a turret (0.35); "stay" is high (0.24-0.36). No defense instruction added yet, to keep the first
  defense batch a facts-only change. Run at most 3 games at a time: the classic AI thinks for a fixed time, so a
  busier machine weakens it.

## 2026-11-02: recovered loop, reliable baseline, supply facts and Scanner probe

- Fixed parallel game-directory allocation with lifetime OS locks. New runs preserve prompt snapshots and hashes;
  filtered suite runs now have separate comparison pointers. Added paid-request and input-token thresholds to new
  bridges. Failed/incomplete/error-containing game batches now exit unsuccessfully.
- Valid baseline `b3-baseline` (100 turns, worlds/seeds 0:42, 3:7, 6:13): three wins, points 88/132/120,
  worth 316/582/716, losses 18/20/1. The old `b1` shared-directory runs are not valid controls. `b2-baseline` did
  not start games because sandboxed local sockets were denied; its old runner incorrectly exited successfully.
- Added read-only factory supply forecasts and selected the factory's own resource network. All 20 real-engine
  checks passed, including no state changes. `b4-power-facts`: three wins, points 6/143/136, worth 136/558/652,
  losses 23/12/2. Kept as factual controller corrections; these results do not establish a strength improvement.
- Replayed the original Light Vehicle Plant question with production rebalancing, supply, role and shorter
  instruction variants. None produced a reliable acceptable top pick. No speculative production wording was kept.
  Exact requests/replies are in `runs/prototypes/power/` (synthetic supply probes are hypothetical, not historical
  measurements of that saved factory).
- Mined `scanner-avoids-visible-fire` from a real Scanner loss. Explicit unarmed-spotter instructions improved the
  six-scenario suite from 12/18 acceptable, 3 forbidden to 15/18 acceptable, 0 forbidden, margin +0.17 to +0.27.
  `b5-scanner-safety` then lost world 3/seed 7 on turn 10; reverted the wording because it did not meet the game gate.
  Full batch: two wins, points 167/0/132, losses 18/2/0. This single stochastic batch does not establish that the
  sentence caused the loss. Prompt snapshots preserve the tested candidate.
- Added held-out `constructor-leaves-visible-fire`; tested once after the Scanner candidate was frozen: 1/3
  acceptable, zero forbidden (the other two picks were refills, outside either label). Do not tune on this case.
- Investigation of repeated attack choices found that executing computer attacks are cancelled outside firing
  range. A real-engine regression reproduced it. Next candidate starts the approach path before entering the
  firing scheduler. The candidate passed nine real-engine checks but `b6-attack-path` won only two of three:
  points 100/0/141, losses 9/34/2. Held-out `b7-attack-heldout` won world 2/seed 29 (153 points) and lost
  world 5/seed 101 (zero points). Did not promote: default attack behavior was restored; the candidate is saved
  in `tools/jev_experiments/attack-approach.patch` and can be tested in a generated source copy with
  `python3 test/run-production-preview-checks.py --suite jev-attack --attack-candidate`.
- All 14 game logs were complete without logged API/template errors. Structured result/provenance summaries are
  retained in `tools/jev_experiments/2026-11-02-results.json`; raw logs remain local under `runs/`. The tool's
  input-token price estimate for those games is about $0.93, excluding small replay/suite probes.
- No tactical strength gain is claimed. Next iteration should address defending the scoring base and the safety
  of approaches before re-testing attack pursuit; rotate to fresh held-out cases rather than tuning on b7.

## 2026-11-01: production wording, first loop iteration

Suite before: 12/15 acceptable, 0 forbidden, margin +0.28. The Light Vehicle Plant chooses to produce nothing at turn
11 (`light-plant-early-production`, 0.47 for nothing).

- Kept, as corrections of fact, not tuning: "produce nothing" says when storage is full that mined raw materials are
  lost; the Personnel Carrier has a role text; "an Engineer". Suite: 12/15, margin +0.28, no change.
- Reverted: a production rule for factories that make no combat units and no Constructors (produce Engineers,
  Surveyors, a Scout). Suite unchanged, nothing still chosen.
- Found: without the base part of the state Jev picks an Engineer (0.33); with power_left 1, nothing drops to 0.35.
  An idle factory may indeed have no power to start, so the honest fix is a fact on the production question saying
  whether the factory can run and what producing switches off. Controller work, next.

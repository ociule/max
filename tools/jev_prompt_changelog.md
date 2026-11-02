# Jev prompt changelog

One entry per prompt change tried with `tools/jev_suite.py` or a game batch: what changed, the scores before and
after, kept or reverted. Newest first. The suite's margin varies by about ±0.03 between runs of the same prompt.

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

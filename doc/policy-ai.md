# External policy controller

Select `--controllers policy,classic` to run an external policy against classic
AI. This controller is the experimental integration for direct weight
programming. It currently has an observation exporter and a small command
interface; it does not yet include a neural architecture, generated weights or
an Astra editor session.

Run the local transport smoke test from the repository root:

```sh
python3 tools/policy_match.py --name policy-smoke --end-turn -- \
  --controllers policy,none --turns 3 --seed 42
```

The runner uses `runs/base`, the Windows build at
`build/windows-hotkeys/max-port.exe`, Wine, and the same lifetime slot locks as
the Jev runner. It keeps `run.json`, `game.log`, `bridge.log`, `stderr.log` and
`observations.jsonl` in a new `runs/NAME/` directory. It does not overwrite a
previous run. End-turn mode tests the interface, not playing strength.

To supply a local policy, create a Python file with `choose(request)` returning
a command dictionary, then pass `--policy /absolute/path/policy.py:choose`
instead of `--end-turn`. The runner freezes that file in the run directory.
Additional imported files and weight artifacts are not automatically frozen;
record their hashes before using this runner for an actual model comparison.
There are no remote model calls in these tools.

For direct integration, start:

```sh
python3 tools/policy_bridge.py --end-turn --log runs/manual/observations.jsonl
```

Then run the game with the policy controller. The socket is loopback only,
port 47812 by default. Use bridge `--port N` and game environment variable
`MAX_POLICY_BRIDGE_PORT=N` to select another port. Jev uses its separate port.

## Protocol

Each message is a UTF-8 JSON object followed by a newline. Coordinates are
zero-based. IDs are the engine's unique `UnitInfo::GetId()`, not the per-type
display number. One observation permits exactly one reply:

```json
{"protocol":"max-policy-v1","episode":1,"id":17,"state":{"schema":"max-policy-observation-v1"}}
```

The example omits the actual state fields for space. The reply echoes `id`:

```json
{"id":17,"command":{"action":"move","unit":23,"at":[40,28]}}
```

The game supplies facts, not action scores or tactical candidates. A local
policy may keep its own memory, keyed by game process, episode and team. The
runner starts a fresh bridge per game. The engine reconnects at turn boundaries;
a connection alone is not an episode. Setup and loading start a new episode.

Reply errors, stale IDs, a missing bridge, transport failure and reply/order
timeouts end the team's turn with an explicit controller-failure log entry.
No other AI takes over. There is a 256-request cap per turn, a 15-second reply
deadline, a 30-second settling deadline and a 16 MiB message limit. Sending and
receiving are incremental, with bounded socket work per game-loop call.

## Observation version 1

`map` has `width`, `height` and three complete row-major arrays, indexed by
`y * width + x`:

- `terrain`: the world's base surface enum. MAX displays terrain outside scan
  coverage too; its minimap darkens terrain there rather than concealing it.
  Roads, bridges and platforms are separate entities, not hidden modifications
  read through a movement oracle.
- `visible`: current team scan coverage, as booleans.
- `resources`: `null` when unsurveyed, otherwise `[kind, amount]`, where kind
  is 0 raw, 1 fuel, 2 gold, and amount is the player-visible 0-16 density.
  Other teams' survey flags are never exported.

`own` contains individual unit/building records, including coordinates, current
attributes, health, movement, shots, ammo, cargo, orders, destinations, build
queues/rates/progress, mining, contained-unit IDs and economy network IDs.
`visible_enemies` contains currently visible entities' IDs, owners, types,
positions and health. It excludes private enemy orders, inventories, research
and attributes. Historical detection must not disclose live hidden positions.
`last_seen` contains observation-time snapshots, with `last_seen_turn`; an entry
does not imply the enemy still exists at that position. Memory starts afresh
after setup/loading and is not serialized into the classic AI save section.

`networks` lists own connected economy inventories, capacities, mining, power
and worker balances. Resource vectors are `[raw, fuel, gold]`. `research` entries
are `[level, turns_to_complete, allocation]` in `ResearchTopicType` order.
Other fields include turn, play mode, own clan/credits, public scores, victory
type/limit, command budget remaining and the previous command result.

`catalog` describes unit types and the team's current attributes and buildable
products. Attribute vectors follow `unitvalues.hpp`:
attack, rounds, range, armor, hits, speed, scan, turns, ammo, move-and-fire,
fuel, storage, attack radius, agent adjustment. Unit records retain their own
attribute version, so an old unit need not match the current catalog.

This initial profile still needs the plan's full field-by-field player-interface
audit, including detailed scan/detection distinctions, additional unit status
and available economic/research controls. Do not label it a frozen complete
observation or begin the full-game weight-construction experiment yet.
One known exporter gap is zero-health ground cover: rubble/slabs can legitimately
have zero hit points, but the current entity filter skips them. Temporary objects
with invalid engine IDs also need a separate non-actionable representation.

## Commands currently supported

Every observation advertises `commands`. Version 1 accepts:

```json
{"action":"end_turn"}
{"action":"move","unit":23,"at":[40,28]}
{"action":"attack","unit":23,"target":8194}
{"action":"sentry","unit":23}
{"action":"build","unit":24,"type":7,"at":[40,28],"rate":1}
{"action":"produce","unit":25,"type":0,"rate":2}
```

The numeric type examples are placeholders: use the observation catalog's enum
values. Build rates are 1, 2 or 4. Construction uses the exact supplied top-left
site and requires the builder to stand within its visible footprint. Move the
builder first when necessary. This slice does not pick or auto-walk to a site.
Build/produce replace the selected unit's queue and disable repeat; they do not
rebalance mining/power or guarantee sufficient supplies. Engine progress and
failure appear in subsequent observations.

Move submits a destination to the engine's normal pathfinder; accepting the
command does not guarantee arrival. Attack requires a currently visible valid
target and enters normal movement/firing processing. Command arguments are
validated before mutation. Unknown actions and malformed, foreign, fractional
or out-of-range IDs/coordinates are rejected rather than substituted.
The previous result is `accepted`, `rejected` or `settled`. Settled means the
engine returned to a stable state, not that the strategic objective succeeded.

Explicit factory/builder release, queue editing, maintenance, transfers,
transport loading, power/mining allocation, research, upgrades and the rest of
the player command surface are still pending. No full-game playing-strength
claim is supported by this first slice. Post-landing setup currently uses the
same classic setup hook as `NoneAi`; policy-controlled setup is later work.

## Checks

```sh
python3 -m unittest discover -s test -p 'policy_bridge_test.py' -v
python3 test/run-production-preview-checks.py --suite policy
```

The real-engine suite checks observations, information boundaries, command
validation and execution. Its report and an example snapshot live under
`build/policy/runtime/`.

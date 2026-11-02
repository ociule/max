# Recovered handover: Jev prompt improvement loop

Recovered from the working tree, commits, saved results and running processes on
2026-11-01 at approximately 22:41 Europe/Paris. This is a snapshot; existing games
were still running. This inspection did not restart games, call the model, change
source code, stop processes or commit anything.

## Repository state

- Branch: `ai_controller`, HEAD `0fc6d73`, four commits ahead of the locally known
  `origin/ai_controller`. No fetch was performed and nothing was staged.
- The only modified tracked file is `tools/jev_prompts.py` (+25/-10 lines).
- Eight pre-existing untracked files: `DEVELOPMENT.md`, `ai-controller-plan.md`,
  `code-review-2026-10-20.md`, `git-branch-plan.md`, `play-max-dev.command`,
  `test/aicontroller_runtime.cpp`, `test/run-ai-controller-checks.py`, and
  `tools/jev_prompt_changelog.md`. Most are older work; the plan and prompt
  changelog describe the current loop.
- `.git/info/exclude` excludes `runs/` and `.env`. Run evidence exists locally
  but will not travel with a normal git push. Do not print `.env`.

## What the loop was doing

The objective in `ai-controller-plan.md` is a Jev controller that can compete
with the classic AI over 100 turns. The current workflow is agent-driven;
the tools implement individual stages, rather than an autonomous optimizer.

1. Edit instructions or option wording in `tools/jev_prompts.py`. The game sends
   structured facts; the Python bridge renders the prompts, avoiding a rebuild
   for wording changes.
2. Run `tools/jev_suite.py`: replay saved questions, three asks per scenario,
   score acceptable and forbidden top picks and probability margins.
3. Validate candidates in games using `tools/jev_batch.py` and `jev_match.py`.
   The baseline configuration is worlds/seeds `0:42,3:7,6:13`, 100 turns,
   `jev,classic` controllers.
4. Mine failures using `tools/jev_review.py`; fix controller failures in C++
   and turn bad decisions from adequate facts into additional scenarios.

Recent local commits already contain the infrastructure:

- `ac967e2`: parallel batch runner and result table.
- `91ec4bc`: structured option facts, Python templates and failure review.
- `345a009`: attack orders now set their destination to the target cell.
- `0fc6d73`: scenario suite and first five scenarios.

## Last prompt iteration and intended next step

`tools/jev_prompt_changelog.md` records the first iteration. The retained edits
add a Personnel Carrier role, use “an Engineer”, and explain that idle production
with full raw-material storage wastes further mined materials. A proposed
production instruction was tried and reverted.

Four saved full-suite results all score **12/15 acceptable, zero forbidden**;
mean margins are approximately +0.26 to +0.28. The same four scenarios pass each
time. The Light Vehicle Plant scenario chooses `nothing` three times.
That scenario is explicitly labelled `unsure`: producing nothing may be sensible
if the factory cannot get power. The changelog says the next change should add
facts about whether production can run and what starting it switches off.
That controller change is not present in the working-tree diff.

The latest full run is `runs/suite/20261101-223635-jev_prompts.json`.
The later `20261101-223709-jev_prompts.json` is only the failing scenario (0/3),
and overwrote `last-jev_prompts.json`. Do not read that latest pointer as a full
suite regression. Results also lack prompt content hashes, so the changelog is
needed to identify which candidate was tried.

All five scenarios currently have `holdout: false`. The planned larger suite,
held-out validation and enforced spending cap are not established by these files.

## Live runs and a confirmed runner defect

The `b1` batch was still active during inspection: parent PID 4525, match PIDs
4532 and 4534, bridge PIDs 4541 and 4543, game PIDs 4546 and 4545.
At 22:40, the question logs were advancing around turns 25 and 32.
An older scratchpad match `e2` was also running (shell 96817, bridge 96820,
game 96841). PIDs are historical clues; recheck before acting.

**The two remaining b1 matches share a game directory.** Both
`runs/b1-w0-s42/run.json` and `runs/b1-w6-s13/run.json` say `slot: game1`.
`runs/slots/game1/.busy` contains PID 4534, despite both match processes being
alive. Its `stdout.txt` contains interleaved turn sequences from both games.
`jev_match.py:51-76` checks a lock then writes it without atomic acquisition,
allowing concurrent claimants. Settings and game output are therefore shared.
These two results cannot be used as independent baseline measurements.
Fix slot acquisition and ownership-aware release before another parallel batch;
preserve these logs and use a new label rather than rerunning `b1` with `--force`.

The separate `b1-w3-s7` run finished on turn 10: Jev/red rank 0, worth 93,
three losses; classic/green rank 1, worth 143. In this engine rank 0 represents
an eliminated team, not first place. The review tool flags a Scanner that moved
into danger 24 on turn 7 and was subsequently lost. This run alone is insufficient
to establish a robust baseline.

## Verification performed during recovery

- `git diff --check` passed.
- All Python files in `tools/` and the untracked harness passed syntax parsing.
- Current templates rendered all five scenarios (41 options) without exceptions.
- Current templates rendered 406 logged b1 questions without exceptions; the
  inspected entries contained no logged API or template errors.
- Ran the read-only failure review on the completed `b1-w3-s7` game.
- No C++ rebuild, fresh gameplay validation or paid model requests were made.

The safest next work is to repair the runner, establish an uncontaminated
baseline, then investigate factory power facts before further prompt tuning.

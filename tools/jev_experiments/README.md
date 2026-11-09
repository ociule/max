# Jev experiment records

`2026-11-02-results.json` preserves summary metrics and available provenance for
14 completed games. Full logs live under the git-ignored `runs/` directory.

`2026-11-03-results.json` preserves the three completed b18 games and three
60-turn b19 economy checks, with their executable/prompt provenance and limits.
The later construction-budget and base-network fixes (commit "Jev construction
within its cargo...", runs/exes/b20-budget-base.exe) are not part of the b19
game executable.

`attack-approach.patch` is a rejected candidate, not part of the default controller.
It fixes the mechanically incorrect out-of-range firing state but failed the
live-game promotion gate. Test a generated copy with:

```sh
python3 test/run-production-preview-checks.py --suite jev-attack --attack-candidate
```

Use `HANDOVER-2026-11-02.md` and `tools/jev_prompt_changelog.md` for decisions and
limitations. Do not infer strength improvements from a single stochastic batch
or promote a change solely because its scenario score rises.

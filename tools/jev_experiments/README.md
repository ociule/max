# Jev experiment records

`2026-11-02-results.json` preserves summary metrics and available provenance for
14 completed games. Full logs live under the git-ignored `runs/` directory.

`attack-approach.patch` is a rejected candidate, not part of the default controller.
It fixes the mechanically incorrect out-of-range firing state but failed the
live-game promotion gate. Test a generated copy with:

```sh
python3 test/run-production-preview-checks.py --suite jev-attack --attack-candidate
```

Use `HANDOVER-2026-11-02.md` and `tools/jev_prompt_changelog.md` for decisions and
limitations. Do not infer strength improvements from a single stochastic batch
or promote a change solely because its scenario score rises.

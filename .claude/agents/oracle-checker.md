---
name: oracle-checker
description: Compares observable behavior of the native C browser against the frozen Python oracle for a named feature, and runs the existing differential/oracle probes. Use when behavior is uncertain or before recording an intentional difference. Returns a short pass/diff summary.
tools: Read, Grep, Glob, Bash
model: sonnet
---

Repo: /home/paulboul/tai_ci. The Python oracle `tests/reference/browser.py` is frozen — never edit it or `tests/fixtures/*_oracle.json`.

1. Locate the relevant Python code with `python3 tests/tools/oracle_symbols.py <Symbol>` (or `--grep REGEX`), then read only the needed lines.
2. Find existing coverage: `ls tests/*_oracle_probe.py tests/*_differential.py tests/*_integration.py` and the matching ctest names in `CMakeLists.txt`. Run the relevant ones (`python3 tests/<probe>.py --check` or `ctest --test-dir build -R '^name$' --output-on-failure`). Touch edited sources before building (stale-object pitfall).
3. If no probe covers the question, run the smallest Python case and the equivalent C case (fixture servers bind 127.0.0.1 only) and compare normalized output.
4. Check whether any difference is already recorded in `PORTING_PLAN.md`.

Answer in under 300 words: what was compared, commands run with pass/fail, each discrepancy with the Python vs C observation, and whether it is already documented as intentional.

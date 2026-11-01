# Plumb config

## Before any edit

This rule applies in every lane and at every rigor, including Light and Direct.
CLAUDE.md「決策」 requires it, and it overrides the plumb lane steps.

1. Explain the plan step by step, in Traditional Chinese.
2. Draw the plan as a PNG. Draw the problem and the proposed fix as separate
   images when both exist. Save them in `build/diagrams/` (ignored by git) and
   send them to the user.
3. State the decision, what it improves, and its pros and cons.
4. Wait for the user to agree. Then edit.

Diagnosis (reading code, running a repro, reading traces) is not an edit and
can run before the plan.

## Tracker

GitHub via `gh` (`paulboul1013/tai_ci`). Slices are tracked in
`PORTING_PLAN.md` and `ACCEPTANCE.md`, not issues; keep them in sync with the
`project-records` skill.

## Checks

- Build: `cmake --build build` (ASan: `cmake --build build-asan`)
- One test: `ctest --test-dir build -R '^<name>$' --output-on-failure`
- Full suite: `ctest --test-dir build -j"$(nproc)"`
- Sanitizers: `ctest --test-dir build-asan -j"$(nproc)"`
- Registration: `python3 tests/tools/check_test_registration.py` (also in CTest)

## Run surface

`tests/tools/window_session.sh` drives a real `tai-browser --window` on a
private Xvfb (see the `native-window-verification` skill). Set
`BROWSER_TRACE_FILE` for every window run and read the trace afterwards.
Start Xvfb only through `tests/tools/xvfb.sh`; `xvfb_owner` fails otherwise.

## Domain docs

Glossary: `CONTEXT.md`
ADRs: none
Layout: single context

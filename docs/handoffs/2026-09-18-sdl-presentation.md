# Handoff — next slice: SDL3 presentation

## Objective

Implement the next native browser vertical slice: present the existing
self-contained Cairo raster output in an SDL3 window. Keep the initial scope
to window creation, presentation, deterministic resize/quit handling, and
headless-contract preservation. Do not add DOM input dispatch, interactive
scroll, tabs/history, general HTML images, remote image loading, or WebP.

## Start here

- Repository router and authority: [`AGENTS.md`](../../AGENTS.md)
- Current migration state: [`PORTING_PLAN.md`](../../PORTING_PLAN.md)
- Browser/runtime ownership: [`docs/architecture/native-runtime.md`](../architecture/native-runtime.md)
- Render/output contract: [`docs/reference-render-contract.md`](../reference-render-contract.md)
- Whole-project limits: [`ACCEPTANCE.md`](../../ACCEPTANCE.md)
- Frozen Python window/raster reference: [`tests/reference/browser.py`](../../tests/reference/browser.py)

The task matches the native-stack/SDL, architecture/ownership,
validation/completion, execution-workflow, and project-records branches in
`AGENTS.md`; read each routed reference in full before changing code.

## Repository checkpoint

- Branch: `main`; worktree was clean immediately after commit/push.
- Latest commit: `96489d8 Add OpenMoji image raster slice`, pushed to
  `origin/main`.
- The immediately preceding image slice is complete at its stated comparison
  boundary. Its exact behavior, intentional differences, ownership, and
  evidence are recorded in the render contract and porting plan; do not
  restate or weaken them.
- Paint/raster and Browser/window both remain `VALIDATING`. SDL presentation
  is not complete merely because headless Cairo PNG output works.

## Current executable seam

`TaiPage` produces an immutable document-coordinate `TaiDisplayList` and the
existing Cairo backend renders it synchronously to PNG. The headless CLI
already uses that path, including a fixed 800×532 viewport screenshot
contract. Preserve this path and its tests unchanged while introducing the
window adapter. The display list must never retain mutable DOM/layout pointers;
a future SDL frame may consume only self-contained raster/display state.

The current render contract explicitly notes that Cairo ARGB32 is
premultiplied/native-endian and has not yet been converted to SDL presentation.
Treat pixel-format conversion, texture ownership, resize lifecycle, and
shutdown order as explicit ownership decisions, not incidental glue.

## Suggested incremental plan

1. Inspect the frozen Python `BrowserWindow`/`RasterWindowState` presentation
   behavior and establish a small deterministic native test seam. Confirm the
   installed SDL3 capability before committing to renderer versus surface APIs.
2. Add a minimal opaque SDL presentation module behind a small interface:
   create window/resources, accept one bounded raster frame, present, destroy.
   Keep Cairo image conversion and SDL texture lifetime inside that module.
3. Connect one current `TaiPage` raster frame to the module without changing
   the headless CLI's JSON or `--screenshot` behavior.
4. Add deterministic resize and quit handling only. Ensure resize creates no
   stale texture/surface references and destruction is safe after partial
   initialization.
5. Validate a window smoke flow where the environment supports it, plus unit
   tests for pixel format/conversion and lifecycle errors. Keep CI-safe tests
   headless where possible.
6. Run focused tests after each increment, then full Debug and ASan/UBSan.
   Separate sandbox localhost fixture failures from sanitizer evidence.
7. Obtain an independent review, resolve findings, and update only the
   affected project records with evidence. Do not mark Browser/window complete
   until its broader acceptance criteria are met.

## Last verification evidence

- Debug CTest: 22/22 passed after the image slice.
- Focused ASan/UBSan for render and image differential passed with
  `ASAN_OPTIONS=detect_leaks=0`.
- A prior full ASan/UBSan suite passed all non-localhost tests in the sandbox;
  the localhost network differential was rerun outside the sandbox and passed.
  This is not LeakSanitizer evidence.
- `git diff --check` passed before committing.
- Independent image review findings were resolved before `96489d8`; see the
  commit and the referenced contracts for the resulting constraints.

## Suggested skills

1. `test-driven-development` — establish presentation/lifecycle RED tests.
2. `incremental-implementation` — land a small executable SDL vertical slice.
3. `security-and-hardening` — validate raster dimensions, byte arithmetic, and
   external window/event inputs.
4. `codebase-design` — keep the SDL adapter deep with a narrow ownership
   interface.
5. `code-review-and-quality` — independent review before declaring the slice
   validated.

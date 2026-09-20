# Handoff — next slice: interactive window page scroll

## Objective

Make `tai-browser --window URL` scroll the current `TaiPage` in response to
SDL wheel and PageUp/PageDown input. Repaint only when the already-clamped page
scroll value changes. Preserve the current resize, expose, texture replacement,
and headless contracts.

Do not add DOM click dispatch, form input, tabs, history, Chrome UI, general
images, remote images, or WebP in this slice.

## Start here

- Repository router: [`AGENTS.md`](../../AGENTS.md)
- Current migration record: [`PORTING_PLAN.md`](../../PORTING_PLAN.md)
- Page/display/SDL ownership: [`docs/architecture/native-runtime.md`](../architecture/native-runtime.md)
- Rendering and window contract: [`docs/reference-render-contract.md`](../reference-render-contract.md)
- Completed predecessor: [`docs/handoff/2026-09-20-window-resize-relayout.md`](2026-09-20-window-resize-relayout.md)
- Frozen input/oracle implementation: [`tests/reference/browser.py`](../../tests/reference/browser.py)

This task matches the SDL/native-stack, architecture/ownership,
validation/completion, execution-workflow, and project-records routes in
`AGENTS.md`; read their matching references before changing code.

## Repository checkpoint

- The worktree is deliberately dirty. It contains the uncommitted SDL
  presentation slice and its resize-relayout follow-up; preserve all changes.
- `TaiPage` owns viewport dimensions, an immutable self-contained display list,
  and clamped page `scroll_y`. See `include/tai/browser.h` and `src/browser.c`.
- `tai_page_set_scroll_y` already validates finite input and clamps to
  `tai_page_max_scroll_y`. `tai_page_write_viewport_png` and SDL raster use
  the same scroll state.
- `tai_present_window` borrows `TaiPage` on the caller thread. It owns SDL
  video/window/renderer/texture and replaces the retained texture only after a
  successful raster/upload/present. See `include/tai/presentation.h` and
  `src/presentation.c`.
- Resize reflow is complete: initial physical pixel size and nonzero
  `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` rebuild the page layout/display list;
  oversized event dimensions are rejected before reflow.
- `tests/test_presentation.c` uses SDL dummy events and a worker only to push a
  quit event; it must not make window-management calls off the caller thread.

## Required behavior and seam

Keep presentation as the narrow event adapter and keep scroll ownership inside
`TaiPage`.

- On an accepted wheel or PageUp/PageDown event for this window, compute a
  finite proposed page scroll and call `tai_page_set_scroll_y`.
- If the clamped value is unchanged, do not raster, upload, create, or replace
  a texture.
- If it changes, raster the current immutable display list with the new page
  scroll and use the existing successful-present texture replacement path.
- Invalid/unrelated SDL events are no-ops. Do not mutate the display list.
- Keep all SDL resource work on the caller thread. Retain the old texture on
  raster/upload/present failure and exit through the established cleanup path.
- Choose scroll increments from the frozen Python behavior only after an
  executable oracle check; record any intentional difference in
  `PORTING_PLAN.md`.

## Suggested incremental plan

1. Inspect the frozen Python wheel/key handling and run a minimal oracle probe
   to establish direction, units, and PageUp/PageDown increments.
2. Add RED `TaiPage`/presentation tests for scroll direction, clamp/no-op, and
   a changed frame path using deterministic SDL dummy events.
3. Add the smallest presentation-private event handling that calls the existing
   `TaiPage` scroll interface and reuses its texture replacement helper.
4. Compare a focused native scroll/layout/display result with Python at the
   content viewport boundary; Chrome/tabs remain outside that boundary.
5. Run focused tests, Debug CTest, ASan/UBSan with `detect_leaks=0`, and an
   independent ownership/event review. Update only affected contracts and
   migration records after evidence exists.

## Current verification evidence

- Debug CTest: 24/24 passed after resize relayout.
- ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0`: every non-localhost test
  passed; the localhost network differential is blocked by sandbox bind policy
  and was rerun successfully in a loopback-permitted environment.
- LeakSanitizer is not a passing claim: Fontconfig/SDL system-library
  allocations remain inconclusive.
- `git diff --check` passed.
- Independent review found and the implementation resolved initial physical
  pixel-size relayout, oversized resize prevalidation, unsupported test-thread
  window access, and stale test-pointer evidence.

## Suggested skills

1. `test-driven-development` — establish scroll behavior before implementation.
2. `incremental-implementation` — keep page and SDL changes independently
   buildable.
3. `codebase-design` — preserve the small `TaiPage`/presentation seam.
4. `security-and-hardening` — validate event-derived deltas and dimensions.
5. `code-review-and-quality` — independently review texture lifetime and event
   ownership before declaring the slice validated.

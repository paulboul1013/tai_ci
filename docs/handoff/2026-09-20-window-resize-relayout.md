# Handoff — next slice: window resize relayout

## Objective

Make `tai-browser --window URL` rebuild the page layout and immutable display
list when SDL reports a non-zero pixel-size change. The newly presented frame
must use the new viewport width and height, retain a clamped page scroll value,
and replace the SDL texture only after a successful raster/upload/present.

Do not add DOM input dispatch, interactive scroll input, tabs, history, Chrome
UI, remote images, or WebP. Keep the existing headless JSON and `--screenshot`
contracts unchanged.

## Start here

- Repository authority: [`AGENTS.md`](../../AGENTS.md)
- Migration state and known discrepancy: [`PORTING_PLAN.md`](../../PORTING_PLAN.md)
- Current SDL/output contract: [`docs/reference-presentation.md`](../reference-presentation.md)
- Ownership and dependency direction: [`docs/architecture/native-runtime.md`](../architecture/native-runtime.md)
- Frozen Python resize oracle: [`tests/reference/browser.py`](../../tests/reference/browser.py), `BrowserWindow.resize`

This work matches the SDL/native-stack, architecture/ownership,
validation/completion, execution-workflow, and project-records routes in
`AGENTS.md`; read their matching references before changing code.

## Repository checkpoint

- Current branch/worktree includes the uncommitted SDL presentation slice. Do
  not overwrite or discard its changes.
- `tai_presentation` owns SDL video, window, renderer, and texture. It borrows
  an immutable `TaiDisplayList` for the blocking `tai_present_window` call.
- `TaiPage` owns document, styles, JavaScript, layout, display list, viewport
  dimensions, and clamped page scroll. It currently creates layout/display once
  in `tai_page_load`; no public resize operation exists.
- Current resize handling rerasterizes that original display list at the new
  texture dimensions. This preserves lifetime safety but differs from Python:
  reflow and line wrapping remain at 800px.
- Latest evidence is Debug CTest 23/23 and ASan/UBSan 23/23 with
  `ASAN_OPTIONS=detect_leaks=0`. LeakSanitizer remains inconclusive because
  Fontconfig/SDL system-library allocations are reported; do not describe this
  as leak-clean.

## Required seam and ownership

Introduce a small page-level operation, expected to be shaped like a resize
request that accepts finite positive viewport dimensions and either succeeds
atomically or leaves the old page state intact.

- `TaiPage` creates the replacement `TaiLayout` from its existing styled DOM,
  then a replacement self-contained `TaiDisplayList`.
- The replacement layout owns its own font and image cache; the replacement
  display list owns its copied draw data. It may outlive the old layout.
- Only after both replacement objects exist may `TaiPage` swap them in, update
  viewport dimensions, clamp `scroll_y`, then destroy old display before old
  layout.
- On allocation, layout, or display-list failure, destroy only replacement
  resources and retain the old page/display/viewport/scroll state.
- Presentation must query the current page display only after a successful page
  resize. It must keep the old texture if reflow/raster/upload/present fails.
- All work remains on the calling SDL/browser thread. Do not introduce worker
  queues or make SDL resources cross thread boundaries.

## Incremental plan

1. Establish RED tests for `TaiPage` resize: width-sensitive line wrapping,
   viewport dimension update, scroll clamping after shorter content range, and
   invalid-input no-op behavior.
2. Implement the page resize seam in `browser.h`/`browser.c` with atomic swap
   and documented ownership/error behavior. Verify the existing headless PNG
   and JSON paths remain byte/shape compatible at their fixed viewport.
3. Change the presentation interface to borrow `TaiPage`, or add a narrow
   callback that obtains a newly resized display list. On
   `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`, call the page seam before rastering.
4. Add a deterministic SDL dummy/offscreen test that injects a resize event and
   then quit. Assert that the resulting layout/display has the new line geometry
   and that a failed resize preserves a valid, presentable old texture/state.
5. Compare a focused width-sensitive Python case with native layout/display
   output. The comparison boundary is page content viewport only: native does
   not yet implement Python Chrome or tabs.
6. Run focused tests after each increment, then Debug CTest, ASan/UBSan, and an
   independent review. Update only the affected architecture, render contract,
   and migration records after evidence exists.

## Acceptance evidence

- A non-zero SDL resize causes a new `TaiLayout` and `TaiDisplayList` using the
  new viewport width, rather than scaling or rerasterizing the 800px list.
- Resize preserves valid page ownership and no stale layout/display/texture is
  used after a successful swap or a failure.
- Headless JSON and `--screenshot` behavior remain unchanged.
- CTest covers page resize, SDL resize/quit, invalid dimensions, and an
  allocation/error path where practical.
- Python/native comparison covers at least one layout whose wrapping changes
  with width; output comparison uses layout/display structure rather than
  backend-sensitive glyph pixels.

## Suggested skills

1. `test-driven-development` — create RED page and SDL resize tests first.
2. `incremental-implementation` — keep each ownership and integration change
   buildable and independently verified.
3. `codebase-design` — make the page-to-presentation resize seam narrow.
4. `security-and-hardening` — validate SDL dimensions and allocation arithmetic.
5. `code-review-and-quality` — independently review atomic swap and cleanup.

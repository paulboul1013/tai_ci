# Native Runtime and Ownership

## Dependency direction

The native dependency direction is core → URL/DOM → network/CSS/JavaScript →
layout → display list/raster → page orchestration → tab set / SDL presentation
or CLI. The scheduler remains an explicit service until browser/window
orchestration integrates it.

Public headers expose subsystem contracts; opaque handles isolate internal
state where practical. Subsystems that require direct DOM access share the one
canonical `TaiNode` representation in `include/tai/dom.h`; they do not define
private, incompatible node layouts.

## Ownership

- `TaiDocument` owns every DOM node. Parent links and layout/JavaScript node
  references are borrowed; JavaScript and layout activity ends before document
  destruction. Detached nodes remain alive until document destruction, so DOM
  mutation cannot leave JavaScript handles dangling.
- `TaiStylesheet` owns selectors and declarations. Computed style owns its
  string values.
- `TaiResponse` owns headers and body. Queue handoff transfers response
  ownership, and cancellation still destroys payloads.
- `TaiLayout` borrows the styled DOM and owns layout nodes, words, font
  state, and successfully decoded OpenMoji cache entries. PNG decoding uses
  the existing Cairo dependency; missing/corrupt assets are not cached.
  Construction reads styles and only mutates a fixed `overflow: scroll`
  node to persist its clamped `scroll_y`, matching the reference owner-thread
  behavior.
- `TaiDisplayList` copies command text, font names, colors, geometry, decoded
  premultiplied image pixels, rounded
  clip radii, scroll offsets, blur sigma, opacity/blend scalars, and scalar DOM node IDs; it keeps no DOM or
  layout pointers and can outlive both. Raw hit results retain only copied ID,
  kind, and bounds. `TaiPage` owns viewport dimensions and the top-level page
  scroll offset, converts viewport coordinates to document coordinates once,
  and resolves the copied ID through its live document. Explicit scroll
  changes clamp to current overflow; resize preserves the prior top-level
  offset even when it exceeds the new maximum, matching Python `Tab.resize`.
- Cairo contexts and surfaces are created and destroyed inside synchronous
  PNG or memory raster calls. The memory call returns an owned opaque ARGB32
  copy. The presentation adapter borrows `TaiPage`, owns SDL video/window,
  renderer and texture on the caller thread, and obtains a display-list borrow
  only while rastering a frame. Page resize atomically replaces layout then its
  self-contained display list before the adapter uploads a new frame; failed
  replacement construction restores DOM scroll state and leaves the old page
  fields intact. The adapter replaces the texture only after the new frame is
  presented; exposed windows repaint the retained texture. On its caller thread
  it adapts only accepted window wheel, PageUp/PageDown, and up/down arrow
  events into finite proposals through `TaiPage`'s clamped page-scroll
  interface; an unchanged clamp leaves the retained texture untouched.
  Each present composes a stateless scrollbar thumb from borrowed page scroll
  and viewport values above the retained texture. The overlay owns no resource
  and never mutates the display list or Cairo pixels; expose recomposes it.
  The window adapter also accepts pointer, key, and text events only for its
  own SDL window ID. SDL text input runs only while that window is focused and
  the current page has an active text/password control; focus loss and adapter
  cleanup stop it. Backspace, left/right, and Return go through the focused
  page-control seam, while PageUp/PageDown and up/down retain page-scroll
  behavior.
  A future raster worker may receive a self-contained display list, never
  mutable DOM state.

- Window navigation keeps network and page-session ownership outside
  `TaiPage`. A page owns at most one pending `TaiNavigationIntent`, including
  its resolved URL and optional POST body; taking the intent transfers
  ownership to the caller. `tai_present_window_with_navigation` lends that
  intent to the outer owner, which builds a candidate with the old page URL as
  referrer and the existing viewport. The caller-owned page slot changes only
  after candidate load succeeds, at which point the old page is destroyed. A
  failed load leaves the old page and its DOM/layout/display list live. Any
  handled intent is a repaint boundary, so presentation paints whichever page
  remains current without retaining a pointer to a page the callback may have
  freed. The compatibility `tai_present_window` API has no navigation
  callback; callers that need link/form navigation use the callback variant.
  Fragment scrolling runs after the candidate layout exists, before the first
  frame is presented.
  `TaiSession` owns one live page and an array of copied URL strings; it borrows
  network and default CSS. A successful new navigation commits a loaded
  candidate and a new history entry together, discarding forward URLs. Back
  and Forward load the selected URL as GET before replacing the page and index;
  failure preserves both. A same-document fragment activation carries an owned
  URL signal from page to presentation and session. If history allocation fails,
  the page restores its prior URL and scroll before the event ends. The
  presentation adapter routes focused Alt+Left/Alt+Right for its own SDL
  window to the session callback; ordinary arrows remain page-control input.
  `TaiSession` also normalizes address submissions and loads them as GET; a
  successful candidate and its URL history entry commit together.
  The chrome-enabled entry point, `tai_present_window_with_chrome`, borrows its
  callback table and userdata for the duration of the call. Its address editor
  owns its UTF-8 text buffer. The adapter owns separate page and chrome SDL
  textures; chrome drawing uses a temporary Cairo image surface/context, which
  is destroyed after texture upload. Both textures are destroyed before the
  renderer/window/SDL resources during cleanup.
  The chrome adapter borrows the session/history callbacks and owns no page or
  network state. Its geometry, rendering, and event-routing contract is recorded
  in [`docs/reference-presentation.md`](../reference-presentation.md).

- `--window` now enters through `tai_present_window_with_tabs`. The presentation
  adapter creates the SDL window before starting the initial navigation and
  keeps polling the tab set while waiting for SDL events. It owns the address
  editor, chrome/page textures, and all SDL resources on the window thread.
  The legacy `tai_present_window_with_chrome` remains available for the
  synchronous single-session adapter.
- `TaiTabSet` owns ordered tab slots, the active index, and one `TaiSession` per
  slot. It borrows default CSS and owns the New Tab URL. Each session owns its
  committed page and copied URL history; in-flight URL/history state is exposed
  provisionally without changing the committed session. A successful document
  commit replaces the provisional state. An initial transport failure commits
  the visible Network Error page and requested URL; a later transport failure
  discards its candidate and provisional history while preserving the prior
  page and committed history, an intentional native difference recorded in
  [`PORTING_PLAN.md`](../../PORTING_PLAN.md).
- The tab set starts one loader thread. That thread creates, exclusively uses,
  and destroys the shared `TaiNetwork`; document and external-resource requests
  use its submit/poll API. It also owns a page candidate through parsing,
  script/style application, layout, and display-list construction. Completion
  carries the stable tab ID and navigation generation plus the candidate page.
  The SDL thread rejects stale generations and takes page ownership only while
  committing to the matching session. Replaced tasks are canceled by the
  loader thread; tab-set shutdown marks pending work canceled, joins the loader,
  drains completions, then destroys sessions. Destruction runs on the window
  owner after it has stopped issuing tab-set operations. No loader path touches
  SDL.
- A navigation snapshots its Referer from the same tab's committed page URL;
  if it supersedes a pending navigation, it uses that provisional URL, matching
  Python's capture-before-assignment behavior. Other tabs never supply a
  Referer to the request.
- Window resize updates every committed session page on the SDL thread. A
  pending candidate is resized to the latest viewport immediately before
  commit. Tab selection changes only the active index; it does not move page,
  history, pending task, or scroll ownership between sessions.

## Current native rendering boundary

`tai_layout_visit` exposes flat borrowed geometry synchronously on the owner
thread. `tai_layout_walk` adds paired enter/leave events at the same seam so the
display-list builder can preserve effect nesting without exposing layout
implementation structs. `tai_display_list_write_png` and
`tai_display_list_hit_test` consume the resulting self-contained list. Hit
testing shares flat paint ordering and paired clip/scroll/blur/blend effects with raster;
rounded raster-only clips preserve the frozen Python Blend-mask hit semantics.
Blur owns no persistent surface: each synchronous raster call owns and deterministically
destroys its Cairo group pattern, Gaussian kernel, and two temporary pixel buffers on both
success and failure.
The small `TaiPage` adapter applies the shared viewport-to-document origin to hit
testing and viewport PNG raster, then resolves a successful hit to a live DOM
node. The display list remains immutable document-coordinate state. The CLI
owns the page/network/URL lifecycle and uses one
cleanup path for screenshot success and failure. Observable dimensions,
supported commands, coordinate semantics, test evidence, and missing paint
features belong to
[`docs/reference-display-raster.md`](../reference-display-raster.md).

## Runtime resources

The default CSS is an install-time resource rather than page-owned memory. Its
lookup order and installation evidence are tracked as migration state in
[`PORTING_PLAN.md`](../../PORTING_PLAN.md); the architectural constraint is that
checkout-specific absolute paths stay out of the binary.

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
  own SDL window ID. It converts finite button coordinates from SDL window
  units to physical pixels once before chrome/tab/page routing, using the
  window's measured logical and pixel sizes. Invalid conversion is an ignored
  click. SDL text input runs only while that window is focused and
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

- `--window` enters through `tai_present_browser(app, ...)` with a
  `TaiBrowserApp` owned by `main`. `src/presentation_tabs.c` owns every window
  on the SDL thread: per window the SDL window, renderer, chrome/page textures,
  the address editor, an owned copy of the active tab's last shown URL (used
  once per loop iteration to discard the draft when that URL changes), and a
  `TaiTabSet` created in the app. A window is created before its initial
  navigation starts. Each loop iteration routes one SDL event to its window,
  closes windows that asked to close (tab set first, then SDL resources), opens
  a Ctrl+N window, then pumps and repaints every window. The loop ends when no
  window remains or on `SDL_EVENT_QUIT`, destroying all windows before
  `SDL_Quit`; `main` destroys the app afterwards. `tai_present_window_with_tabs`
  presents one caller-owned tab set through the same loop without Ctrl+N.
  The legacy `tai_present_window_with_chrome` remains available for the
  synchronous single-session adapter.
- `TaiBrowserApp` is the process-wide state every window shares, matching
  Python's `BrowserApp`: the loader thread and its `TaiNetwork` (one cookie
  jar), the bookmark collection, the New Tab URL, default CSS (borrowed), RTL,
  the app-wide tab ID counter, and a registry of the tab sets created in it.
  The registry, IDs and bookmarks belong to the SDL owner thread. The app must
  outlive every tab set created with `tai_tabset_create_in_app`; the
  single-window `tai_tabset_create*` constructors build a private app that the
  tab set owns and destroys with itself.
- `TaiTabSet` is one window's tabs: ordered tab slots, the active index, one
  `TaiSession` per slot, and an inbox of completions routed to it. It borrows
  its app. Each session owns its
  committed page and copied URL history; in-flight URL/history state is exposed
  provisionally without changing the committed session
  (`tai_tabset_history_url` reads that provisional list). A successful
  document commit replaces the provisional state. A transport or certificate
  failure commits its Network/Certificate Error page like any other document,
  as Python does: an ordinary navigation appends the requested URL and drops
  forward entries, and a Back/Forward traversal keeps its target index. A
  superseded or cancelled load commits nothing, so Back while a navigation is
  pending drops the provisional entry (an intentional difference recorded in
  [`PORTING_PLAN.md`](../../PORTING_PLAN.md)).
- The app starts one loader thread. That thread creates, exclusively uses,
  and destroys the shared `TaiNetwork`; document and external-resource requests
  use its submit/poll API. It also owns a page candidate through parsing,
  script/style application, layout, and display-list construction. Completion
  carries the stable tab ID and navigation generation plus the candidate page;
  a task never points at its tab set. Any tab set's pump moves the app's
  published completions to the inbox of the tab set whose slots contain that
  tab ID (releasing those whose window is gone), then commits its own inbox.
  The SDL thread rejects stale generations and takes page ownership only while
  committing to the matching session. Replaced tasks are canceled by the
  loader thread. Destroying a tab set marks its pending work canceled and
  unregisters it without waiting for the loader; the loader later publishes
  those cancelled tasks and the next pump or app destruction frees them. App
  shutdown cancels queued work, joins the loader, then drains completions.
  Destruction runs on the window owner after it has stopped issuing
  operations. No loader path touches SDL.
- A navigation snapshots its Referer from the same tab's committed page URL;
  if it supersedes a pending navigation, it uses that provisional URL, matching
  Python's capture-before-assignment behavior. Other tabs never supply a
  Referer to the request.
- `TaiBrowserApp` owns one `TaiBookmarks` collection shared by all tabs and
  windows; chrome
  reads it only through `TaiTabSetView.bookmarkable/bookmarked`. Toggle,
  lookup, and the `about:bookmarks` snapshot run on the SDL thread. Starting an
  `about:bookmarks` navigation copies a sorted snapshot into generated HTML
  owned by the load task, so the loader thread never reads the mutable
  collection. `tai_browser_app_create` (and so `tai_tabset_create`) opens the
  per-user file
  (`$XDG_DATA_HOME/tai-browser/bookmarks`, else
  `~/.local/share/tai-browser/bookmarks`); each toggle writes a temporary file,
  fsyncs, and renames it before changing memory, so a failed write leaves both
  unchanged. If the file cannot be opened or parsed, the app warns on
  stderr, keeps the file untouched, and uses a memory-only collection.
  The `_with_home_url` and `_for_test` variants are always memory-only.
- Page security is a property of the committed `TaiPage`, not extra tab-set
  state: `tai_page_secure()` is true when the page's requested URL is `https`
  and its document response had no transport or certificate error (redirects
  do not change it). `TaiTabSetView.secure` reads the active tab's committed
  page, so a pending navigation keeps the previous page's lock, an error page
  has none, and every tab keeps its own value. The SDL thread is the only
  reader.
- `tai_browser_app_create_for_test` (and `tai_tabset_create_for_test`)
  additionally copies a CA bundle path that the
  loader thread applies with `tai_network_set_ca_file()` right after creating
  its `TaiNetwork` and before reporting readiness; the string is written
  before `pthread_create` and only read by the loader afterwards. Only
  integration tests call it: `tai-browser` never trusts a test CA and reads no
  environment variable for trust roots.
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

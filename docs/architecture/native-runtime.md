# Native Runtime and Ownership

## Dependency direction

The native dependency direction is core → URL/DOM → network/CSS/JavaScript →
layout → display list/raster → page orchestration → CLI or future window layer.
The scheduler remains an explicit service until browser/window orchestration
integrates it.

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
  kind, and bounds. `TaiPage` owns viewport dimensions and clamped page scroll,
  converts viewport coordinates to document coordinates once, and resolves the
  copied ID through its live document.
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
  A future raster worker may receive a self-contained display list, never
  mutable DOM state.

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
[`docs/reference-render-contract.md`](../reference-render-contract.md).

## Runtime resources

The default CSS is an install-time resource rather than page-owned memory. Its
lookup order and installation evidence are tracked as migration state in
[`PORTING_PLAN.md`](../../PORTING_PLAN.md); the architectural constraint is that
checkout-specific absolute paths stay out of the binary.

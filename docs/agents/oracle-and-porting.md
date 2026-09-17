# Oracle and Porting

## Porting target

The Python implementation is an executable specification. Preserve observable
architecture boundaries, rendering semantics, event flow, networking behavior,
JavaScript integration, and testable browser behavior. Reconstruct equivalent C
abstractions; do not mechanically map Python classes to large structs.

Derive the real dependency graph from the repository before changing migration
order. Work in dependency order through executable vertical slices, keeping the
C project buildable, runnable, and testable. A typical—not authoritative—flow
is core utilities → URL/HTTP → HTML/DOM → CSS/style → layout → display list →
raster → window/input → JavaScript → scheduling/orchestration.

## Oracle procedure

When behavior is uncertain or documentation conflicts with implementation:

1. Locate the relevant Python implementation and tests.
2. Run the smallest representative Python case and capture normalized output.
3. Add or run the equivalent C case.
4. Compare observable results and explain every discrepancy.

Compare DOM structure, CSS rules, computed style, layout geometry, display
lists, text measurements, network results, JavaScript-visible behavior, event
dispatch, and rendered artifacts as applicable. Prefer structural or key-region
comparisons when pixel-perfect output would be backend- or font-sensitive.

An intentional difference is complete only when its reason, observable impact,
and migration consequence are documented in `PORTING_PLAN.md`.

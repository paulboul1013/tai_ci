# Architecture and Ownership

## Boundaries

Preserve the Python browser's conceptual architecture while expressing C rules
explicitly. Every important subsystem needs:

- a clear public interface and error contract;
- explicit ownership and lifetime rules;
- a testable boundary;
- dependency direction that avoids circular coupling.

Prefer simple C abstractions and opaque structs at subsystem boundaries when
they improve encapsulation. Keep shared mutable global state exceptional and
documented. Fix the underlying boundary instead of accumulating compatibility
hacks.

## Ownership

Every allocation must have one identifiable owner and one deterministic
destruction path. Review strings, DOM nodes, CSS objects, layout nodes, display
lists, surfaces, images, fonts, JavaScript values, network buffers, callbacks,
and event objects whenever they cross a boundary.

For each changed resource, account for:

1. creator and initial owner;
2. borrow versus transfer at every call boundary;
3. validity duration and thread assumptions;
4. cleanup on success, partial construction, cancellation, and error;
5. destruction order relative to borrowed dependencies.

A boundary change is complete only when every affected owned resource has been
accounted for and its upstream/downstream integration is tested.

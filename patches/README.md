# Dependency patches

`deps/` is a local, untracked checkout. Fixes Tai needs in a dependency are
kept here as tracked patches so every build applies the same source.

`CMakeLists.txt` applies `patches/quickjs/*.patch` to `deps/quickjs` in name
order at configure time. A patch that is already present is skipped; a patch
that neither applies nor is present stops configuration. Re-run CMake after
replacing a checkout.

Each patch states the defect, the upstream revisions checked, and how it was
found. Keep a regression test that fails without the patch, and remove the
patch once the pinned upstream revision contains the fix.

| Patch | Defect | Regression test |
|---|---|---|
| `quickjs/0001-unlink-context-on-class-proto-oom.patch` | `JS_NewContextRaw()` frees a context still linked in the GC list when `class_proto` allocation fails; the next `JS_FreeRuntime()` reads freed memory | `quickjs_oom`; `tabset_loader_oom` covers the browser path |
| `quickjs/0002-check-push-scope-allocation.patch` | Callers ignore a failed `push_scope()`; the matching `pop_scope()` drives `scope_level` to -1 and `has_with_scope()` reads `scopes[65535]` | `quickjs_oom` (runtime.js compile sweep, one-failure mode); `tabset_loader_oom` |
| `quickjs/0003-skip-relocations-after-bytecode-oom.patch` | `resolve_labels()` writes jump offsets through `bc_out.buf` after growing it failed | `quickjs_oom` (runtime.js compile sweep); `tabset_loader_oom` |
| `quickjs/0004-check-child-function-constant-slot.patch` | A failed `cpool_add()` leaves `parent_cpool_idx = -1`; `js_create_function()` asserts | `quickjs_oom` (one-failure mode) |
| `quickjs/0005-fail-truncated-function-bytecode.patch` | `js_create_function()` resolves byte code whose buffer already failed (out-of-bounds read in `skip_dead_code`, label assertion) | `quickjs_oom` (one-failure mode) |

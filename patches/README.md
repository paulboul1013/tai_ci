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

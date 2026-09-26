---
name: native-stack
description: Approved native stack and toolchain rules (C17, CMake, Ninja, SDL3, Cairo, HarfBuzz, FriBidi, FreeType, QuickJS-NG, libcurl, OpenSSL, zlib, stb_image, libwebp, utf8proc, profiling) and dependency patch policy. Use when choosing or replacing a library, touching deps/ or patches/, or changing build flags. 觸發：C17 工具鏈／依賴選擇／函式庫替換／影像編解碼器／效能分析。
---

# Native Stack

## Platform

- Language: C17; no project-owned C++.
- Build: CMake and Ninja.
- Primary environment: Linux and WSL2.
- Prefer portable standard C where practical.

## Approved technologies

| Concern | Technology |
|---|---|
| Window and input | SDL3 |
| 2D rasterization | Cairo |
| Text shaping | HarfBuzz |
| Bidirectional text | FriBidi |
| Font rasterization | FreeType |
| JavaScript | QuickJS-NG |
| HTTP | libcurl multi |
| TLS | OpenSSL through libcurl |
| Compression | zlib |
| PNG/general image loading | stb_image where appropriate |
| WebP | libwebp |
| Unicode utilities | utf8proc |
| Profiling | Perfetto-compatible tracing and/or Tracy |
| Tests | CTest and cmocka |

Use the approved stack unless a demonstrated technical constraint requires a
replacement. A replacement decision must document why the existing dependency
fails, alternatives evaluated, architectural impact, and migration impact.

Fix a defect inside a dependency with a tracked patch under `patches/<dependency>/`
and a regression test, following [`patches/README.md`](../../../patches/README.md);
do not rely on edits to the untracked `deps/` checkout.

New C code should compile under strict warnings. Treat warnings as defects.
Measure non-trivial performance optimizations and preserve correctness.

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

New C code should compile under strict warnings. Treat warnings as defects.
Measure non-trivial performance optimizations and preserve correctness.

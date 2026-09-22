# C17 移植狀態

## 使用方式

這份文件是移植工作的導航，不是行為契約或歷史日誌。先從「目前切片」完成一個可驗證的垂直路徑，再依下表選擇下一個 subsystem。每次變更只更新受影響列的 state、evidence 與 gap。

`COMPLETE` 僅在功能、build、相關測試、Python oracle、ownership review、差異記錄與上下游整合皆有證據時使用；其餘進行中的可執行切片維持 `VALIDATING`。完整定義見 [`docs/agents/project-records.md`](docs/agents/project-records.md)。

## 目前切片：window page scroll

**State:** `VALIDATING`

`tai-browser --window URL` 的 wheel、PageUp/PageDown 與 ↑/↓ 已接到既有 `TaiPage` scroll seam。presentation 僅在 clamped scroll 改變後重繪；SDL 資源仍由 caller thread 的 presentation 擁有。

**刻意差異：** frozen Python 對非 flipped 的未知 wheel direction 仍當 normal，且 `int()` 遇非有限 y 會拋錯；native 將這兩種無效 SDL 輸入視為 no-op，以免無效事件改變頁面或中止視窗。有效 normal／flipped 輸入維持相同方向；後續完整輸入路由須沿用此驗證邊界。

**已驗證：** frozen Python oracle 確認 100px step 與 wheel direction；新 dummy SDL regression 先 RED 後 GREEN，審查後加強非零結尾與無效事件覆蓋；Debug CTest 24/24（加強測試另以 focused Debug 通過）；ASan/UBSan CTest 排除 sandbox 無法 bind 的 `network_differential` 後 23/23 通過（`detect_leaks=0`）。event、window ID、texture 與 cleanup 路徑已審查。

**下一個 seam：** DOM click/form input；tabs、history、Chrome 與完整 browser input ordering 仍未完成。詳細範圍見 [`docs/handoff/2026-09-20-window-interactive-scroll.md`](docs/handoff/2026-09-20-window-interactive-scroll.md)。

## Subsystem dashboard

| Subsystem | Python → C destination | Dependencies | State | Evidence | Gap / next seam |
|---|---|---|---|---|---|
| Core / ownership | builtins → `src/core.c` | C17 | VALIDATING | map unit tests；shared string/file/JSON primitives | allocation-failure 與 destruction paths；C error returns are intentional |
| DOM / HTML | `browser.py` parser/nodes → `src/dom.c` | core, Unicode | VALIDATING | parser/mutation unit；normalized DOM differential | JS DOM breadth、mutation differential、detached lifetime |
| CSS / style | parser/selectors/style → `src/css.c` | DOM, core | VALIDATING | parser/cascade unit；CSS differential | supported subset and compatibility rules → [`compatibility semantics`](docs/architecture/compatibility-semantics.md) |
| URL / HTTP | URL/cookie/referrer → `src/url.c`, `src/network.c` | core, libcurl multi | VALIDATING | URL differential；local HTTP integration | CORS/XHR/fetch、header/TLS/error limits、browser-level cancellation |
| Fonts / layout | document/block/line/text → `src/layout.c` | DOM, CSS, FreeType, fontconfig, utf8proc | VALIDATING | geometry/layout differentials；overflow clamp tests | HarfBuzz/FriBidi, controls, complete shaping/BiDi |
| Paint / raster | display commands/raster → `src/render.c` | Cairo, layout | VALIDATING | structural differentials；scroll/blur/blend/image key-region tests；viewport PNG | general/remote images and WebP; exact scope → [`render contract`](docs/reference-render-contract.md) |
| JavaScript / events | JS runtime/context → `src/js.c` | QuickJS-NG, DOM, CSS, network | VALIDATING | bridge, cancellation, exception tests | bubbling, broad DOM mutation, timers/fetch, browser integration |
| Scheduling | task runners/clocks → `src/scheduler.c` | threads, network | VALIDATING | priority/FIFO/aging/generation unit tests | browser/network/frame integration and close protocol |
| Browser / window | app/window/tab/chrome → `src/browser.c`, `src/presentation.c`, `src/main.c` | page, Cairo, SDL3 | VALIDATING | headless/PNG E2E; resize/layout differential; dummy SDL resize/scroll | current scroll validation; then DOM input, history, forms, tabs, Chrome, resource ordering |

## Disclosure map

Read the indicated source only when its branch is active:

| Trigger | Authoritative reference |
|---|---|
| Observable Python/C behavior or intentional divergence | [`docs/agents/oracle-and-porting.md`](docs/agents/oracle-and-porting.md) |
| Page, SDL, thread, texture, display-list ownership | [`docs/architecture/native-runtime.md`](docs/architecture/native-runtime.md) |
| Rendering coordinates, supported paint, screenshot/window behavior | [`docs/reference-render-contract.md`](docs/reference-render-contract.md) |
| CSS compatibility difference | [`docs/architecture/compatibility-semantics.md`](docs/architecture/compatibility-semantics.md) |
| Whole-project completion claim | [`ACCEPTANCE.md`](ACCEPTANCE.md) |
| Historical slice context or a resumed incomplete slice | [`docs/handoff/`](docs/handoff/) |

## Evidence constraints

- The Python browser is the behavioral authority; compare at the affected boundary before declaring equivalence.
- `ASAN_OPTIONS=detect_leaks=0` supports ASan/UBSan claims only. Fontconfig/SDL system-library allocations leave LeakSanitizer inconclusive.
- The sandbox may block the localhost network fixture. Record an externally rerun loopback result separately; never call that sandbox failure a product regression.
- Keep `ARCHITECTURE.md` as the directory index and put architecture, rendering, and acceptance details in their disclosed references rather than duplicating them here.

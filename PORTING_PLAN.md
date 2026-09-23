# C17 移植狀態

## 使用方式

這份文件是移植工作的導航，不是行為契約或歷史日誌。先從「目前切片」完成一個可驗證的垂直路徑，再依下表選擇下一個 subsystem。每次變更只更新受影響列的 state、evidence 與 gap。

`COMPLETE` 僅在功能、build、相關測試、Python oracle、ownership review、差異記錄與上下游整合皆有證據時使用；其餘進行中的可執行切片維持 `VALIDATING`。完整定義見 [`docs/agents/project-records.md`](docs/agents/project-records.md)。

## 目前切片：window 互動輸入與單頁導覽

**State:** `VALIDATING`

`tai-browser --window URL` 已接上當前視窗的 click、SDL text input、Backspace/左右/Return 特殊鍵、文字與 password 控制項、checkbox、button/Enter 表單送出，以及成功後在同一視窗替換文件。PageUp/PageDown、↑/↓ 與 wheel 維持既有 page scroll 路徑；presentation 依目前 scroll state 繪製右緣藍色 thumb。SDL text input 只在視窗聚焦且文字控制項有效時啟動，focus loss 與清理時停止。

`TaiPage` 建立擁有 URL、method/body 的 navigation intent；外層 window/session owner 以目前頁 URL 作 referrer、沿用 viewport 載入候選頁。候選頁成功後才替換並銷毀舊頁，失敗時保留舊頁。相同文件的 fragment 連結更新 URL/scroll；跨文件 fragment 在新文件 layout 建立後捲到目標。Presentation 仍只擁有 caller thread 的 SDL 資源，network 與 page slot 留在外層 owner。

**刻意差異：** frozen Python 對非 flipped 的未知 wheel direction 仍當 normal，且 `int()` 遇非有限 y 會拋錯；native 將這兩種無效 SDL 輸入視為 no-op，以免無效事件改變頁面或中止視窗。有效 normal／flipped 輸入維持相同方向；後續完整輸入路由須沿用此驗證邊界。

**已驗證：** Python oracle probes 核對文字插入/左右/Backspace、`quote_plus`、GET query 分隔與 fragment/query 的既有差異。Dummy SDL 覆蓋目前/其他視窗、focus、Unicode、特殊鍵、preventDefault、button/Enter 導覽、替換後的新頁 checkbox 輸入，以及候選載入失敗後同一事件迴圈仍處理後續 click。Local HTTP fixture 驗證 GET path/query、POST body/headers、Referer、跨文件 fragment scroll、preventDefault 不發請求，以及失敗載入保留原 page slot。完整 CTest 25/25 與最終 focused CTest 5/5 通過；ASan/UBSan focused `presentation_dummy`、`browser_headless`、`browser_navigation` 3/3 通過，均設 `ASAN_OPTIONS=detect_leaks=0`，因此不構成 LeakSanitizer 結論。此前 scrollbar slice 的原生視窗截圖證據仍適用於該 overlay。

**範圍差異與下一個 seam：** history/back-forward、tabs、browser chrome、mailto 外部程式啟動、一般網站相容性，以及原生視窗鍵盤自動操作仍未完成；鍵盤/text input 僅有 dummy SDL 證據。`tai_present_window` 舊介面沒有 navigation callback，需要 link/form 導覽的呼叫端應使用 `tai_present_window_with_navigation`。詳細通用缺口見 subsystem dashboard；已刪除的舊 handoff 不再作為連結目標。

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
| Browser / window | app/window/tab/chrome → `src/browser.c`, `src/presentation.c`, `src/main.c` | page, Cairo, SDL3 | VALIDATING | headless/PNG E2E; resize/layout differential; dummy SDL scroll/click/text/key/focus/navigation; local HTTP GET/POST/referrer/failure/fragment fixture; native scrollbar screenshots | history/back-forward, tabs, Chrome, external launch, real-window keyboard validation, broader input ordering |

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

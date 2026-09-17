# C17 移植狀態

Python source 已定位於 tai_gar 並固定工作樹快照；目前尚未達成整體驗收。
tai_ci 原有 main.c/Makefile 刪除狀態保持不變。

| Subsystem | Python source | C destination | Dependencies | State | Behavioral tests | Known discrepancies |
|---|---|---|---|---|---|---|
| Core / ownership | Python builtins | src/core.c | C17 | VALIDATING | map unit tests；string/file/JSON primitives 已供各 subsystem 使用 | allocation failure、string/file/JSON 與 ownership destruction path 的獨立測試仍不足；C API 以明確 error return 取代 Python exceptions |
| DOM / HTML | browser.py Text/Element/HTMLParser/ViewSourceParser | src/dom.c | core / Unicode | VALIDATING | parser/mutation unit tests；normalized DOM differential | 廣泛 JS DOM API、mutation differential 與 detached-node lifetime 驗證仍未完成 |
| CSS / style | CSSParser/selectors/style | src/css.c | DOM/core | VALIDATING | parser/selector/cascade/style unit 與 differential tests | 僅支援目前 property/selector subset；完整 CSS/CSSOM 與 rendering effects 尚未完成；非標準語意見 `docs/architecture/compatibility-semantics.md` |
| URL / HTTP | URL/cookie/referrer helpers | src/url.c / src/network.c | core/libcurl multi | VALIDATING | URL differential；local HTTP、cookie/cache/redirect/referrer/cancellation integration tests | TLS/compression capability 由目前 libcurl build 提供，並非直接 CMake OpenSSL/zlib integration；CORS response validation、XHR/fetch、browser-level Referrer-Policy、header limits、TLS/error paths 與 in-flight cancellation 尚未完整驗證；cookie/redirect 不委由 curl 自動決策 |
| Fonts / layout | Document/Block/Line/Text/controls | src/layout.c | DOM/CSS/FreeType/fontconfig/utf8proc | VALIDATING | layout CLI smoke test；geometry-tree differential 與 browser DOM/layout differential；固定高度 overflow content/scroll clamp unit integration | HarfBuzz/FriBidi、controls、互動 scroll、完整 shaping/BiDi 尚未完成；現行 RTL 語意見 compatibility contract |
| Paint / raster | Draw*/Blend/Blur/Scroll/Raster* | src/render.c / include/tai/render.h | Cairo/layout | VALIDATING | Python/C DrawRect/DrawText/DrawHitTest structural differential；Scroll push/pop structural differential；paint-order、半開邊界、clip、非零與巢狀 scroll hit differential；巢狀 scroll Cairo/hit key-region cross-check；self-contained node-ID ownership/unit tests；PNG CLI integration | 尚缺 rounded `overflow: clip`/shape hit、opacity/blend、blur、image、viewport/page scroll 與 SDL presentation；完整 scope 與座標契約見 `docs/reference-render-contract.md` |
| JS / events | JSContext/runtime.js | src/js.c | QuickJS-NG/DOM/CSS/network | VALIDATING | attribute/query bridge、單節點 event cancellation 與 exception handling tests | event bubbling 與 execution-limit test coverage 尚未完成；bridge/limit mechanism 已存在，但廣泛 DOM mutation APIs、`innerHTML`/`outerHTML`、cookie、XHR/fetch、RAF/timers、完整 differential 與 scheduler/browser integration 尚未完成 |
| Scheduling | TaskRunner/NetworkTaskRunner/frame clocks | src/scheduler.c | threads/network | VALIDATING | priority/FIFO/aging、frame guard、generation cancellation unit tests | 目前仍為獨立 scheduler unit；缺 Browser/Network/frame-clock integration、concurrent lifecycle/close protocol 與 stale browser snapshot 驗證 |
| Browser / window | BrowserApp/BrowserWindow/Tab/Chrome | src/browser.c / src/main.c | 已接入 subsystem；目標另需 SDL3 | VALIDATING | synchronous headless navigation、browser DOM/layout differential、inline script ordering、單一 external script smoke、page document-coordinate hit adapter 與 `--screenshot` PNG E2E | CLI 仍為 headless-only；完整 mixed inline/external resource-order differential、SDL3 window/input/event dispatch、viewport scroll、history、forms 與 tabs 尚未完成 |

## 目標執行順序與驗證關卡

1. 固定 oracle、完成 dependency 分析及 C ownership contracts。
2. 先跑 Python cases，建立 DOM/CSS probe 的失敗測試；實作 core、DOM、CSS，build、differential、sanitizer。
3. URL/HTTP 與 layout/raster 可在共用 contracts 確定後平行實作；各自使用本機 deterministic fixtures。
4. 整合 native headless navigation→DOM→style→layout→display list→PNG，驗證 rendering。
5. 補完 JS、event 與 network policy，並整合 scheduler/SDL3/window orchestration。
6. 獨立 verification agent 對行為、ownership、stale work、resource destruction 與 E2E 主動找錯；修正後更新此表。

每項只有 functionality、build、tests、oracle、ownership review、差異文件、上下游 integration 全通過才標 COMPLETE。
Dependencies 固定 revision；不以可編譯或少數 smoke cases 代替完整驗收。

## 目前完成 checkpoint

目前 `tai-browser` 已串接 synchronous headless navigation→DOM→style→layout→display list→Cairo PNG；CLI integration 已覆蓋成功與失敗路徑。精確的 output contract 與測試證據見 `docs/reference-render-contract.md`。

Default stylesheet 不再編譯成 source-tree absolute path。執行時先依序嘗試 executable-relative install-time `../share/tai-ci/browser.css` 與 build-tree `../assets/browser.css`；若兩者均無法讀取（也涵蓋無法建立 executable-relative path），最後回退至 current working directory 的 `assets/browser.css`。CMake install 會將資源放至 `${prefix}/share/tai-ci/browser.css`。

Display-list ownership 與 runtime boundary 見 `docs/architecture/native-runtime.md`；layout、font、display 與 screenshot 的可觀察契約見 `docs/reference-render-contract.md`。

Stage 4 的目前 slice 已建立 Python/C display 結構 gate：支援的 `DrawRect`/`DrawText` leaves
在固定案例中比對順序、文字、RGBA 與 geometry；一個固定高度 `overflow: scroll` subtree 以
document-space clip、先 clip 後 translate 的 push/pop 命令表示。Debug CTest 18/18 通過；
ASan/UBSan（`detect_leaks=0`）同一套測試通過，其中 localhost network fixture 因 sandbox
socket policy 在允許 loopback 的環境重跑。這不是 LeakSanitizer 證據，也不改變 Paint/raster
與 Browser/window 的 `VALIDATING` 狀態。

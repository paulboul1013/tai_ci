# C17 移植狀態

Python source 已定位於 tai_gar 並固定工作樹快照；目前尚未達成整體驗收。
tai_ci 原有 main.c/Makefile 刪除狀態保持不變。

| Subsystem | Python source | C destination | Dependencies | State | Behavioral tests | Known discrepancies |
|---|---|---|---|---|---|---|
| Core / ownership | Python builtins | src/core.c | C17 | VALIDATING | map unit tests；string/file/JSON primitives 已供各 subsystem 使用 | allocation failure、string/file/JSON 與 ownership destruction path 的獨立測試仍不足；C API 以明確 error return 取代 Python exceptions |
| DOM / HTML | browser.py Text/Element/HTMLParser/ViewSourceParser | src/dom.c | core / Unicode | VALIDATING | parser/mutation unit tests；normalized DOM differential | 廣泛 JS DOM API、mutation differential 與 detached-node lifetime 驗證仍未完成 |
| CSS / style | CSSParser/selectors/style | src/css.c | DOM/core | VALIDATING | parser/selector/cascade/style unit 與 differential tests | 僅支援目前 property/selector subset；完整 CSS/CSSOM 與 rendering effects 尚未完成，已知非標準 comment/`!important` 語意須維持文件化 |
| URL / HTTP | URL/cookie/referrer helpers | src/url.c / src/network.c | core/libcurl multi | VALIDATING | URL differential；local HTTP、cookie/cache/redirect/referrer/cancellation integration tests | TLS/compression capability 由目前 libcurl build 提供，並非直接 CMake OpenSSL/zlib integration；CORS response validation、XHR/fetch、browser-level Referrer-Policy、header limits、TLS/error paths 與 in-flight cancellation 尚未完整驗證；cookie/redirect 不委由 curl 自動決策 |
| Fonts / layout | Document/Block/Line/Text/controls | src/layout.c | DOM/CSS/FreeType/fontconfig/utf8proc | VALIDATING | layout CLI smoke test；geometry-tree differential 與 browser DOM/layout differential | HarfBuzz/FriBidi 尚未接入，`--rtl` 僅為簡化右對齊；controls、content height/scroll、完整 shaping/BiDi 與 text/display differential 尚未完成 |
| Paint / raster | Draw*/Blend/Blur/Scroll/Raster* | src/render.c / include/tai/render.h | Cairo/layout | VALIDATING | display-list command ownership/unit tests；Cairo PNG component 與 CLI 尺寸/anchor-pixel integration | 目前只有 block fill/text slice；Python oracle 僅以既有 DOM/layout structural differential 與穩定 key regions 間接驗證，尚無完整 display differential、effects、image、clip、scroll、hit testing 或 SDL presentation integration |
| JS / events | JSContext/runtime.js | src/js.c | QuickJS-NG/DOM/CSS/network | VALIDATING | attribute/query bridge、單節點 event cancellation 與 exception handling tests | event bubbling 與 execution-limit test coverage 尚未完成；bridge/limit mechanism 已存在，但廣泛 DOM mutation APIs、`innerHTML`/`outerHTML`、cookie、XHR/fetch、RAF/timers、完整 differential 與 scheduler/browser integration 尚未完成 |
| Scheduling | TaskRunner/NetworkTaskRunner/frame clocks | src/scheduler.c | threads/network | VALIDATING | priority/FIFO/aging、frame guard、generation cancellation unit tests | 目前仍為獨立 scheduler unit；缺 Browser/Network/frame-clock integration、concurrent lifecycle/close protocol 與 stale browser snapshot 驗證 |
| Browser / window | BrowserApp/BrowserWindow/Tab/Chrome | src/browser.c / src/main.c | 已接入 subsystem；目標另需 SDL3 | VALIDATING | synchronous headless navigation、browser DOM/layout differential、inline script ordering、單一 external script smoke 與 `--screenshot` PNG E2E | CLI 仍為 headless-only；完整 mixed inline/external resource-order differential、SDL3 window/input、history、forms、hit testing、scroll 與 tabs 尚未完成 |

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

目前 `tai-browser` 已串接 synchronous headless navigation→DOM→style→layout→display list。預設 CLI 輸出仍是 DOM、layout 與 display JSON；`--screenshot OUTPUT.png` 則直接將同一個 `TaiPage` display list 交給 Cairo，產生寬 800px、白色不透明背景的 PNG，不輸出 JSON。高度遵循 reference 的 document-height contract：`max(1, ceil(layout_height + 2 * VSTEP))`，其中 `VSTEP=18`。CLI integration 驗證成功輸出、尺寸、穩定色塊/背景 anchor pixels、無法寫檔與缺少參數的失敗路徑。

Default stylesheet 不再編譯成 source-tree absolute path。執行時先依序嘗試 executable-relative install-time `../share/tai-ci/browser.css` 與 build-tree `../assets/browser.css`；若兩者均無法讀取（也涵蓋無法建立 executable-relative path），最後回退至 current working directory 的 `assets/browser.css`。CMake install 會將資源放至 `${prefix}/share/tai-ci/browser.css`。

## 目前 component slice：layout → display list → Cairo PNG

`tai_layout_visit` 以同步、borrowed callback 暴露 layout 幾何；callback 返回前不得保留 node、word 或 layout item。`TaiDisplayList` 會複製文字與字型名稱，因此建立完成後不再依賴 DOM、CSS 或 layout，可先銷毀來源再交給 Cairo raster。現階段只產生 block 的 `fill_rect` 與文字 `text` command，PNG surface 以 opaque white 起始；這是可執行的 paint vertical slice，不代表 Python `paint_tree` 的完整 effects contract 已完成。

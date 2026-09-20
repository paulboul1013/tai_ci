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
| Paint / raster | Draw*/Blend/Blur/Scroll/Raster* | src/render.c / include/tai/render.h | Cairo/layout | VALIDATING | Python/C DrawRect/DrawText/DrawHitTest/OpenMoji structural differential；Scroll、Blur 與 opacity/blend push/pop structural differential；paint-order、clip、scroll、image/blur key-region tests；viewport PNG CLI；owned ARGB32 memory-raster pixel test | OpenMoji 單碼點本地 PNG path 已支援；仍缺一般 `<img>`/remote image/WebP；完整 scope 與座標契約見 `docs/reference-render-contract.md` |
| JS / events | JSContext/runtime.js | src/js.c | QuickJS-NG/DOM/CSS/network | VALIDATING | attribute/query bridge、單節點 event cancellation 與 exception handling tests | event bubbling 與 execution-limit test coverage 尚未完成；bridge/limit mechanism 已存在，但廣泛 DOM mutation APIs、`innerHTML`/`outerHTML`、cookie、XHR/fetch、RAF/timers、完整 differential 與 scheduler/browser integration 尚未完成 |
| Scheduling | TaskRunner/NetworkTaskRunner/frame clocks | src/scheduler.c | threads/network | VALIDATING | priority/FIFO/aging、frame guard、generation cancellation unit tests | 目前仍為獨立 scheduler unit；缺 Browser/Network/frame-clock integration、concurrent lifecycle/close protocol 與 stale browser snapshot 驗證 |
| Browser / window | BrowserApp/BrowserWindow/Tab/Chrome | src/browser.c / src/presentation.c / src/main.c | page/display list、Cairo、SDL3 | VALIDATING | headless navigation/differential、viewport PNG E2E；width-sensitive Python/C layout differential；`--window` SDL dummy-driver resize/quit smoke；ARGB32/ARGB8888 mask check | input dispatch、interactive scroll、history、forms、tabs、Chrome 與完整 resource ordering 尚未完成 |

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

Default stylesheet 不再編譯成 source-tree absolute path。執行時先依序嘗試 executable-relative install-time `../share/tai-ci/browser.css`、build-tree `assets/browser.css` 與 `../assets/browser.css`；若均無法讀取（也涵蓋無法建立 executable-relative path），最後回退至 current working directory 的 `assets/browser.css`。CMake 會複製資源到 build-tree `assets/`，安裝時放至 `${prefix}/share/tai-ci/browser.css`。

Display-list ownership 與 runtime boundary 見 `docs/architecture/native-runtime.md`；layout、font、display 與 screenshot 的可觀察契約見 `docs/reference-render-contract.md`。

Stage 4 的目前 slice 已建立 Python/C display 結構 gate：支援的 `DrawRect`/`DrawText` leaves
在固定案例中比對順序、文字、RGBA 與 geometry；固定高度 `overflow: scroll` subtree 以
document-space rectangular clip、先 clip 後 translate 的 push/pop 命令表示；`overflow: clip`
則以 isolated rounded destination-in mask 合成，並保留 Python 的 raster-only hit 語意。
rounded fill differential 另比較整數、fractional、huge radius 的 DrawRRect/DrawRect 分流。Debug CTest 20/20 通過；
ASan/UBSan（`detect_leaks=0`）同一套測試通過，其中 localhost network fixture 因 sandbox
socket policy 在允許 loopback 的環境重跑。這不是 LeakSanitizer 證據，也不改變 Paint/raster
與 Browser/window 的 `VALIDATING` 狀態。

後續 page-scroll slice 將 viewport dimensions 與 clamped `scroll_y` 收進 `TaiPage`，保留 raw
display-list document-coordinate contract；viewport hit 與 Cairo raster 共用一次
viewport→document conversion。Python/C differential 覆蓋 zero/non-zero/clamp/boundary，另以雙層
element scroll fixture 驗證 page scroll 疊加後 raster/hit 一致。headless screenshot 現為
frozen Python rendered Chrome geometry 推導的 800×532 page viewport，且 differential 直接
比較 PNG IHDR 高度；在該 checkpoint，SDL presentation 與 input/event dispatch 尚未開始。

後續 opacity/mix-blend-mode slice 以成對 `push_blend`/`pop_blend` 保留 subtree nesting，
解析 numeric/percentage opacity、invalid fallback 與 `[0,1]` clamp，並以同一 Cairo group
一次套用 subtree alpha。multiply、difference、destination-in 與 source-over fallback 已由
oracle structural differential 和 key-pixel tests 覆蓋；rounded clip 與 element scroll 位於
外層 compositing group 內，ordinary Blend 與 opacity zero 不改 point-hit traversal。獨立審查
發現的 hexadecimal opacity parser 差異已以 oracle regression 修正。Paint/raster 維持
`VALIDATING`；在該 checkpoint，image、SDL3 presentation 與完整 paint scope 仍未完成。

後續 blur slice 以成對 `push_blur`/`pop_blur` 保留完整 subtree isolation，並置於 rounded
overflow clip 之內及 opacity/blend 之外；Cairo raster 以 3σ separable Gaussian 處理
premultiplied ARGB32 group。parser/display/hit differential、oracle edge probe 與 key-region tests
覆蓋 no-op elision、subtree-once、sibling isolation、rounded clip 與 compositing nesting。
Frozen parser 對 `blur(infpx)` 回傳 infinity、但 strict JSON serializer 失敗；native 刻意將
non-finite sigma 視為 none，以維持有限 allocation 與合法 JSON。Paint/raster 仍為
`VALIDATING`；這項 slice 不代表 image 或 SDL presentation 完成。
Exact Gaussian raster 另設 25,000,000 channel-tap work budget；超限會明確失敗而非以不同
濾鏡近似，避免 untrusted CSS 壟斷 raster thread。這項 intentional resource limit 由大 sigma
regression 覆蓋。
完成後 Debug CTest 21/21 通過；ASan/UBSan（`detect_leaks=0`）21 項亦通過，其中 localhost
network fixture 依既有 sandbox 限制在 sandbox 外重跑。這不是 LeakSanitizer 證據。獨立
adversarial re-review 的 correctness/ownership findings 均已解決；opaque display list 使
malformed effect stream 無 public 注入 seam，相關 defensive branches 尚無直接 focused test。

OpenMoji cache 的 intentional difference：frozen Python module-global 成功 cache 可跨 layout 存活；
native cache 由每個 `TaiLayout` 擁有並確定清除。因此只有 process 存活期間外部修改／刪除 asset 後
另建 layout 的情境不同。Native 並在 Cairo decode 前限制 PNG 為 64 MiB、單邊 16384、總像素
25,000,000；超限 fallback 為文字，以避免本地不受信任 asset 的無界 decoder allocation。
OpenMoji slice 完成後 Debug CTest 22/22 通過；最終 image/render focused ASan/UBSan 在
`ASAN_OPTIONS=detect_leaks=0` 下通過，且先前完整 sanitizer 22 項僅 localhost fixture 受 sandbox
阻擋，該項已於 sandbox 外單獨通過。這不是 LeakSanitizer 證據。

SDL presentation slice 新增 opt-in `--window`，從既有 page display list 經 Cairo
ARGB32 memory raster 上傳至 SDL3 texture；同步處理 resize、expose 與 quit。SDL dummy-driver
smoke 驗證開窗／呈現／quit 及超限初始化後的清理重試；render test 驗證像素 copy 與無效尺寸。
Python resize 會重新 layout，而此 slice 僅以原 800px display list 在新視窗大小重繪；
這是刻意保留至後續互動 browser orchestration 的差異，影響換行與流動排版。
Debug CTest 23/23 通過；ASan/UBSan CTest 在 `ASAN_OPTIONS=detect_leaks=0` 下 23/23 通過。
未關閉 LeakSanitizer 的首次執行於 Fontconfig/SDL 系統函式庫配置回報 leaks，
因此這不是 LeakSanitizer 通過證據。Browser/window 與 Paint/raster 仍為 `VALIDATING`。

後續 window-resize relayout slice 將 `TaiPage` viewport、RTL state、layout 與 immutable
display list 收斂在 `tai_page_resize`。它先 snapshot DOM element scroll values，再建立
replacement layout/display；只有兩者皆成功才交換、更新 dimensions、夾住 page scroll，並依
display→layout 順序釋放舊資源。任何配置、layout 或 display 失敗會清除 replacement 並還原
snapshot，保留舊 page fields。SDL pixel-size event 先呼叫這個 seam，成功後才借用 current
display raster；初始 physical pixel size 也走同一 seam，超限 resize 在 reflow 前拒絕，
raster/upload/present 失敗不銷毀舊 texture。`test_browser` 覆蓋換行、scroll clamp 和 invalid
no-op，`test_presentation` 在 owner thread 注入 resize/quit 與超限 resize，layout differential
新增 80px 寬換行 oracle case。Browser/window 維持 `VALIDATING`，因 Chrome、tabs/input 與完整
browser event ordering 尚未移植。

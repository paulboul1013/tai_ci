# Browser C17 架構

## 來源與分析邊界

Python oracle 是 `/home/paulboul/tai_gar` 的目前工作樹，不是 tai_ci 的舊 C CLI。
`tests/reference/manifest.json` 記錄來源 commit 與實際檔案 SHA-256；工作樹的 browser.py 有未提交修改，故 commit 本身不足以識別規格。
入口為 browser.py（9,862 行）、runtime.js、browser.css；web_server.py 提供測試網站。
server.py 是另一份較舊 Browser，不能取代目前入口。test.md 是人工測試清單，並非自動測試套件。

## 實際 Python dependency graph

```mermaid
graph TD
  App[BrowserApp / SDL event loop] --> Window[BrowserWindow / Chrome]
  App --> Network[NetworkTaskRunner]
  App --> Raster[RasterAndDrawRunner]
  Window --> Tab[Tab / TaskRunner]
  Tab --> URL[URL / HTTP / cookies / cache]
  Network --> URL
  Tab --> DOM[HTMLParser / Element / Text]
  Tab --> CSS[CSSParser / selectors / style]
  CSS --> DOM
  Tab --> JS[JSContext / runtime.js / dukpy]
  JS --> DOM
  JS --> CSS
  JS --> URL
  Tab --> Layout[Document / Block / Line / Text / controls]
  Layout --> DOM
  Layout --> Font[Skia fonts / local emoji]
  Layout --> Display[display commands / visual effects / hit testing]
  Display --> Raster
  Tab --> Commit[CommitData snapshots]
  Commit --> Window
  Raster --> Window
```

## 狀態、資料與事件

BrowserApp 共享 visited URLs、bookmarks、network/raster runners 與 windows。每個 Window 擁有 tabs、active tab、Chrome、frame clock、committed snapshots 與 scene epoch。
每個 Tab 擁有 URL/history、navigation generation、DOM、CSS rules、JS context、layout、display list、focus、document/element scroll 與 frame estimator。

Navigation 先增加 generation，網路結果排回 Tab task queue；過期 generation 不可改寫頁面。
HTML parse 後按 DOM 順序收集 external scripts、stylesheets、inline styles；可並行取得資源，但按原始順序處理。
Inline 與 external script 依 DOM traversal source order 收集，載入後以同一 JS context 依序執行；
runtime.js 未提供 Python 所有 timer hooks，實際 bridge 必須逐項驗證。
Style → layout → paint tree → immutable commit → raster → window present。點擊使用 paint order 與 clip/scroll 座標 hit test，再執行 JS event 與預設 navigation/form/focus 行為。

## Thread model

SDL browser thread 負責 native window/input/presentation；每個 Tab 有序列化 Main Thread。
Networking Thread 派發 I/O workers，完成後僅傳送結果，不直接修改 DOM/JS。
CPU raster thread 擁有 raster surfaces；GPU 路徑因 GL context 綁定而由 browser thread raster。
Window lock 保護跨執行緒狀態，generation/scene epoch 拒收 stale results；frame scheduling 使用 monotonic deadlines。

## C 邊界與 ownership 決策

依序建立 core、DOM、CSS、transport、layout/display/raster、JS、scheduler、browser/window。
Document 擁有所有 nodes，node parent 與 layout/JS 對 node 的引用皆 borrowed。Document 銷毀前必須先結束 JS/layout 與 callback 使用；DOM mutation 不可讓 JS handles 懸空。
CSS stylesheet 擁有 selectors/declarations；computed style 擁有字串副本。
Network response 擁有 headers/body；跨 queue 移交時移轉 ownership。取消仍須銷毀 payload。
Display snapshot 必須自足且不可在 raster 執行時修改；不得讓 worker 讀取可變 DOM。
Opaque subsystem handles 隔離內部狀態；內部 DOM model 用共用 header 明訂欄位，以免 subsystem 自行發明結構。

目前 native paint slice 將 layout 與 raster 分成兩個邊界：`tai_layout_visit` 只在 owner thread
同步提供 borrowed 幾何；`TaiDisplayList` 複製每個 command 的文字、字型名稱、顏色與幾何，
不保留 DOM/layout pointer。`tai_display_list_write_png` 只讀 immutable list，Cairo surface 與
context 均在呼叫內建立並確定性銷毀。這使後續 raster worker 可以接收 list ownership，而不會
因 layout/DOM 先釋放而產生 UAF；目前尚未加入 Python 的 effect tree、clip/scroll isolation
或 SDL present。

Headless screenshot 是目前第一條完整 raster integration：`tai-browser --screenshot PATH URL`
載入 `TaiPage` 後借用其 immutable display list，同步寫出 800px 寬 PNG；輸出高度為
`max(1, ceil(layout_height + 36))`，對應 reference 的 `document.height + 2 * VSTEP`。
surface 為 opaque white，寫檔完成或失敗後都由 raster API 銷毀 Cairo context/surface；
`TaiPage` 仍由 CLI 在共同 cleanup path 銷毀。未指定 screenshot 時維持原有 JSON contract。

Default CSS 是 install-time resource，不屬於 browser page ownership；`tai-browser` 依序從
executable-relative install path、build-tree fallback 或最後的 cwd fallback 讀取，避免把
checkout 絕對路徑寫入 binary。

## 必須保留的非標準語意

一般 HTML end-tag 僅 pop 一層；attributes 不解 entities；`<br/>` 是 br/ element。
CSS comments 並未正確支援，會影響後續 rule；同 declaration block 後項覆蓋先項，即使先項 important。
一般 img 無 layout；emoji 僅單字元對應本地 PNG。`--rtl` 是右對齊，並非完整 BiDi。
不能用標準 library 的預設行為默默覆寫這些規格。Memory corruption 不屬於可保留行為；C 以錯誤回傳處理無效輸入，差異記錄於 PORTING_PLAN.md。

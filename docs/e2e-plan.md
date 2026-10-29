# 完整管線 E2E：實作計畫

**狀態：已實作，`VALIDATING`（2026-10-29）。** 證據見
[`docs/acceptance/2026-10-29-e2e.md`](acceptance/2026-10-29-e2e.md)，其中也列出與本計畫的差異
（Back 拆成兩個檢查點、頁面寫在 `tests/e2e_fixture.py`、`#bottom` 位置、`viewport` 只記錄 Python 的值）。

本計畫處理 [ACCEPTANCE.md](../ACCEPTANCE.md) 的這個條件：

> Navigation→resource loading→script/style→layout→raster→presentation 的 native E2E 通過。

完成後，證據寫入 `docs/acceptance/<日期>-e2e.md`，刻意差異寫入
[PORTING_PLAN.md](../PORTING_PLAN.md)「已知差異與範圍」。

## 一句話摘要

用一個本機測試網站，讓 native 瀏覽器從輸入網址開始，跑完整條管線到視窗顯示。每一段的
結果都和凍結的 Python oracle 比對，最後在真實視窗中再跑一次。

## 現在缺少什麼

目前的測試都只測管線的一段，或只測一個功能：

```
            網址   網路    資源     script   layout   raster   視窗
            輸入   載入    載入     /style            (繪製)   顯示
            ────   ────    ────     ──────   ──────   ──────   ────
browser_    ✗      ✗       ✗        style    ✓        ✗        ✗     ← 5 個 inline 案例
differential
network_    ✗      ✓       ✗        ✗        ✗        ✗        ✗     ← 只測網路
integration
js_dom_     ✓      ✓       script   ✓        ✗        ✗        ✗     ← 只測 JS
integration
*_window    ✓      ✓       部分     部分     部分     ✗        dummy ← 每個測一個功能
.c
trace_      ✓      ✓       ✓        ✓        ✗        ✗        Xvfb  ← 只看 trace
window
            ────────────────────────────────────────────────────────
E2E（新）   ✓      ✓       ✓        ✓        ✓        ✓        ✓     ← 一次跑完整條
```

沒有一個測試同時證明：外部 CSS 和外部 JS 都經網路載入，JS 改了 DOM，改過的 DOM 正確排版，
display list 正確，最後畫到視窗上。

## 測試網站

新增 `tests/e2e_fixture.py`（127.0.0.1 伺服器）和 `tests/fixtures/e2e_site/`。伺服器記錄每個
請求（method、path、`Cookie` header、body），用來證明資源都有載入。

```
 GET /  ──302──▶  /index.html
                    │
                    ├── <link rel=stylesheet href=/style.css>   回應用 gzip
                    ├── <script src=/app.js>                     回應用 chunked
                    │      app.js：新增一個 <p>、改 <title>、設 document.cookie
                    ├── <a href=/form.html>                      連結導覽
                    └── <a href=#bottom>  + 長內容                fragment 與捲動
                    
 /form.html
   └── <form method=post action=/submit>
         text、password、checkbox、submit
 /submit (POST)
   └── 伺服器把收到的欄位寫進結果頁    ← 證明表單資料正確送出
```

Python oracle 支援 redirect、gzip 和 chunked（`browser.py` 第 8735–8879 行），所以兩邊可以比對。

## 劇本與檢查點

兩邊跑同一個劇本。每一步之後，等頁面提交一個 frame，再取一個檢查點。

```
 步驟                         檢查點內容
 ─────────────────────────    ─────────────────────────────────────────────
 1 開啟 http://127.0.0.1/     URL（兩邊都保留請求的 `/`）、標題（JS 改過的）、
                              DOM 文字、layout 樹、display list、請求紀錄
 2 點 #bottom 連結            URL 有 fragment、scroll 位置、沒有新請求
 3 點 /form.html 連結         URL、history（Back 可用）、Cookie header 有值
 4 輸入 text、password、      欄位的值、checkbox 狀態、display list
   勾 checkbox
 5 送出表單                   POST body、結果頁的 DOM 與 layout
 6 按 Back 兩次               回到 index、重新 GET、JS 再跑一次
```

比對的欄位和 tolerance 沿用既有測試：layout 用 `browser_differential.py` 的 `layout_shape`，
display list 用 `display_differential.py` 的結構比對。

## 三層驗證

```
 ┌───────────────────────────────────────────────────────────────────┐
 │ 第 0 層  Python oracle（凍結答案）                                │
 │   tests/e2e_oracle_probe.py：真的 BrowserApp + SDL dummy           │
 │   ── 跑劇本 ──▶ tests/fixtures/e2e_oracle.json                     │
 │   --check 模式：重跑並確認 JSON 沒變                                │
 └───────────────────────────────┬───────────────────────────────────┘
                                 │ 逐點比對
 ┌───────────────────────────────▼───────────────────────────────────┐
 │ 第 1 層  Native + SDL dummy（決定性，每次 CTest 都跑）             │
 │   tests/test_e2e_window.c：用 window_driver.h 跑真的 TaiBrowserApp │
 │   tests/e2e_integration.py：啟動伺服器，比對 native 與 oracle JSON  │
 └───────────────────────────────┬───────────────────────────────────┘
                                 │ 同一個劇本
 ┌───────────────────────────────▼───────────────────────────────────┐
 │ 第 2 層  Native + Xvfb 真實視窗（證明 presentation）                │
 │   tests/e2e_window_integration.py：用 window_session.sh 點擊、輸入 │
 │   檢查：截圖關鍵區域的顏色、請求紀錄、SDL 事件紀錄、正常結束        │
 └───────────────────────────────┬───────────────────────────────────┘
                                 │
 ┌───────────────────────────────▼───────────────────────────────────┐
 │ 第 3 層  build-asan：第 1、2 層用 ASan/UBSan/LSan 再跑一次          │
 └───────────────────────────────────────────────────────────────────┘
```

## 步驟

```
 步驟 0  使用者確認下面的決定
            ▼
 步驟 1  測試網站與伺服器（e2e_fixture.py、e2e_site/）
            ▼
 步驟 2  Python oracle probe，凍結 e2e_oracle.json
            ▼
 步驟 3  Native dummy SDL driver 與比對（第 1 層）── 找到差異就修或記錄
            ▼
 步驟 4  Xvfb 真實視窗（第 2 層）
            ▼
 步驟 5  Sanitizer（第 3 層）
            ▼
 步驟 6  CTest 註冊、acceptance 證據、勾選 ACCEPTANCE.md、更新 PORTING_PLAN.md
```

每一步都要能獨立驗證，才開始下一步。步驟 3 發現的 native 錯誤，先停下來和使用者討論，再決定
修正或記錄成刻意差異。

## 要確認的決定

| # | 決定 | 建議 | 理由 |
|---|---|---|---|
| E1 | 範圍 | 只做管線 E2E（ACCEPTANCE 第 8 項）。view-source、resize、tabs、多視窗、書籤留給第 9 項。 | 這些功能已經有各自的 integration 測試。一次全做，計畫太大，失敗時也不容易定位。 |
| E2 | Raster 怎麼驗證 | 比對 display list 結構；native 截圖只檢查關鍵區域的顏色。不做 Python 與 native 的逐像素比對。 | 延續 2026-10-14 的決定：Skia 和 Cairo 的像素本來就不會相同。 |
| E3 | Script | **已選 B（2026-10-29）**：第 0、1 層只用外部 `<script src>`；第 2 層多一頁 `/inline.html`，只檢查 native 的 inline script 結果。 | Python 不執行 inline script（D6），放進 oracle 比對會讓兩邊答案不同；真實視窗仍要測到 D6。 |
| E4 | 圖片 | 不放 `<img>`。 | 兩邊都還不排版 `<img>`，放了也沒有東西可比對。 |
| E5 | 字型 | 頁面只用預設 sans-serif。 | 避開「缺字型的替代字型」差異，讓 layout 可以精確比對。 |
| E6 | 第 1 層用 C driver | 新增 `test_e2e_window.c`，沿用 `window_driver.h`。 | 和既有 9 個 `*_window.c` 測試同一種寫法，決定性高。 |

## 優點與缺點

**改善了什麼**

- 第一次有一個測試證明整條管線接在一起可以運作。
- 測到各段之間的接縫：loader 執行緒把資源交給 SDL 執行緒、JS 改 DOM 後重建 layout、
  導覽後丟掉舊頁面。
- Sanitizer 會在真實的完整流程中檢查記憶體錯誤。

**優點**

- 全部重用既有工具：oracle probe 模式、`window_driver.h`、`window_session.sh`、fixture 伺服器。
- 第 1 層是決定性的，可以每次 CTest 都跑。

**缺點與風險**

- E2E 失敗時，比較難知道是哪一段壞掉。對策：檢查點分得很細，訊息寫出是哪一步、哪個欄位。
- Xvfb 測試比較慢，也比較不穩定。對策：只在第 2 層用 Xvfb，而且只檢查顏色區域，不比像素。
- 可能找到新的 native 差異，讓工作變多。對策：步驟 3 先停下來討論。
- 這個測試不會讓 ACCEPTANCE 的其他條件（第 9、10 項）自動完成。

## 設計：資料形狀與 seams（plumb Design 步驟）

### 檢查點的資料形狀

兩邊每個檢查點都產生同一種 JSON。這是 oracle 和 native 之間唯一的契約。

```
checkpoint = {
  "url":      str,                 # 已提交頁面的 URL（302 後兩邊都保留請求 URL）
  "title":    str,                 # Tab.get_title／tai_page_title
  "history":  {"back": bool, "forward": bool},
  "scroll":   float,               # 頁面捲動位置
  "dom":      dom_value(styled),   # oracle.py dom_value；native tai_dom_json(styled)
  "layout":   layout_shape(...),   # browser_differential.py 的 kind/x/y/width/height/word
  "display":  [leaf, ...],         # display_differential.py 的 fill_rect／text 葉節點
  "inputs":   {id: {"value": str, "checked": bool}},
  "requests": [{"method", "path", "cookie", "body"}, ...]   # 上個檢查點之後的新請求
}
```

### 模組與 seams

```
 tests/e2e_fixture.py ─────────── E2EServer：頁面、gzip/chunked/302、請求紀錄 since(n)
        │                          （probe、integration、window 測試共用）
        ▼
 tests/e2e_shape.py ───────────── 把原始輸出轉成 checkpoint 形狀
        │                          重用 display_differential 的 oracle_leaves/native_leaves
        │                          和 browser_differential 的 layout_shape（搬到這裡共用）
        ├──▶ tests/e2e_oracle_probe.py ──▶ tests/fixtures/e2e_oracle.json
        │      Python BrowserApp + SDL dummy，用 BrowserWindow 方法跑劇本
        │
        └──▶ tests/e2e_integration.py ◀── STATE <scenario> <step> <raw json> ── tests/test_e2e_window.c
               比對每個檢查點（數值 tolerance 0.0001）          window_driver.h，SDL 事件驅動

 tests/e2e_window_integration.py ── window_session.sh（Xvfb）
        劇本同上 + /inline.html（native-only，D6）
        檢查：截圖區域顏色、requests、events、正常結束
```

Seam 1：`E2EServer` 的請求紀錄。Seam 2：checkpoint JSON。Seam 3：`STATE` 協定（沿用
`window_driver.h`）。測試只透過這三個 seam 觀察結果，不讀內部狀態。

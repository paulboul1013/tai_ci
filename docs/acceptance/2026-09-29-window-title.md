# 視窗標題跟隨頁面、改名 Tai Ci 驗收（2026-09-29）

**狀態：`VALIDATING`。** 依 [計畫](../window-title-plan.md) 完成步驟 1–6，分支 `window-title`。整體
browser 仍未達 `COMPLETE`（外部開啟、完整 chrome 視覺比對、JS mutation 等仍缺，見 `PORTING_PLAN.md`）。

## Python oracle

- `tests/title_oracle_probe.py` 以 SDL dummy driver、127.0.0.1 fixture（`tests/title_fixture.py`）與
  不受信任 CA 的 HTTPS server（`tests/https_fixture.py`）凍結 8 個情境到
  `tests/fixtures/title_oracle.json`：18 種 `<title>` markup、Network／Certificate Error、
  `about:bookmarks`、pending、分頁切換與背景分頁載入、兩個視窗、剛建立的視窗、JS 改寫標題。
  每個檢查點記錄 `Tab.get_title()`、committed state 的 `title` 與呈現後的 `SDL_GetWindowTitle()`。
  連續三次輸出相同；已加入 CTest `title_oracle_probe`。
- 固定下來的規則：第一個非空 `<title>` 勝出（空的與只有空白的被跳過）；只串接直接子 Text
  （`x<b>y</b>z` → `xz`）；entity 會解碼；`str.strip()` 也會去掉 `&nbsp;`、U+00A0、U+3000 與
  `\v\f`，中間的換行保留；`<body>` 內與沒有 `<head>` 的 `<title>` 都算。Network Error 沒有
  title → 後備名稱；Certificate Error → `Certificate Error`。pending 期間保留舊標題；背景分頁
  載入完成不改變視窗標題；剛建立與第一次 commit 前都是後備名稱；JS 以 `innerHTML` 改寫後標題跟著變。

## Native 比對與單元測試

- `tai_page_title()`（`src/browser.c`）：空白判斷用 utf8proc 的 Zs 與 bidi WS／B／S，與 Python
  `str.isspace()` 定義相同；無效 UTF-8 位元組視為非空白。`test_page_title.c` 對 18 種 markup 與
  oracle 逐一相同，另測 `NULL` page 與無效 UTF-8（`" \xff x \xc3"` → `"\xff x \xc3"`）。
- 憑證錯誤頁加上 `<title>Certificate Error</title>`；Network Error 維持沒有 title。
- `test_title_window.c`（dummy SDL、真實多視窗迴圈）32 個檢查點與 oracle 相同，oracle 的
  `Tai Gar` 對應為 `Tai Ci`。切換分頁以點擊 tab strip 觸發；背景分頁是否載入完成以不經過 frame 的
  暫時 select 檢查。未比對：`committed_title`（native 直接讀已提交頁面）、`fresh_window.created`
  （observer 第一次執行已在第一個 frame 之後，以 `presented_before_commit` 代表）、`dom_change`
  （native JS 沒有 `innerHTML`，屬既有 JS mutation 缺口）。
- `new_window_integration` 不再略過 `title`，23 個檢查點含每個視窗的標題皆相同。
- `network_differential` 把 oracle 的 `Tai_Gar/1.0` 對應為刻意差異 `Tai_Ci/1.0`（並斷言 oracle 仍送
  `Tai_Gar/1.0`）。

## 建置、CTest 與 sanitizer

- `cmake --build build`（`-Werror`）無警告；完整 `ctest --test-dir build -j 3`：45/45 通過。
- `build-asan`（`-DTAI_SANITIZERS=ON`）整套 45/45 通過，`ASAN_OPTIONS=detect_leaks=1`，LSan
  suppression 僅 `leak:libfontconfig.so`、`leak:libcairo.so`。審查後的修改（標題設定失敗不快取、
  無效 UTF-8 測試）重建後，以 ASan＋LSan 重跑 `title_integration`、`new_window_integration`、
  `presentation_dummy`、`browser_tabs`：4/4 通過；一般建置再跑完整 CTest 45/45。

## 獨立審查

`ownership-reviewer` 審查 `tai_page_title()`、錯誤頁 markup、`PresWindow.shown_title` 與測試：沒有
正確性或記憶體安全缺陷。採納一項低嚴重度建議：`SDL_SetWindowTitle()` 失敗時不快取，下次重繪重試。
審查指出未測的路徑：`tai_page_title()` 與 `window_sync_title()` 的配置失敗、`SDL_SetWindowTitle()`
失敗（無法在 dummy driver 觸發）；無效 UTF-8 已補測。

## 真實視窗（Xvfb）

在 `unshare -rn` 建立、只有 loopback 的網路命名空間內執行 Xvfb、127.0.0.1 fixture server
（`tests/fixtures/title_window/`）與 `tai-browser --window`，X 用戶端以 `nsenter` 進入同一命名空間，
讓 New Tab／Ctrl+N 的 `https://browser.engineering/` 不會連外。`window_session.sh` 改以
`xdotool search --pid` 找視窗；實測每個 SDL 視窗只有一個 X 視窗（`_NET_WM_PID` 相符、WM_CLASS
`tai-browser`）。標題以新增的 `title`／`windows` 指令（`xdotool getwindowname`）讀取：

1. 載入 `/index.html` → `Index Page`。
2. 點頁面連結到 `/second.html` → `第二頁 Second`（非 ASCII 正確）。
3. 地址欄輸入 `/notitle.html` → `Tai Ci`；Alt+Left 回上一頁 → `第二頁 Second`。
4. New Tab（Network Error 頁，無法解析主機）→ `Tai Ci`；點 Tab 0 → `第二頁 Second`。
5. Ctrl+N：第二個 PID 相符的視窗標題 `Tai Ci`（Network Error），視窗 1 仍是 `第二頁 Second`；在視窗 2
   導覽到 `/index.html` → `Index Page`，視窗 1 不變。
6. 對視窗 2 送 `WM_DELETE_WINDOW`：只剩視窗 1（`第二頁 Second`）。`stop` → `browser exit=0`。
7. request log：`GET /index.html`、`/second.html`、`/notitle.html`、`/second.html`、`/index.html`，
   全部 200。

截圖 t1–t5 已目視確認，存放於本次 session 的 scratchpad，未加入 repo。

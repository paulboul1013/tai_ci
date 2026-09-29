# 新視窗（Ctrl+N）：實作計畫

**狀態：`PLANNED`（2026-09-29），決定 D1–D4 已確認，可從步驟 1 開工。** 本工作項目接在
[PORTING_PLAN.md](../PORTING_PLAN.md)「Chrome 與 History」之後，處理 Browser / window
子系統的第一個缺口「新視窗（Ctrl+N）」。完成後的驗證證據寫入 `docs/acceptance/`，
刻意差異寫入 `PORTING_PLAN.md`「已知差異與範圍」。

## 一句話摘要

在 `tai-browser --window` 中按 Ctrl+N 會開一個新的 800×600「Tai Gar」視窗，新視窗有一個
載入首頁的分頁；所有視窗共用 cookie 與書籤，各自擁有分頁、history 與地址欄；關閉一個視窗
不影響其他視窗，關掉最後一個視窗才結束程式。

## 整體流程（給人看的版本）

```
 步驟 0  使用者確認決定 ──┐
                          ▼
 步驟 1  Python oracle：寫 probe，凍結 Python 在 9 個情境的答案（JSON fixture）
                          ▼
 步驟 2  拆出共用層：TaiBrowserApp（loader thread、cookie、書籤）── 單視窗行為不變
                          ▼
 步驟 3  多視窗事件迴圈：一個 SDL 迴圈管理 N 個視窗 ─────────── 單視窗行為不變
                          ▼
 步驟 4  Ctrl+N 與關閉：接上快捷鍵、事件路由、關閉單一視窗 ── 與 oracle 逐點比對
                          ▼
 步驟 5  真實視窗驗證：Xvfb 裡實際按 Ctrl+N、操作兩個視窗、截圖
                          ▼
 步驟 6  收尾：完整 CTest、ASan/UBSan/LSan、獨立審查、更新文件與驗收紀錄
```

步驟 2、3 是「只重構、不改行為」：完成條件是既有測試全部照舊通過。真正的新行為只在
步驟 4 出現，這樣出問題時可以清楚分辨是重構造成還是新功能造成。

## 目標與完成條件

對照 Python 的 `BrowserApp` 與 `BrowserWindow`，讓 native 支援多視窗，並保持每個視窗內
既有的分頁、history、地址欄、書籤與 HTTPS 行為完全不變。

完成條件（全部要有可重跑的證據）：

1. `tests/new_window_oracle_probe.py --check` 通過，fixture 三次產生結果相同。
2. `tests/new_window_integration.py` 逐點比對 native 與 fixture，只有列出的刻意差異例外。
3. 既有 CTest 全數通過（重構未改變單視窗行為）。
4. ASan/UBSan＋LSan 跑受影響測試通過，特別是「關閉仍有載入中分頁的視窗」。
5. Xvfb 真實視窗：Ctrl+N、兩視窗分別操作、共用書籤、關閉其一、關閉最後一個正常結束，
   有截圖與 request log。
6. `ownership-reviewer` 獨立審查無未處理的問題。

## Python 依據（`tests/reference/browser.py`）

用 `python3 tests/tools/oracle_symbols.py <符號>` 定位，只讀需要的行段。

| 行為 | 位置 | 內容 |
|---|---|---|
| App 擁有共用狀態 | `BrowserApp.__init__` 2530–2577 | 一個 process 共用：network thread、`visited_urls`、`bookmarks`、視窗清單 `windows` 與 `windows_by_id`。 |
| Cookie 共用 | 全域 `COOKIE_JAR` 41、122–130、4574–4577 | Cookie 是 process 全域，所有視窗、分頁共用。 |
| 建立視窗 | `BrowserApp.new_window` 2581–2589 | 建 `BrowserWindow`、登記到清單、`new_tab(url)`；`url` 預設 `https://browser.engineering/`。 |
| Ctrl+N | `dispatch_event` 2794–2804 | `SDL_KEYDOWN` 且 `mod & KMOD_CTRL` 且鍵為 `n` → `handle_new_window()`。**先於** Enter/方向鍵等判斷，與地址欄焦點無關；沒有檢查其他修飾鍵，也沒有過濾 key repeat。 |
| 新視窗網址 | `BrowserWindow.handle_new_window` 8510–8513 | 固定 `https://browser.engineering/`，不沿用目前分頁網址。 |
| 視窗外觀 | `BrowserWindow.__init__` 7435–、`SDL_CreateWindow` 7510–7517 | 標題 `Tai Gar`、`WIDTH,HEIGHT = 800,600`（205）、`SDL_WINDOWPOS_CENTERED`：新視窗與舊視窗疊在同一位置。 |
| 事件路由 | `dispatch_event` 2665–2826 | 滑鼠、滾輪、按鍵、文字輸入、視窗事件都以 `windowID` 找視窗；找不到就丟掉。 |
| 關閉單一視窗 | `SDL_WINDOWEVENT_CLOSE` 2678 → `BrowserWindow.close` 7601–7645 | 停掉並 join 該視窗所有分頁的 thread，丟棄其 raster 工作，`unregister_window`，銷毀 SDL window。 |
| 最後一個視窗 | `BrowserApp.unregister_window` 2591–2597 | 清單變空時 `running = False`，主迴圈結束。 |
| SDL_QUIT | `dispatch_event` 2669 → `run` finally 2877–2893 | 停止迴圈並關閉**所有**視窗。 |

## Native 現況

- **`TaiTabSet`（`src/tabset.c`、`include/tai/tabset.h`）同時擁有三種東西：** 一個視窗的
  分頁與 session、一條 loader thread 與其 `TaiNetwork`（cookie 存在 `src/network.c` 的
  `TaiNetwork.cookies`）、以及 `TaiBookmarks`（持久化檔案）。若直接為每個視窗建立一個
  `TaiTabSet`，cookie 會各自獨立，兩個書籤物件還會互相覆蓋同一個檔案——都違反 Python
  行為，所以需要先拆出共用層（步驟 2）。
- **Presentation（`src/presentation.c` 的 `present_tabset_window`，277–586）：** 一個函式
  內包辦 `SDL_Init`、單一 `SDL_Window`、renderer、texture、地址草稿與事件迴圈，最後
  `SDL_Quit`。收到 `SDL_EVENT_QUIT` 或本視窗的 close request 就結束。需要拆成「每視窗
  狀態」加「一個 app 層迴圈」（步驟 3）。
- **`src/main.c`：** `--window` 建立一個 `TaiTabSet` 後呼叫
  `tai_present_window_with_tabs()`。
- **快捷鍵：** 目前只有 Alt+Left/Right（`presentation.c` 412–417）；Ctrl+N 沒有作用。
- **`:visited`：** CSS 有 `:visited` selector，但 native 從未設定 `node->visited`，
  所以 Python 的共用 `visited_urls` 目前沒有對應功能，**不在本工作範圍**。
- **真實視窗工具：** `tests/tools/window_session.sh` 只記錄一個 `wid`（104–112 取第一個
  屬於該 PID 的 `Tai Gar` 視窗），需要擴充成可列出、切換多個視窗（步驟 5）。

## 決定項目（使用者已於 2026-09-29 確認，見文末）

| # | 問題 | Python | 建議 |
|---|---|---|---|
| D1 | 視窗數量上限 | 無上限 | **上限 10 個視窗**（每視窗仍最多 25 分頁）；達上限時 Ctrl+N 不動作並在 stderr 警告。理由與既有「25 分頁上限」一致：避免誤按住鍵造成資源耗盡。 |
| D2 | 按住 Ctrl+N 的 key repeat | 不過濾，按住會連續開很多視窗 | **忽略 repeat**，一次按下只開一個視窗。記為刻意差異。 |
| D3 | 新視窗建立失敗（SDL/配置失敗） | 丟例外，整個程式崩潰 | **只在 stderr 報錯，既有視窗繼續運作**。記為刻意差異。 |
| D4 | 新視窗網址 | 固定 `https://browser.engineering/` | 使用與 New Tab 相同的 `home_url`（正式版就是 `https://browser.engineering/`，測試可替換成本機網址）。正式版行為相同，不算差異。 |

## 需先用 oracle 固定的問題（步驟 1）

新增 `tests/new_window_oracle_probe.py` 與 `tests/fixtures/new_window_oracle.json`。
Probe 使用 SDL dummy driver 與本機 HTTP server，直接以 `app.dispatch_event()` 送合成
SDL 事件。因為 `handle_new_window` 寫死外部網址，probe 包裝 `BrowserApp.new_window`：
記錄 Python **原本要求**的網址，再改成本機網址載入，避免連外。

每個檢查點記錄：視窗數、每個視窗的寬高與標題、分頁數、active 分頁 URL／history／index、
地址欄文字／dirty／focus、書籤星狀態，以及伺服器收到的 method／path／Cookie header。

1. **Ctrl+N 基本：** 視窗 1 載入 A 後按 Ctrl+N。新視窗大小、標題、分頁數 1、要求的網址、
   history、地址欄狀態。
2. **原視窗不受影響：** 視窗 1 有兩個分頁、history [A, B]、地址欄有未送出的草稿且聚焦時按
   Ctrl+N：視窗 1 的分頁、history、草稿與焦點是否保留。
3. **修飾鍵組合：** Ctrl+N、Ctrl+Shift+N、Ctrl+Alt+N 是否都開視窗；單按 N 不開視窗。
4. **事件路由：** 在視窗 2 點擊連結、輸入文字、滾動、按 Enter，只有視窗 2 改變。
5. **共用書籤：** 視窗 2 收藏頁面 C 後，視窗 1 導覽到 C 時星號為已收藏；視窗 1 的
   `about:bookmarks` 列出 C。
6. **共用 cookie：** 視窗 1 的頁面回應 `Set-Cookie`，之後視窗 2 對同一 host 的請求帶有該
   cookie。
7. **從新視窗再開：** 視窗 2 按 Ctrl+N 產生視窗 3，三個視窗互相獨立。
8. **關閉單一視窗：** 對視窗 1 送 close 事件（含視窗 1 有載入中分頁的情況）：視窗 2 仍在、
   可繼續導覽；app 仍在執行。
9. **關閉最後一個視窗與 SDL_QUIT：** 關閉最後一個視窗後主迴圈結束；另測有兩個視窗時送
   `SDL_QUIT` 會關閉全部。

注意（沿用 history probe 的經驗）：Python 的 commit 由 `run_animation_frame` 產生，要以
`tab_call(... run_animation_frame)` 讓 `committed_states` 更新；每個頁面標題唯一，避免
`wait_for` 提前成立；port 正規化為 `<PORT>`。

## 設計方向（以 oracle 結果為準）

### 步驟 2：共用層 `TaiBrowserApp`

```
            TaiBrowserApp（process 內一個，SDL owner thread 擁有）
            ├─ loader thread ＋ TaiNetwork（cookie）   ← 原本在 TaiTabSet
            ├─ TaiBookmarks（持久化檔案，只開一次）      ← 原本在 TaiTabSet
            ├─ default_css、rtl、home_url、ca_file（測試用）
            └─ 全域唯一的 tab ID 產生器
                 ▲ 借用（borrow）
     ┌───────────┼───────────┐
 TaiTabSet（視窗 1）  TaiTabSet（視窗 2） …  每個視窗一個，擁有自己的分頁與 session
```

- 新增 `include/tai/app.h`、`src/app.c`：`tai_browser_app_create*()`／`_destroy()`、
  `tai_browser_app_pump()`（把 loader 完成的工作依 tab ID 分送給對應 `TaiTabSet`）。
- `TaiTabSet` 改為借用 `TaiBrowserApp`，新增 `tai_tabset_create_in_app(app, ...)`。
  **既有的 `tai_tabset_create*()` 保留**，內部建立一個私有 app 並擁有它，讓既有測試與
  呼叫端不必修改。
- **所有權規則：** app 必須比所有借用它的 tab set 活得久；銷毀 tab set 時取消它所有
  排隊與進行中的載入，並丟棄之後才送到的完成結果（依 tab ID 找不到擁有者就釋放）。
  所有公開操作仍只在 SDL owner thread 呼叫；loader thread 只碰 app 的佇列。
- 細節先依 `architecture-and-ownership` skill 設計，並更新
  `docs/architecture/native-runtime.md`。

### 步驟 3：多視窗事件迴圈

- 把 `present_tabset_window` 拆成：
  - `PresWindow`：`SDL_Window`、renderer、兩個 texture、`AddressEditor`、`AddressWatch`、
    `row_wraps`、focus、text input 狀態，以及它擁有的 `TaiTabSet`。
  - App 迴圈：`SDL_Init` 一次；每回合依事件的 `windowID` 找 `PresWindow`，找不到就丟掉
    （比照 Python）；之後 `tai_browser_app_pump()` 一次，再逐一更新每個視窗的畫面。
- 新增公開入口 `tai_present_browser(TaiBrowserApp *app, const char *initial_url, int width,
  int height, char **error)`；`tai_present_window_with_tabs()` 保留（單視窗、不接 Ctrl+N），
  讓既有 presentation 測試不變。
- `TaiPresTabsObserver` 測試縫擴充成能看到視窗清單與各視窗 ID，供 dummy SDL 測試送事件。

### 步驟 4：Ctrl+N 與關閉

- `SDL_EVENT_KEY_DOWN`、`key == SDLK_N`、`mod & SDL_KMOD_CTRL`、非 repeat（D2）→ 建立新
  `PresWindow`（800×600、`Tai Gar`、置中、新 `TaiTabSet` 載入 `home_url`）。判斷放在地址欄
  按鍵處理**之前**，與 Python 順序相同。
- 超過上限（D1）或建立失敗（D3）→ stderr 警告，既有視窗不受影響。
- `SDL_EVENT_WINDOW_CLOSE_REQUESTED` → 只關閉該視窗：先銷毀其 `TaiTabSet`（取消載入），
  再銷毀 texture、renderer、window。清單變空就結束迴圈。
- `SDL_EVENT_QUIT` → 關閉全部。SDL3 預設在最後一個視窗關閉時也會送 `SDL_EVENT_QUIT`，
  兩條路徑都要安全（不可重複釋放）。
- `src/main.c` 的 `--window` 改為建立 app 後呼叫 `tai_present_browser()`。

## 驗證計畫（每一層各證明什麼）

| 層級 | 檔案／指令 | 證明什麼 |
|---|---|---|
| Python oracle | `tests/new_window_oracle_probe.py --check`（加入 CTest） | Python 在 9 個情境的真實答案被凍結，之後不靠記憶判斷。 |
| C 單元：共用層 | 新 `tests/test_browser_app.c` | 兩個 tab set 共用 app：分頁互不串線、書籤共用、cookie 共用（本機 HTTP fixture）、銷毀有載入中分頁的 tab set 不洩漏、不 UAF。 |
| 回歸 | 完整 `ctest --test-dir build` | 步驟 2、3 的重構沒有改變任何單視窗行為。 |
| Dummy SDL | 新 `tests/test_new_window.c` | 透過 observer 送 Ctrl+N、修飾鍵組合、repeat、依 windowID 的點擊／輸入、關閉單一視窗、SDL_QUIT、上限。 |
| Native 對 oracle | 新 `tests/new_window_integration.py` | 以同一組 fixture 驅動 native，逐點比對 oracle 答案（只容許 D1–D3 差異）。 |
| Sanitizer | `build-asan/`，ASan/UBSan＋LSan（suppression：`leak:libfontconfig.so`、`leak:libcairo.so`） | 關閉視窗、關閉仍在載入的視窗、SDL_QUIT 關全部時無洩漏、無 UAF、無 double free。 |
| 真實視窗 | `native-window-verification` skill，Xvfb（WSLg 會丟合成按鍵） | 實際 Ctrl+N 出現第二個 PID 相符的 `Tai Gar` 視窗；在視窗 2 操作；共用書籤；關閉視窗 1 後視窗 2 仍可用；關閉最後一個 rc=0。截圖＋server request log＋SDL event log（windowID 正確）。 |
| 獨立審查 | `ownership-reviewer` subagent | App／tab set／視窗的生命週期、取消與 thread 邊界。 |

`window_session.sh` 需要擴充：列出同一 PID 的所有 `Tai Gar` 視窗（`windows` 指令）、
切換目前操作的視窗（`select <n>`），並在關閉後確認剩餘視窗與程序狀態。

## 實作順序與完成條件

| 步驟 | 內容 | 完成條件 |
|---|---|---|
| 0 | 使用者確認 D1–D4 | 已完成（2026-09-29）。 |
| 1 | Oracle probe 與 fixture | Probe 可重跑、三次輸出相同，涵蓋問題 1–9；加入 CTest。 |
| 2 | 拆出 `TaiBrowserApp` | 完整 CTest 通過；`test_browser_app.c` 通過；ASan＋LSan 通過；`native-runtime.md` 更新。 |
| 3 | 多視窗事件迴圈 | 完整 CTest 通過（單視窗行為不變）；observer 能看到視窗清單。 |
| 4 | Ctrl+N、路由、關閉 | `test_new_window.c` 與 `new_window_integration.py` 通過；ASan＋LSan 通過。 |
| 5 | 真實視窗 | Xvfb session 證據（截圖、request log、event log）存入 acceptance 紀錄。 |
| 6 | 收尾 | 完整 CTest、整套 sanitizer、`ownership-reviewer` 審查；更新 `PORTING_PLAN.md`、`ARCHITECTURE.md`、`docs/reference-presentation.md`、`docs/architecture/native-runtime.md`，新增 `docs/acceptance/<日期>-new-window.md` 並在 `ACCEPTANCE.md` 加一列。 |

每個步驟完成後各自 commit；步驟 2、3 若在 CTest 發現行為改變，先修正再進下一步。

## 預期的刻意差異（D1–D3）

- **視窗上限（D1）：** native 最多 10 個視窗；Python 無上限。
- **Key repeat（D2）：** native 按住 Ctrl+N 只開一個視窗；Python 會連續開。
- **建立失敗（D3）：** native 報錯後繼續；Python 崩潰。

## 不在本工作範圍

- `mailto:` 等外部開啟、中鍵在新分頁開啟、完整 chrome 視覺比對、窄寬分頁標籤排版
  （仍是 `PORTING_PLAN.md` 的其他缺口）。
- `:visited` 連結樣式與共用 `visited_urls`（native 尚未實作 `:visited` 狀態）。
- 視窗之間拖曳分頁、記住視窗位置。

## 風險

- **SDL3 多視窗的 text input：** `SDL_StartTextInput` 以視窗為單位；焦點切換時要確保只有
  聚焦視窗接收文字，否則輸入可能送錯視窗。以 dummy SDL 與 Xvfb 兩層驗證。
- **關閉時序：** loader thread 可能在 tab set 銷毀後才送出完成結果；由 app 依 tab ID
  丟棄。這是 sanitizer 與 `ownership-reviewer` 的重點。
- **重構範圍：** `present_tabset_window` 約 300 行；拆分時保持逐行語意不變，靠既有
  presentation 測試守住。

## 決定

使用者於 2026-09-29 確認全部採用建議：

1. **D1 視窗上限：** 最多 10 個視窗；達上限時 Ctrl+N 不動作，stderr 警告。
2. **D2 Key repeat：** 忽略 repeat，一次按下只開一個視窗。
3. **D3 建立失敗：** stderr 報錯，既有視窗繼續運作。
4. **D4 新視窗網址：** 沿用 tab set 的 `home_url`（正式版為 `https://browser.engineering/`）。

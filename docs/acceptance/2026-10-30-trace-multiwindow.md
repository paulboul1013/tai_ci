# Trace 多視窗（2026-10-30）

狀態：VALIDATING。

## 問題

Native 只有 `Browser thread` 與 `Loader thread`。開兩個視窗時，trace 不能指出 `render`、
`raster_and_draw`、`sdl_present` 屬於哪個視窗。Python 用每個 tab 的 `Main thread` 區分，
native 沒有這種執行緒。使用者 2026-10-30 決定比照 Firefox：頁面事件帶 tab，合成事件帶 window，
用一張表連起來（`PORTING_PLAN.md` D-T11）。

## 變更

- `render` 帶 `tab`。值來自 `TaiPageNet.trace_tab`（0 表示省略）。Loader 在 `load_task_start`
  以不可變的 `task->tab_id` 填入，所以 loader 的第一次 render 已經帶 tab。Pump 提交時複製
  `event_net`，填入同一個 tab 後再呼叫 `tai_page_set_net`（它複製整個 struct）。
- `raster_and_draw` 與 `sdl_present` 帶 `window`（SDL window ID）。
  `tai_pres_repaint_tabs_scene` 多一個 `SDL_WindowID` 參數。
- Native-only instant `tab_window`（`tab`、`window`，`Browser thread`）。`window_frame` 在加入 tab 的
  同一輪迴圈發出，每個 tab 一次：第一個 tab、New Tab、中鍵開新分頁、Ctrl+N 新視窗的 tab。
  Tab 只會附加，所以每個視窗記住已發出的 tab 數。新 getter：`tai_tabset_tab_id`。
- `tai-browser --window --home URL` 設定 New Tab 與 Ctrl+N 的頁。`tai_browser_app_create` 多一個
  `home_url` 參數（NULL 為 https://browser.engineering/），仍開啟持久書籤。沒有 `--window`、
  缺值或空值時印 usage 並 exit 2。`window_session.sh start --home URL` 傳給 browser。
- `tests/trace_oracle_probe.py` 的 `NATIVE_ONLY` 加入 `tab_window`。

## 證明了什麼

`tests/trace_window_integration.py` 的 `multiwindow_run`：Xvfb、真實 X11 輸入、trace 開啟。
Fixture 是測試在暫存目錄寫的 `a`、`b`、`c`、`home` 四頁，由 127.0.0.1 server 提供，
`--home` 指向其中的 `home.html`。步驟：視窗 1 開 `/a.html`；`key ctrl+n`、`await 2`、`select 2`；
視窗 2 顯示 home；視窗 2 導覽到 `/b.html`；`close` 視窗 2、`await 1`；`select 1`，導覽到 `/c.html`；停止。

- **M1：** `/a.html` 與 `/c.html` 的 `commit` 在同一個 tab，它經 `tab_window` 對到視窗 1；
  home 與 `/b.html` 在另一個 tab，對到另一個視窗。對應表剛好兩列，每個視窗一個 tab。
- **M2：** 每個 `raster_and_draw`／`sdl_present` 都帶 `window`，值剛好是兩個不同的 window ID。
- **M3：** `/c.html` 的 `Task: Tab.load` 在視窗 2 關閉後才開始。從它開始沒有任何 window 2 的
  frame（範圍：從 `/c.html` 開始載入算起；關閉到開始載入之間的 frame 沒有檢查，trace 裡沒有關閉時間點）；`/c.html` commit 之後，視窗 1 仍然 `sdl_present`。
- **M4：** 每個 `render` 都帶 `tab`，每個 tab 都對到視窗，兩個 tab 都有 render。整個檔案是
  有效 JSON，B/E 與 b/e 配對（`trace_check.check`），名稱都在 vocabulary 內。
- **只用本機：** 每個 fetch 的 `url` 都以 fixture 的 `http://127.0.0.1:<port>/` 開頭；
  server log 有 `"GET /home.html" 200`（以及 a、b、c）。
- 一次執行的實際內容：`tab_window` `{tab 1, window 2}`、`{tab 2, window 3}`；commit 依序為
  a（tab 1）、home（tab 2）、b（tab 2）、c（tab 1）；render 帶 tab 1 與 tab 2 各兩次。

### 突變（一次性，已從備份還原，`cmp` 相同）

- (i) `sdl_present` 不帶 `window`：`AssertionError: M2: 234 frame spans without a window arg`。
- (ii) Ctrl+N 視窗不發 `tab_window`（`open_new_window` 把已發出數設為 `TAI_MAX_TABS`）：
  `AssertionError: M1: tab 2 of http://127.0.0.1:41007/b.html has no tab_window`。
  M1 先失敗，所以 M4 在此突變下沒有單獨執行。

### 指令

- `cmake --build build`：沒有警告（`-Werror`）。
- `ctest --test-dir build -R 'trace|cli' --output-on-failure` 連跑 3 次：每次 6/6 通過
  （152–156 秒）。`browser_screenshot`（`test_cli`）另跑通過。
- `ctest --test-dir build -j4`：76/76 通過（195 秒）。第一次執行時 `javascript` 的
  `test_append_cost` 計時斷言在 -j4 負載下失敗（1.48 秒 > 1.0 秒），單獨重跑通過，第二次完整執行 76/76。
- `python3 tests/tools/check_test_registration.py`：通過。
- `build-asan`（ASan＋UBSan＋LSan）的 `tai-browser` 與 `test_trace`，跑整個
  `trace_window_integration`：修正前 3 次中 2 次通過（約 130 秒），1 次在 `multiwindow_run` 停止時失敗：
  `window_session: pid … is not tai-browser`；較早另有 2 次與其他測試並行時在 `traced_run` 失敗
  （`browser exit=143`）。原因：最後一個視窗關閉後 browser 自己結束，ASan 版結束較慢，`stop` 讀
  `/proc/PID/exe` 時程式正在結束。修正：`window_session.sh stop` 讀不到 exe 時先等 `rc` 檔。
  修正後連跑 3 次全部通過。所有 log 都沒有 sanitizer 報告。

## 剩餘缺口

- `traced_run` 在 ASan 版與其他測試並行時的 `exit=143`（既有缺口「ASan 版整合測試偶發無法結束」）沒有查明；單獨執行時沒有重現。
- 視窗的第一組 `raster_and_draw`／`sdl_present` 早於它的 `tab_window`；讀 trace 要用 tab ID 對應，不能依時間順序。
- `test_cli` 的 `--home` 錯誤用法斷言在修改前也會通過（未知選項本來就 exit 2）；`--home` 的正面
  行為只由 `multiwindow_run` 證明。
- 中鍵開新分頁與 New Tab 按鈕的 `tab_window` 走同一個 `window_frame` 路徑，但沒有專門的 trace 測試。
- 沒有在 WSLg 執行。

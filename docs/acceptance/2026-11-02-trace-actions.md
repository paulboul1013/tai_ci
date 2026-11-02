# Trace 動作紀錄（D-T12，2026-11-02）

狀態：VALIDATING。

## 問題

使用者手動操作後，trace 中 5 次點擊只有 5 個 `Task: Tab.click {}`，看不出點在哪裡、點到什麼、為什麼
沒有導覽；DuckDuckGo 的 400 要用 curl 重現才知道；resize 後的重新 layout 沒有任何 span。

## 新增

- `include/tai/trace.h`、`src/trace.c`：`input`、`action` 兩個 native 專用 instant；
  `tai_trace_fetch_done(fetch, status, bytes, error)`。
- `src/presentation_events.c`：`tai_pres_trace_input`（click／wheel／key／text／resize／close／focus）、
  `tai_pres_trace_action`；捲動、頁面文字與頁面按鍵的 action。會打出字元的鍵（含數字鍵盤、AltGr）記成
  `char`，只有 Ctrl／左 Alt／GUI 快捷鍵保留鍵名；text 只記字數。
- `src/presentation_tabs.c`：`window_event` 包一層，記 input，body 沒有寫 action 時補 `none`；
  關閉、焦點、地址列（`address_text`、`address_key`、`address_submit`、`address_blur`）、history 快捷鍵、
  fragment、導覽（url、method、external、result；不記 POST body）、resize、中鍵 `open_link`、Ctrl+N `new_window`。
- `src/presentation_address.c`：chrome 點擊的 `hit`（`new_tab`、`new_tab_full`、`select_tab`、
  `bookmarks`、`bookmark_toggle`、`bookmark_unavailable`、`address_focus`、`back`、`forward`、
  `history_unavailable`、`blank`）。
- `src/browser.c`：頁面點擊的 action（tab、target、href、result：`none`／`prevented`／`submit`／`button`／
  `checkbox`／`input_focus`／`fragment`／`navigate`／`error`）；`tai_page_resize` 包在 `render` span；
  文件與資源請求結束時帶結果（取消為 `cancelled`、送出失敗為 `submit failed`）。
- `src/tabset.c`：非同步 XHR 結束時帶結果。
- 測試：`test_trace` 的 `test_fetch_done`；新的 `tests/trace_actions_window_integration.py` 與
  `tests/fixtures/trace_actions_window/`；`trace_oracle_probe` 的 `NATIVE_ONLY` 加入 `input`、`action`。

## 證明了什麼

- **Xvfb 真實視窗：** 點純文字、文字框、密碼框，打字，數字鍵盤 5／7、Shift+A、Ctrl+A，Enter，滾輪，
  點 404 連結，Back，點存在的連結，新分頁，resize，關閉。每個 input 後至少一個 action；打過的字不在 trace 中；`missing.html` 的
  `e` 為 status 404、`next.html` 為 200；resize 後同一執行緒有帶 tab 的 `render`。
- **突變：** 五個一次性突變都讓 `trace_actions_window_integration` 變紅：可列印鍵記出鍵名、數字鍵盤記出鍵名、
  text 記出內容、拿掉 `none` 補位、status 一律寫 0。（第一次「text 記出內容」沒有變紅，因為 xdotool
  逐字送 text 事件；因此補上 `length` 必須是整數的斷言。）每次以 `clear_stale_objects.sh` 強制重編，還原後轉綠。
- **回歸：** CTest 83/83（含新測試）。ASan-UBSan-LSan：`trace`、`trace_format`、`trace_oracle_probe`、
  `trace_window_integration`、`trace_actions_window_integration`、`tabset_xhr`、`tabset_loader_oom`、
  `tabs_oracle_probe` 通過；`browser_tabs`、`presentation_dummy` 失敗與修改前相同，洩漏都在 fontconfig
  （`FcPatternDuplicate`，Cairo toy font 路徑）。
- **審查：** `ownership-reviewer` 沒有找到生命週期、執行緒或控制流程問題；指出數字鍵盤（`Keypad 5`）與
  AltGr 會洩漏鍵名，已修正並加測試；GET 表單參數在網址中，記入 D-T12 說明。

## 剩餘缺口

同步 XHR 的 span 沒有 status；SDL_EVENT_QUIT 沒有 input 紀錄（它不屬於任何視窗）；AltGr 的判斷沒有真實鍵盤配置的自動測試。

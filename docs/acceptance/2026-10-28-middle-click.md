# 中鍵開新分頁（2026-10-28）

狀態：`VALIDATING`。比照凍結 Python 的 `BrowserWindow.handle_middle_click`／
`committed_link_at`／`new_tab`。

## 行為

- 內容區中鍵：地址列失焦但保留 dirty 草稿；頁面輸入焦點不變。以已提交 display list 命中測試，
  從命中節點往上找第一個有 `href` 的 `<a>`，以目前頁面 URL 解析；不派送 JS，所以
  `preventDefault` 擋不住。`#frag` 連結不特別處理，在新分頁載入 `page#frag`。
- mailto 連結交給外部開啟器（URL 去掉 fragment），不開分頁；其他 URL 新增分頁、設為 active
  並載入（history 只有該 URL）。新分頁成為 active 時地址列繼續顯示草稿。
- Chrome 區中鍵只清除頁面輸入焦點，不命中任何 chrome 控制項、不碰地址列。
- 使用者決定（2026-10-28）：維持 25 分頁上限；刻意差異記在 [PORTING_PLAN.md](../../PORTING_PLAN.md)
  的「Tabs 上限與呈現」列。
  差異紀錄見 `PORTING_PLAN.md`「Tabs 上限與呈現」列。

介面：`tai_page_link_at_viewport()`（`include/tai/browser.h`，純命中測試，呼叫者擁有 URL）、
`tai_tabset_open_link()`（`include/tai/tabset.h`，SDL 擁有者執行緒），由
`src/presentation_tabs.c` 的 `window_middle_click` 呼叫。

## 證據

| 層級 | 指令／方法 | 結果 |
|---|---|---|
| Python oracle | `python3 tests/middle_click_oracle_probe.py --check`（`webbrowser.open` 換成記錄器），CTest `middle_click_oracle_probe` | 7 個情境凍結於 `tests/fixtures/middle_click_oracle.json`，重跑一致 |
| Native 對 oracle | `ctest -R middle_click_integration`（dummy SDL 分頁迴圈、SDL 中鍵／左鍵、假開啟器） | 9 個檢查點逐欄相符（opened、tab_count、active、每個分頁的 url／heading／history、address、focused、dirty、input_focused、requests） |
| 負向對照 | 暫時拿掉 chrome 區 blur、讓內容區中鍵不讓地址列失焦 | `draft.text.focused`、`draft.link.focused`、`chrome.chrome.input_focused` 如預期失敗 |
| 分頁上限 | `ctest -R browser_tabs`（`tests/test_tabset.c` 的 `tab_limit_scenario`） | 第 25 個分頁由 `tai_tabset_open_link` 開出並載入該 URL；第 26 個回錯含「25」、分頁數與 active 不變；達上限時 mailto 仍成功（假開啟器 `true`） |
| 真實視窗 | `tests/tools/window_session.sh`（新增 `click X Y middle`），Xvfb :97，PATH 前置記錄用 `xdg-open` | 中鍵 mailto 連結→開啟器收到 `mailto:mid@example.com?subject=Mid`（fragment 已去除），無新請求、仍一個分頁；中鍵一般連結→`GET /second.html`，新的 active 分頁「Tab 1」顯示 MC Second、Back 停用；中鍵非連結文字與字間空白不動作（左鍵同點亦不命中）；browser exit=0 |
| 全套 | Debug CTest；`build-asan`（`ASAN_OPTIONS=detect_leaks=1`，suppression `leak:libfontconfig.so`、`leak:libcairo.so`，LSan 確實執行） | 62/62；ASan-UBSan-LSan 62/62 |

## 剩餘缺口

- oracle 情境未涵蓋：捲動後的中鍵、載入中（pending）時的中鍵、尚無 committed page、巢狀
  inline（`<a><b>x</b></a>`）與圖片連結、無法解析的 href（如 `javascript:`）。
- 達上限時的 stderr 診斷只有單元層檢查回錯，視窗層未自動驗證。

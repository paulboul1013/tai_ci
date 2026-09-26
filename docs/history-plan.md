# Chrome 與 History：實作計畫

**狀態：規劃完成，待實作（2026-09-26）。** 本切片是 [PORTING_PLAN.md](../PORTING_PLAN.md)
「下一個垂直切片」第 3 項「續做 Chrome 與 History」。前置的 [HTTPS 鎖頭](https-lock-plan.md)
與 tab 列換行修正已在 `https-lock` 分支完成。完成後的驗證證據寫入 `docs/acceptance/`，
刻意差異寫入 `PORTING_PLAN.md`。

## 目標與完成條件

對照 Python 的 `Tab`、`Chrome`、`BrowserWindow`，讓每個分頁的瀏覽紀錄、Back/Forward
按鈕、地址欄顯示與地址草稿都與 Python 一致；刻意保留的差異要有測試與紀錄。

完成條件：下列每一項都有可重跑的 Python oracle／native 對照；active 與 inactive 分頁的
URL、history index、按鈕狀態與請求方法互不串線；ASan/UBSan＋LSan 與真實視窗驗證通過。

## Python 依據（`tests/reference/browser.py`）

用 `python3 tests/tools/oracle_symbols.py <符號>` 定位，只讀需要的行段。

| 行為 | 位置 | 內容 |
|---|---|---|
| 初始 history | `Tab.__init__` 5729 | `history_index = -1`、`history = []`。 |
| 導覽即寫入 history | `Tab.load` 6067–6133（6082–6091） | 導覽**一開始**就設 `self.url = url`、scroll 歸零；`add_to_history` 時先截斷 `history_index` 之後的項目再 append、index+1。尚未收到回應。 |
| Back／Forward 可用 | `Tab.can_go_back/forward` 6144–6148 | `index > 0`、`index < len − 1`。因為 load 開始就改 index，pending 期間已反映新項目。 |
| Back／Forward | `Tab.go_back/forward` 6150–6159、`BrowserWindow.schedule_go_back` 7737 | 先改 `history_index`，再 `navigate(history[i], add_to_history=False)`：一律重新以 **GET** 從網路載入，不保存 DOM、scroll 或 POST body。會清除該 tab 佇列中的待辦工作。 |
| 連結 | `Tab.click` 6734–6752 | `href` 以 `#` 開頭 → `navigate_to_fragment`；其他 → `resolve` 後 `navigate`；外部 scheme → `open_external`。 |
| 同頁 fragment | `Tab.navigate_to_fragment` 6384–6398 | 只改 `url.fragment`、截斷並 append history，**不發請求**；下一次 render 呼叫 `scroll_to_fragment`（6317–6320、6334–6382）。 |
| 載入後捲到 fragment | `_finish_document_load` 5922 | 文件完成後 `pending_fragment = url.fragment`，render 時捲動；找不到 id 時不捲。 |
| 表單 | `Tab.submit_form` 6617–6636 | POST → `navigate(url, body)`；GET → 把 body 接在 URL 後。history 只存 URL。 |
| 地址欄顯示 | `Chrome.address_bar_display_text` 5207–5215、`BrowserWindow.active_url_string` 8256 | 聚焦或 dirty 時顯示草稿，否則顯示 **active tab 已提交狀態** 的 `url_string`；因為 commit 每幀發生且 `tab.url` 在 load 開始就改變，pending 期間顯示新 URL。 |
| 草稿清除 | `Chrome.discard_address_bar_edit` 5201、`BrowserWindow.commit` 7839–7843、8060–8062 | active tab 的已提交 `url_string` 改變時（含 load 開始、fragment、Back/Forward），下一次 raster 清除草稿與焦點。inactive tab 的 commit 不影響。 |
| Chrome 點擊 | `Chrome.click` 5270–5341 | New Tab、Back、Forward、書籤、tab 連結與空白 chrome 都會清除草稿；`set_active_tab`（7668）本身不清除。 |
| Enter | `Chrome.enter` 5601–5618 | 草稿轉 URL 後 `schedule_load`，並清除草稿。 |
| 鍵盤 | SDL 事件 2800–2815 | Python 的 Left/Right 只送地址欄游標；沒有 Alt+Left/Right history 快捷鍵（native 已有，列為既有差異）。 |

## Native 現況

- **Session history：** `src/session.c`。`append_url`（66–89）截斷目前 index 之後的項目再
  append；`tai_session_commit_history`（276–287）只改 index 與 page；
  `tai_session_record_fragment`（243）以 `append_url` 記錄同頁 fragment。
- **Tab set：** `src/tabset.c`。導覽在 loader thread 非同步進行，只有 commit 才寫入 session。
  `virtual_history()` 在 pending 期間把尚未提交的項目「虛擬」加入：一般導覽顯示為
  `count = index + 2`（看起來已截斷），history 導覽顯示為目標 index。`TaiTabSetView.url`
  pending 時是請求 URL。之後的載入失敗會丟棄候選頁與虛擬項目，舊頁面與**原本的
  forward 項目**都保留（既有回滾策略）。
- **Presentation：** `src/presentation.c`（tabbed 迴圈、Alt+Left/Right、fragment 記錄）、
  `src/presentation_address.c`（地址草稿、tab chrome 點擊）。
- **既有測試：** `tests/test_history.c`（session 層）、`tests/navigation_integration.py`
  ＋ `test_navigation.c`（POST、fragment）、`tests/test_tabset.c`＋`tabset_integration.py`
  （pending、history 失敗回滾、Referer）、`tests/test_chrome.c`、`tests/test_presentation.c`
  （地址草稿、Back/Forward 點擊）。目前**沒有**針對 history 的 Python oracle fixture。

## 需先用 oracle 固定的問題

新增 `tests/history_oracle_probe.py` 與 `tests/fixtures/history_oracle.json`，把 Python 的
實際答案凍結後再改 native。每一步記錄 active tab 的 `url`、`history`（正規化 port）、
`history_index`、`can_go_back/forward`、地址欄顯示文字、`address_bar_dirty`／`focus`，以及
伺服器收到的 method／path。

1. **基本 Back/Forward 與截斷：** A → B → C，Back 兩次到 A，再導覽 D：history 應為 [A, D]。
   每一步的按鈕狀態。
2. **Pending 期間：** 從 A 導覽到被 barrier 擋住的 B：URL、history、index、按鈕狀態與地址欄
   文字。釋放後的狀態。
3. **Pending 時按 Back：** A → B（pending）時按 Back。Python 預期 history 仍為 [A, B]、
   index 0、`can_go_forward` 為 true，並重新 GET A。native 會丟掉 B 的虛擬項目、Forward
   不可用；使用者決定**維持 native**，oracle 只用來把差異寫清楚。
4. **之後的載入失敗：** (a) A → B，導覽到會失敗的 D；(b) A → B → C，Back 到 B，導覽到 D；
   (c) 憑證錯誤；(d) Back/Forward 到已失效的 URL。記錄 Python 的錯誤頁標題、URL、history、
   index、按鈕狀態、HTTPS `secure`。使用者決定 native **改為比照 Python 顯示錯誤頁**。
5. **同頁 fragment：** 點 `#target`：不發請求、history 多一項、scroll 到目標；Back 回到
   沒有 fragment 的項目時 Python 會**重新 GET**（記錄請求數）與 scroll 值；Forward 回到
   fragment 項目時的 scroll。
6. **跨頁 fragment：** 連結到 `/page#target`：載入後捲到目標；找不到 id 時 scroll 為 0。
7. **POST 後 traversal：** A → 表單 POST 到 B → C，Back 到 B：伺服器收到 **GET** B（無 body）；
   Forward 再到 C。
8. **地址草稿：** 在地址欄輸入但不送出，然後分別 (a) 切換分頁、(b) 點頁面、(c) 頁面內
   fragment 連結、(d) 按 Back、(e) inactive 分頁完成載入、(f) active 分頁 pending 導覽開始：
   每種情況草稿是否保留、焦點是否保留。
9. **跨分頁獨立：** 兩個分頁各自有不同長度的 history，其中一個 pending；切換分頁後
   URL、index、按鈕狀態不串線；在分頁 1 按 Back 不影響分頁 0。

注意：Python 的 commit 由 `run_animation_frame` 產生，probe 要以 `tab_call(... run_animation_frame)`
讓 `committed_states` 更新（HTTPS probe 已踩過這個坑；只呼叫 `render()` 不會 commit）。
同一標題的頁面會讓 `wait_for(h1 == ...)` 提前成立，每個頁面要有唯一標題。

## 設計方向（以 oracle 結果為準）

- **維持非同步架構。** 不改成 Python「load 開始就寫入 history」的模型；以
  `virtual_history()` 讓 pending 期間的**可見**狀態符合 Python，commit 時才寫入 session。
  Pending 期間仍顯示舊頁面（既有差異，不變）。
- **之後的載入失敗改為提交錯誤頁（決定 2）：** `tai_tabset_pump()` 目前只在首次失敗
  （`initial_failure`）提交錯誤頁候選；改為**所有**網路／憑證失敗都提交錯誤頁。一般導覽
  以 `tai_session_commit_navigation` 寫入（截斷 forward 項目、URL 為請求 URL）；history
  導覽以 `tai_session_commit_history` 停在目標 index（依 oracle 問題 4(d) 確認 Python 的
  index 與 history）。`TaiTabSetView.secure` 自然變成 false（錯誤頁的 `page->secure` 為
  false）。要同步檢查：地址草稿清除、Referer（錯誤頁 URL 成為下一次導覽的 referrer，比照
  Python）、書籤可收藏條件、`test_tabset.c` 的 `history_failure_scenarios` 與
  `tabset_integration.py`、`test_tabset_secure.c` 的回滾斷言都要改寫。
- **Pending 時按 Back（決定 1）：** 維持目前行為，不保留被取代的 pending URL；只補測試。
- **地址草稿（問題 8）：** 以 `TaiTabSetView.url` 是否改變作為清除條件，只看 active tab；
  inactive 分頁完成載入不清除。把清除規則集中在 `presentation.c` 一處。
- **Fragment：** 確認 Back/Forward 到 fragment 項目時的請求與 scroll 行為；若 native 目前
  沒有重新 GET 或 scroll 不同，依 oracle 修正。
- 所有 tab set 狀態只在 SDL owner thread 讀寫；新增欄位要更新
  `docs/architecture/native-runtime.md`。

## 測試計畫

- **Oracle：** `tests/history_oracle_probe.py --check` 加入 CTest（參考
  `tests/https_oracle_probe.py`、`tests/tabs_oracle_probe.py` 的結構：barrier 伺服器、
  `run_animation_frame` commit、stdout 導向、port 正規化）。
- **Native 整合：** 新的 `tests/test_tabset_history.c`＋`tests/history_integration.py`，
  使用同一組本機 HTTP fixture（含 barrier、POST 記錄、失敗路徑），逐項比對 fixture 的答案
  或列出的刻意差異。
- **Dummy SDL：** `tests/test_presentation.c`／`tests/test_chrome.c` 增加地址草稿各清除
  情況、Back/Forward 按鈕外觀與點擊、Alt+Left/Right。
- **真實視窗：** `native-window-verification` skill；Back/Forward、fragment、POST 後 Back
  的截圖與伺服器 request log。鍵盤操作用 Xvfb（WSLg 會丟掉合成按鍵）。
- **Sanitizer：** ASan/UBSan＋LSan 跑受影響測試（suppression：`leak:libfontconfig.so`、
  `leak:libcairo.so`）。

## 實作順序與完成條件

1. **Oracle probe 與 fixture。** 完成條件：probe 可重跑、三次輸出相同，涵蓋問題 1–9。
2. **Back/Forward、截斷、pending 與失敗錯誤頁（問題 1–4）。** 完成條件：native 整合測試
   與 oracle 相符（決定 1 除外）；既有回滾測試已改寫為錯誤頁行為；HTTPS 失敗後鎖頭消失。
3. **Fragment 與 POST traversal（問題 5–7）。** 完成條件：請求 method／次數與 scroll 值
   與 oracle 相符。
4. **地址草稿與跨分頁（問題 8–9）。** 完成條件：dummy SDL 覆蓋每種清除情況。
5. **真實視窗與紀錄。** 完成條件：截圖與 request log、完整 CTest、sanitizer；更新
   `PORTING_PLAN.md`、`docs/reference-presentation.md`、`docs/architecture/native-runtime.md`，
   新增 `docs/acceptance/<日期>-history.md` 並在 `ACCEPTANCE.md` 表格加一列。

## 預期的刻意差異

- **Pending 時按 Back（決定 1）：** native 丟掉尚未提交的 pending 項目，Forward 不可用；
  Python 保留它為 forward 項目。
- **Pending 時的頁面：** native 顯示舊頁面與其 scroll；Python 開始導覽時 scroll 已歸零。
- **Alt+Left/Right：** native 有 history 快捷鍵，Python 沒有。

**要移除的既有差異（決定 2）：** `PORTING_PLAN.md` 的「載入失敗與 pending」中「之後的
載入失敗保留舊 page/URL/history」，以及「HTTPS 鎖頭時機」中「回滾到舊 HTTPS 頁面時鎖頭
恢復」。實作後這兩處改為與 Python 相同，並更新 `docs/architecture/native-runtime.md` 的
commit／rollback 描述與 `docs/reference-presentation.md`。

## 決定（2026-09-26，使用者確認）

1. **Pending 時按 Back：** 維持 native 目前做法（不保留被取代的 pending 項目），記為刻意差異。
2. **之後的載入失敗：** 一律顯示錯誤頁，讓使用者知道目前網址是錯的；URL、history 與
   HTTPS 狀態比照 Python（從中間導覽失敗時 forward 項目被截斷）。取代既有的回滾策略。

## 實務經驗（前兩個切片學到的）

- **WSL 時鐘會倒退：** `.o` 的 mtime 可能晚於「現在」，`touch` 無法觸發重建。編輯後直接
  刪除對應的 `.o`（`find build -name '<檔名>.c.o' -delete`），或刪除所有未來時間的物件。
  突變測試前後都要確認物件真的重建。
- **憑證有效期：** 時鐘倒退也會讓剛簽發的憑證「尚未生效」；`tests/https_fixture.py` 已把
  有效期回推兩天。
- **真實視窗：** `window_session.sh` 的 `stop` 只接受執行檔名為 `tai-browser` 的程序；
  WSLg（`:0`）會丟掉合成按鍵，鍵盤驗證用 `--xvfb`。
- **Python oracle 的 stdout：** oracle 的網路執行緒會 `print`，probe 的 `main()` 要整段
  `redirect_stdout`，否則 JSON 會被污染。
- **分頁數量影響 chrome 位置：** 窄視窗時 toolbar 列只在 tab 標籤換行時下移；dummy SDL
  點擊座標要用 `tai_pres_tab_row_wraps()` 或 oracle 位置，不要假設寬度即決定位置。

# Chrome and History work item acceptance — 2026-09-28

`PORTING_PLAN.md`「Chrome 與 History」第 4 項（驗收）。涵蓋同一工作的三個切片：
[兩顆星書籤](2026-09-26-two-star-bookmarks.md)、[HTTPS 鎖頭](2026-09-26-https-lock.md)、
[tab 列換行](2026-09-26-tab-strip-wrap-rows.md)、[History](2026-09-26-history.md)。
工作項目完成；各子系統與整體 browser 維持 `VALIDATING`，不勾選 ACCEPTANCE.md 的整體條件
（新視窗、外部開啟、完整 chrome 視覺比對等仍未完成）。

- **補齊 History 切片留下的缺口：**
  - **地址草稿自動比對：** 新增 `tests/test_history_window.c`。它在 dummy SDL 的真實分頁事件
    迴圈中重放 `history_oracle.json` 的 `address_drafts`（分頁標籤、頁面空白、fragment 連結、
    Back 按鈕都是 SDL 點擊；草稿設定、開分頁、inactive 分頁載入與 active 分頁導覽則和
    Python probe 一樣直接呼叫）。`history_integration.py` 依序執行兩個 native 程式，逐欄比對
    address／focused／dirty，檢查點從 30 個變成 37 個，全部相符。為此新增只供測試使用的
    `TaiPresTabsObserver`（`src/presentation_internal.h`）：observer 在每輪迴圈結束時於 owner
    thread 呼叫；傳 NULL 時，公開的 `tai_present_window_with_tabs` 行為不變。連跑三次都相符。
    突變測試：拿掉「URL 改變丟棄草稿」會讓 fragment_link、active_pending 的 5 個欄位失敗；
    拿掉「點分頁標籤丟棄草稿」會讓 switch_tab 的 2 個欄位失敗。
  - **憑證錯誤後的 history：** Python `Tab.load` 在送出請求前就更新 history，跟錯誤種類無關
    （`tests/reference/browser.py:6067-6092`）；憑證錯誤與網路錯誤只差在錯誤頁內容，
    因此網路失敗那部分已經過 oracle 驗證的 history 語意同樣適用。`test_tabset_secure.c` 新增斷言：
    憑證錯誤頁是一般的 history 項目，Back 回到安全頁面並顯示鎖頭，Forward 重新請求並停在同一 index，
    Forward 不可用。
- **建置與 CTest：** Debug 建置 `-Werror` 無警告；`ctest --test-dir build -j 3` 40/40 通過。
- **ASan/UBSan＋LeakSanitizer（整套）：** `build-asan`（`-DTAI_SANITIZERS=ON`）、
  `ASAN_OPTIONS=detect_leaks=1`、suppression `leak:libfontconfig.so`／`leak:libcairo.so`。
  `-j 3` 下 37/40 通過；`browser_tabs`、`history_integration`、`https_integration` 因平行負載
  逾時（等待子程序輸出，不是 sanitizer 報告），`--rerun-failed` 序列重跑 3/3 通過。log 中沒有任何
  sanitizer 錯誤或 leak summary。修改 `test_history_window.c` 後，`history_integration` 在
  ASan 下又重跑一次並通過。
- **真實視窗（Xvfb `:97`、`tests/tools/window_session.sh`、800×600、`tai-browser --window`，
  另以 127.0.0.1 的 `history_fixture` 提供頁面，data home 為 session 私有目錄）：** 在同一個
  session 內依序操作：
  1. 輸入 /frag；New Tab 開啟預設首頁（外部 `https://browser.engineering/`，畫面上有鎖頭；
     屬於外部網站，不作為可重現的證據）。
  2. 在 Tab 1 輸入 /b，再輸入草稿 `draft`，點 Tab 0：草稿被丟棄，地址列顯示 /frag。
  3. 點「to target」：地址變成 `/frag#target` 並捲到目標，這一步沒有發出請求。
  4. 點收藏星：星星變金色。開書籤清單：`about:bookmarks` 列出 `/frag#target`。
  5. Back：重新 GET /frag 並捲到目標，Forward 變成可用；Forward 回到書籤清單。
  6. 導覽 /fail：顯示 Network Error，地址為 /fail，Forward 停用；Back 回到書籤清單。
  7. 點清單中的收藏連結：GET /frag 並捲到目標，星星為金色。
  8. 切到 Tab 1：顯示 /b，Tab 0 的狀態沒有串到 Tab 1。
  9. resize 到 640×480：版面與捲軸正常。

  Fixture 收到的請求依序為 `GET /a, /frag, /b, /frag, /fail, /frag`，全部是 GET、沒有 body。
  83 個 TEXT_INPUT 的 windowid 都是 SDL 視窗（沒有 windowid=0）；`stop` 回報 `browser exit=0`。
  15 張截圖都已目視檢查，存於本次 session scratchpad 的 `ws/`（暫存目錄，之後可能被清除）。
  HTTPS 鎖頭的可重現真實視窗證據見 HTTPS 切片（`test_tabs_secure_window --window`）。
- **獨立審查（`ownership-reviewer`）：** 檢查 observer 在 NULL 與回傳 false 兩種情況下的資源
  釋放、observer 改動 tab set 後 `view`／`row_wraps`／address watch 是否一致、
  `tai_tabset_select` 有無副作用、兩個子程序之間 fixture gate 狀態是否殘留，以及憑證錯誤斷言與
  oracle 是否一致。沒有發現缺陷。唯一的低風險點是測試以頁面指標是否相同判斷新頁面，理論上
  可能因記憶體被重複使用而誤判；已改成確認 Back 開始時正在載入，再等載入完成。
- **剩餘差異與缺口**（唯一紀錄在 `PORTING_PLAN.md`「已知差異與範圍」）：pending 時按 Back、
  pending 時仍顯示舊頁面與 scroll、Alt+Left/Right、tab 上限與窄寬標籤排版、窄寬地址欄、
  鎖頭時機、書籤控制與跨重啟保存、測試信任根。尚未涵蓋：完整 chrome 截圖比對、新視窗、
  `mailto:` 外部開啟、WSLg／Wayland 合成鍵盤路徑。

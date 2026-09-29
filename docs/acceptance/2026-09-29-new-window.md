# 新視窗（Ctrl+N）驗收（2026-09-29）

**狀態：`VALIDATING`。** 依 [計畫](../new-window-plan.md) 完成步驟 1–6，分支 `new-window`。整體
browser 仍未達 `COMPLETE`（外部開啟、完整 chrome 視覺比對等仍缺，見 `PORTING_PLAN.md`）。

## Python oracle

- `tests/new_window_oracle_probe.py` 以 SDL dummy driver 與 127.0.0.1 fixture
  （`tests/new_window_fixture.py`）對每個情境建立新的 `BrowserApp`，經 `dispatch_event` 送合成
  SDL 事件，凍結 9 個情境到 `tests/fixtures/new_window_oracle.json`：Ctrl+N 基本、原視窗不受影響、
  修飾鍵組合、事件路由、共用書籤、共用 cookie、第三個視窗、關閉單一視窗（含載入中）、SDL_QUIT。
  連續三次 `--check` 相同；已加入 CTest `new_window_oracle_probe`。
- 發現：Python 的 SDL 視窗標題跟隨頁面標題；native 固定 `Tai Gar`，記為既有差異。

## Native 比對與單元測試

- `tests/new_window_integration.py` 驅動 dummy SDL 的 `test_new_window.c`（真實多視窗迴圈），
  23 個檢查點逐欄比對 oracle；只排除視窗標題、Python 的外部首頁網址（D4），並套用 D2
  （key repeat）差異。反向檢查：故意改壞 fixture 的 scroll 與 cookie 值，比對器兩處都報錯。
  另以原生限定情境確認第 11 次 Ctrl+N 被忽略（D1）。
- `tests/browser_app_integration.py` ＋ `test_browser_app.c`：兩個 tab set 共用一個 app 時的完成
  結果路由、分頁互不串線、共用書籤、共用 cookie（fixture 回顯 `Cookie` header）、載入進行中
  （請求已送達 `/delay`）銷毀 tab set、inbox 已有完成結果時銷毀 tab set、之後再建立視窗。

## 建置、CTest 與 sanitizer

- `cmake --build build`（`-Werror`）無警告；完整 `ctest --test-dir build -j 3`：43/43 通過。
- `build-asan`（`-DTAI_SANITIZERS=ON`）整套 43/43 通過，`ASAN_OPTIONS=detect_leaks=1`，LSan
  suppression 僅 `leak:libfontconfig.so`、`leak:libcairo.so`。補上 inbox 情境後
  `browser_app_integration` 再以 ASan＋LSan 重跑通過。
- 過程中發現：SDL3 的 `SDL_Quit` 會重設 hint，`test_new_window` 第二個情境起改用了 X11 driver
  （LSan 報 X11 `GetMonitorInfo` 洩漏）；改為每個情境重新設定 dummy hint 後解決。

## 真實視窗（Xvfb）

在 `unshare -rn` 建立、只有 loopback 的網路命名空間內執行 Xvfb、127.0.0.1 fixture server 與
`tai-browser --window`（WSLg 的 `/tmp/.X11-unix` 唯讀，Xvfb 只有 abstract socket，因此 X 用戶端
也以 `nsenter` 進入同一命名空間）。目的是讓正式版新視窗的 `https://browser.engineering/` 請求
不會連外。fixture：`tests/fixtures/new_window_window/`。

1. 視窗 1 載入 `/index.html`，地址欄輸入 `draft` 後按 Ctrl+N：出現第二個 PID 相符的 `Tai Gar`
   視窗；新視窗顯示 `https://browser.engineering/` 的 Network Error 頁（無法解析主機）。視窗 1
   仍顯示草稿 `draft`。Ctrl+N 的 KEY_DOWN 為 keycode 110、mod=LCTRL、windowid=2，沒有
   `n` 的 TEXT_INPUT。
2. 視窗 2 在地址欄輸入 `/second.html` 並按 Return，接著點收藏星（變金色），滾輪向下 3 次後
   只有視窗 2 捲動。
3. 視窗 1 導覽到 `/second.html`：星號是金色（書籤共用），捲動位置獨立（為 0）。
4. 對視窗 1 送 `WM_DELETE_WINDOW`：只剩視窗 2；在視窗 2 點 Back，回到上一頁，Forward 變為可用。
5. 關閉最後一個視窗：SDL 送出 `SDL_EVENT_QUIT`，browser `rc=0`。另一次 session 在兩個視窗開啟時
   以 SIGTERM 結束，也是 `exit=0`。
6. 事件紀錄：599 筆 key/text 事件，`windowid=0` 為 0 筆，id 只有 2 與 3；TEXT_INPUT 內容為
   `draft` 加上兩次 URL。request log：`GET /index.html`、`GET /second.html` ×2，全部 200。

截圖 01–07 已目視確認，存放於本次 session 的 scratchpad，未加入 repo。`window_session.sh`
新增 `windows`／`await`／`select`／`close`。另外，因為 Xvfb 沒有 window manager，點擊與截圖前
會先把選定視窗提到最上層。

## 獨立審查

`ownership-reviewer` 審查 `edc8659..HEAD` 的 `src`、`include`：沒有 correctness 或記憶體安全
缺陷。審查涵蓋 LoadTask／Completion 的生命週期、loader 與 SDL thread 邊界、部分建構的清理，
以及 close／QUIT 的順序。依其建議補上「inbox 已有完成結果時銷毀 tab set」的測試；其餘建議
情境已由既有測試涵蓋。

## 未涵蓋

- D3 的視窗建立失敗沒有故障注入測試，只經程式碼審查確認。
- 有 window manager 時的焦點切換與 per-window text input，以及 WSLg／Wayland 的鍵盤路徑，
  都未驗證（Xvfb 沒有 WM）。
- 沒有像素比對，截圖只做目視檢查。

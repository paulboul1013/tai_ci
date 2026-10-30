# 慢速網路與 stale callback（2026-10-30）

狀態：VALIDATING。

## 問題

慢速網路下，舊導覽的回應會晚到。瀏覽器不可接受它。generation 機制已存在，但子資源晚到、
亂序回應與快照沒有測試，也沒有證明測試會抓到錯誤。計畫與覆蓋矩陣見
[docs/stale-callback-plan.md](../stale-callback-plan.md)。

## 新增

- `tests/stale_fixture.py`：gate 擋住 `/slow.css`、`/slow.js`、`/slow-a`、`/slow-b`、`/slow-c`、
  `/links`；記錄請求；`wait_served` 等晚到的回應送完。
- `tests/stale_oracle_probe.py` 與 `tests/fixtures/stale_oracle.json`：Python oracle 的 11 個檢查點。
- `tests/test_tabset_stale.c`：`TaiTabSet` driver，跑 G1–G4。
- `tests/stale_integration.py`：G1–G3 逐點比對 oracle；檢查從未請求 `/stale-beacon`；G4 只檢查 native。
- `src/` 沒有修改。

## 證明了什麼

- **G1 CSS 晚到、G2 JS 晚到：** `/b` 在 gate 仍關閉時 commit。release 後 page、history、`<h1>` 都不變。
  晚到的 JS 沒有執行（沒有 `/stale-beacon`）。
- **G3 亂序：** 先放 a、b 時分頁仍在 loading 並顯示原 page；放 c 後 c commit。先放 c 時，之後放 a、b 不改變任何狀態。
- **Oracle：** `url`、`heading`、`history`、`history_index`、`can_go_*`、`requests` 全部相符，
  只有「取代 pending 導覽」的 history 為使用者決定的刻意差異（計畫決定 S4、`PORTING_PLAN.md`）。
- **G4 已造訪快照：** commit 當下連結不是 visited（請求送出時的快照，[visited 計畫](../visited-plan.md) V2，計畫決定 S3）。
  之後重新 style 時，page 的 net 指向 app 的即時集合，連結會變成 visited。
- **G5 測試會變紅（一次性突變，已還原）：**
  - M1：pump 只檢查 `slot`。`stale_integration` 失敗：`expected heading page-b, got start`。
  - M2：`task_publish` 不取消前一個 task。`stale_integration` 失敗：請求多了 `/stale-beacon`。
  - M1 下 `history_integration` 仍通過；其他既有測試沒有在 M1 下執行。
- **G6：** `build-asan`（ASan＋UBSan＋LSan，suppression：`libfontconfig.so`、`libcairo.so`）
  `stale_oracle_probe`、`stale_integration` 通過；`stale_integration --repeat until-fail:20` 20/20 通過。
- **真實視窗（Xvfb，`tests/stale_window_integration.py`）：** `tai-browser --window` 以真實 X11 輸入在地址列輸入 `/b`。
  - W1 CSS 晚到、W2 JS 晚到：標題變成 `page-b`，banner 區域是 B 的綠色；放行並送完晚到的回應後 0.5 秒，
    標題與顏色不變；W2 從未請求 `/stale-beacon`。
  - W3 載入中關閉視窗（`WM_DELETE_WINDOW`）：程式 exit 0，關閉後約 0.46 秒結束；閘門到程式結束後才放行，所以證明關閉不等被擋住的回應。
  - Fixture 的 `wait_served` 只計算放行後送出的回應；閘門逾時（60 秒）不算，晚到檢查不會空洞通過。
  - `build` 連跑 3 次通過；`build-asan`（LSan suppression 另加 `X11_InitModes_XRandR`）通過，log 沒有 sanitizer 報告。
  - 突變：M1 下畫面沒有可見症狀（舊 task 已被取消，completion 沒有 page），只由 stderr 的
    `tab navigation failed: navigation cancelled` 抓到；M2 讓 W2 變紅（請求多了 `/stale-beacon`）。
- **排程可重現：** 既有的 `scheduler`、`raf_window`、`timer_window` 以 `--repeat until-fail:20` 各連跑 20 次通過（2026-10-30）。
- **CTest：** 76/76（`ctest --test-dir build -j4`，含 `stale_window_integration`）。

## 剩餘缺口

- 「tab close」：Python 與 native 都沒有關閉單一分頁的功能，以銷毀 tab set（E4）與關閉視窗（E5、W3）代表
  （使用者 2026-10-30 決定補真實視窗後勾選）。
- 真實視窗只驗 W1–W3；亂序（G3）與快照（G4）只在 `TaiTabSet` 層驗證。沒有在 WSLg 執行。
- 沒有證明 frame 迴圈中哪一刻會重新 style 已提交的頁面（G4 的「之後」）。
- 沒有測試 image 子資源晚到。

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
- **排程可重現：** 既有的 `scheduler`、`raf_window`、`timer_window` 以 `--repeat until-fail:20` 各連跑 20 次通過（2026-10-30）。
- **CTest：** 75/75（`ctest --test-dir build -j4`）。

## 剩餘缺口

- 總驗收條件「Slow network…」**尚未勾選**：Python 與 native 都沒有關閉單一分頁的功能，「tab close」
  只以銷毀 tab set（E4）與關閉視窗（E5）代表；是否據此勾選由使用者決定。
- 只在 `TaiTabSet` 層驗證；沒有在 presentation frame 迴圈或真實視窗驗證晚到的完成。
- 沒有證明 frame 迴圈中哪一刻會重新 style 已提交的頁面（G4 的「之後」）。
- 沒有測試 image 子資源晚到。

# 過期載入完成（stale callback）驗證切片

**狀態：已實作，`VALIDATING`（2026-10-30）。** 證據見 [驗收紀錄](acceptance/2026-10-30-stale-callback.md)。

## 目的

慢速網路下，舊導覽的回應會晚到。瀏覽器不可接受它。本切片證明這件事。

## 機制（已存在，本切片不改）

- Native：`src/tabset.c` 的 `task_create` 給每個導覽 `slot->generation + 1`。
  `task_publish` 把前一個 active task 標為 cancelled。
  `tai_tabset_pump` 只在 `slot->generation == task->generation` 且
  `slot->active_task == task` 時 commit。
- Python oracle：`Tab.load` 遞增 `navigation_generation`。
  `_finish_document_load` 與 `_finish_page_resources` 在 generation 不同時直接返回。

```
請求送出        新導覽            舊回應晚到
   │              │                  │
   ▼              ▼                  ▼
 gen=1 ──────▶ gen=2（gen=1 取消）──▶ gen=1 完成 → 丟棄
                  │
                  └──▶ gen=2 完成 → commit
```

## 檔案

| 檔案 | 用途 |
|---|---|
| `tests/stale_fixture.py` | 127.0.0.1 server。gate 擋住 `/slow.css`、`/slow.js`、`/slow-a`、`/slow-b`、`/slow-c`、`/links`。記錄請求。`wait_served` 等回應送完。 |
| `tests/stale_oracle_probe.py` | 用 Python oracle 跑 G1–G3。產生並以 `--check` 比對 `tests/fixtures/stale_oracle.json`。 |
| `tests/test_tabset_stale.c` | Native driver。用 `TaiTabSet` API 跑 G1–G4。 |
| `tests/stale_integration.py` | 回答 driver 指令。G1–G3 比對 oracle JSON。G4 只檢查 native。 |

順序一律由 gate 決定，不用 sleep。只有「晚到的回應沒有效果」無法等待，
所以 release 並等回應送完後，再觀察 0.5 秒。

## 覆蓋矩陣

| ID | 情境 | 證據 | 狀態 |
|---|---|---|---|
| E1 | Pending 導覽被新導覽取代，舊回應晚到 | [`tests/test_tabset.c:397`](../tests/test_tabset.c#L397) | 既有 |
| E2 | Pending 時按 Back，舊回應晚到 | [`tests/test_tabset_history.c:299`](../tests/test_tabset_history.c#L299) | 既有 |
| E3 | 背景分頁完成，不影響 active 分頁 | [`tests/test_tabset.c:249`](../tests/test_tabset.c#L249) | 既有 |
| E4 | Pending 時銷毀 tab set | [`tests/test_tabset.c:534`](../tests/test_tabset.c#L534) | 既有 |
| E5 | 關閉視窗取消載入期 XHR | [`tests/test_tabset_xhr.c:189`](../tests/test_tabset_xhr.c#L189) | 既有 |
| E6 | 離開頁面時 XHR 與 timer 不再執行 | [`tests/test_tabset_xhr.c:129`](../tests/test_tabset_xhr.c#L129)、[`tests/test_tabset_xhr.c:283`](../tests/test_tabset_xhr.c#L283)、[`tests/test_timer_window.c`](../tests/test_timer_window.c) | 既有 |
| G1 | 文件已到、CSS 被擋時導覽到 `/b` | `test_tabset_stale.c` `subresource_late("css_late")` | 通過 |
| G2 | 文件已到、JS 被擋時導覽到 `/b`；JS 若執行會請求 `/stale-beacon` 並改 `<h1>` | `subresource_late("js_late")` | 通過 |
| G3a | 三個文件都擋住；先放 a、b，再放 c | `out_of_order_oldest_first` | 通過 |
| G3b | 先放 c，再放 a、b | `out_of_order_newest_first` | 通過 |
| G4 | 已造訪快照：請求送出時取快照 | `visited_snapshot`（native only） | 通過 |
| G5 | 一次性 mutation：M1 拿掉 pump 的 generation 檢查、M2 不取消被取代的 task，確認測試失敗 | 見「結果」M1、M2 | 通過（變紅） |
| G6 | ASan 建置，`stale` 測試重複 20 次 | 見「結果」 | 通過 |

## 每個情境檢查什麼

- **G1／G2：** `/b` 在 gate 仍關閉時 commit（沒有 head-of-line blocking）。
  release 後：仍是 `/b`、history 相同、page 指標相同、`<h1>` 仍是 `page-b`。
  G2 另檢查 server 從未收到 `/stale-beacon`。
- **G3a：** 放 a、放 b 後，分頁仍在 loading，仍顯示原本的 `start` page。放 c 後 c commit。
- **G3b：** c commit 後放 a、b，page 指標與 history 不變。
- **與 oracle 比對的欄位：** `url`、`heading`、`history`、`history_index`、
  `can_go_back`、`can_go_forward`、`requests`（正規化路徑）。不比時間。

## 決定（使用者 2026-10-30）

| # | 決定 | 選擇 | 優點 | 缺點 |
|---|---|---|---|---|
| S1 | 範圍 | 方案 A：盤點矩陣，只補 G1–G6 | 新程式碼最少，既有證據不搬動 | 證據分散，要靠本矩陣串起來 |
| S2 | 反向測試 | 只做一次，記在驗收檔，不做成 CTest | 建置不變慢 | 之後的改動不會自動重驗 |
| S3 | 已造訪快照 | 請求送出時取快照（[visited 計畫](visited-plan.md) V2），只記錄不修改 | 不改 `src/` | commit 後重新 style 才會更新 |
| S4 | 取代 pending 導覽的 history | 記為刻意差異：沒有 commit 的頁面不進 history | 比照真實瀏覽器，和「Pending 時按 Back」一致，不改 `src/` | 和 Python 多一條差異 |

結果與證據只記在 [驗收紀錄](acceptance/2026-10-30-stale-callback.md)。

## 指令

```
python3 tests/stale_oracle_probe.py --check
ctest --test-dir build -R stale --output-on-failure
```

重新產生 oracle JSON（只能用 probe）：

```
python3 tests/stale_oracle_probe.py 2>/dev/null > tests/fixtures/stale_oracle.json
```

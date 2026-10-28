# overflow 容器捲動（2026-10-28）

狀態：`VALIDATING`。比照凍結 Python 的 `Tab.click`／`Tab.blur`／`Tab.scroll_by`／
`scroll_focused_element`／`scrollable_ancestor`／`scrollable_layout_for_node`。

## 行為

- 頁面左鍵點擊先清除捲動焦點（落空或被 `preventDefault` 時維持清除）；未被取消時，命中物件
  最內層的可捲動區塊（`overflow:scroll` 加固定 px `height`）取得焦點，巢狀時是內層。候選在
  派送 click 前從當下 layout 決定，listener 之後移除或改樣式不影響選擇。
- Wheel、↑/↓ 與 PageUp/PageDown 都經 `tai_page_scroll_by()`：有焦點且容器仍在 layout 時捲容器，
  夾限於 `[0, content_height − height]`，改變時重建 layout／display list；到底時頁面不接手。
  容器已不在 layout（被移除或不再可捲動）時清除焦點，改捲頁面。
- Chrome 區左鍵與中鍵經 `tai_page_blur_input()` 清除焦點；內容區中鍵不動它。導覽換掉
  `TaiPage`，焦點跟著消失（Python 保留舊節點，下一次捲動時才發現不在 layout，結果相同）。
- 使用者決定（2026-10-28）：wheel 目標是點擊焦點（同 Python，不看游標位置）；PageUp/PageDown
  比照 ↑/↓。刻意差異記在 [PORTING_PLAN.md](../../PORTING_PLAN.md)「快捷鍵與 wheel」列。

介面：`tai_page_scroll_by()`（`include/tai/browser.h`，SDL 擁有者執行緒）；`TaiPage` 借用
`scroll_focus` 節點（D2）。`src/presentation_events.c` 把四個鍵與 wheel 都送到它。

## 證據

| 層級 | 指令／方法 | 結果 |
|---|---|---|
| Python oracle | `python3 tests/overflow_scroll_oracle_probe.py --check`，CTest `overflow_scroll_oracle_probe` | 9 個情境（container、outside、nested、cancelled、gone、flat、fragment、navigation、chrome）凍結於 `tests/fixtures/overflow_scroll_oracle.json`，重跑一致 |
| Native 對 oracle | `ctest -R overflow_scroll_integration`（dummy SDL 分頁迴圈，SDL 左鍵／wheel／↑↓ 事件） | 21 個檢查點的頁面捲動與各容器 `scroll_y` 相符（容差 0.0001px） |
| Native 刻意差異 | 同一測試的 `pagekeys` 情境 | PageDown 讓容器捲 100、再兩次夾在底部，兩次 PageUp 回 0，頁面一直為 0 |
| 負向對照 | 暫時讓容器到底時改捲頁面、不在點擊後設焦點、`tai_page_blur_input` 不清焦點 | 分別在 `container.limit`／`past_limit`／`nested.outer_limit`；`container.*`／`nested.inner`；`chrome.chrome` 如預期失敗 |
| 真實視窗 | `tests/tools/window_session.sh`，Xvfb :98，固定高度 120px、內容 300px 的淺藍 overflow 容器 | 點容器後滾輪下一格→容器 100px、頁面不動；再兩格→夾限 180px、頁面仍 0；↑→80px；點容器外再滾輪→頁面捲 100px、容器維持 80px；回頁頂後點容器按 PageDown→180px；鍵盤事件 `windowid` 為 SDL 視窗；browser exit=0 |
| 回歸 | `hit_differential`、`page_scroll_differential`、`presentation_dummy`、`presentation_chrome` | 通過 |
| 全套 | Debug CTest；`build-asan`（`ASAN_OPTIONS=detect_leaks=1`，suppression `leak:libfontconfig.so`、`leak:libcairo.so`，LSan 確實執行） | 64/64；ASan-UBSan-LSan 64/64 |

## 剩餘缺口

- 第一個分頁的頁面最大捲動與 Python 差 6.48px，記在 [PORTING_PLAN.md](../../PORTING_PLAN.md)
  的「Tab 列換行時的 viewport」列；本切片的 fixture 刻意避開夾限到頁面最大值的檢查點。
- 候選容器堆疊的配置失敗與捲動時重建失敗（還原 `scroll_y`）沒有 malloc 注入測試。
- 未涵蓋：容器內的 input／button、文字輸入框聚焦時按 ↑↓、resize 讓 `max_scroll` 縮小、
  非整數 px 高度；native 沒有觸控路徑（Python 觸控點擊也會設定捲動焦點）。

# 已造訪連結（`:visited`）：實作計畫

**狀態：已實作，`VALIDATING`（2026-10-29）。** V1–V5 依下表完成；Standards、Spec、ownership 審查已處理。

完整管線 E2E（[計畫](e2e-plan.md)）在 `fragment` 與 `back_index` 檢查點發現：native 從不標記
已造訪的連結。`:visited` 選擇器已實作（`src/css.c:279`），但 `src/` 沒有任何地方設定
`TaiNode.visited`（`include/tai/dom.h:22`）。使用者 2026-10-29 決定先補這個缺口，再完成 E2E。

## Python oracle 的行為

```
 BrowserApp.visited_urls = set()          ← 整個 app 共用（所有 tab、所有視窗），只在記憶體
        │
        ├─ Tab.load(url)  開始時 add(str(url))       browser.py:6085
        │    請求的 URL（含 fragment、redirect 之前），載入失敗也算；Back/Forward 也走這裡
        ├─ Tab.navigate_to_fragment  add(新 URL)      browser.py:6388，之後 set_needs_render
        │
        └─ Tab.render → restyle → mark_visited_links  browser.py:6174-6205
             每個 <a href>：url.resolve(href) 在集合中 → is_visited = True
             其他元素：is_visited = False
             只在該 tab 下次 render 時才更新（別的 tab 新增的 URL 不會立刻重畫）
```

## Native 的設計

```
 SDL 執行緒                                         Loader 執行緒
 ──────────                                         ─────────────
 TaiBrowserApp
   └─ TaiVisited（URL 字串集合，app 擁有）
        ▲ add：頁面 commit（含 Back/Forward，錯誤頁除外）、fragment
        │
        ├─ 導覽開始：複製快照（tai_visited_copy） ──▶ LoadTask.visited（loader 獨佔）
        │                                          │
        │                                          ▼
        │                                     page_first_frame：
        │                                       mark_visited(快照) → tai_css_style
        │
        └─ rebuild_dirty_page（SDL 執行緒）：
             mark_visited(TaiVisited 本體) → tai_css_style
             fragment 導覽後呼叫 tai_page_restyle（對應 set_needs_render）
```

- `mark_visited` 放在 `src/browser.c`：走訪 DOM，`tai_url_resolve(page->url, href)` 序列化後查集合，
  每個元素先設 `visited = false`。
- 快照符合 `.claude/rules/runtime-ownership.md`：loader 不讀可變的共用狀態。
- 新的 `TaiVisited` 是不透明 struct，API 只有 create、destroy、add、contains、snapshot。

## 要確認的決定（2026-10-29 使用者：不必完全照 Python，大約符合即可）

| # | 決定 | 建議 | 和 Python 的差別 |
|---|---|---|---|
| V1 | 何時記錄 | 頁面**提交（commit）時**記錄它的請求 URL；跳到 fragment 時記錄新 URL。app 共用、只在記憶體。 | Python 在導覽開始就記錄，失敗也算。native 比照真實瀏覽器，只記成功顯示的頁面。 |
| V2 | 快照方式 | 每次導覽複製一份集合給 LoadTask。 | 無（執行緒規則需要，不是為了 Python）。 |
| V3 | 其他頁面何時更新 | 該頁下次重建時才更新。 | 和 Python 相同，因為這樣最簡單。 |
| V4 | 上限 | 不設上限。 | 相同。 |
| V5 | 證據 | `TaiVisited` 單元測試（含 OOM）、一個 dummy SDL 測試（別的 tab 與新視窗共用）、E2E 轉綠。**不寫**專用的 oracle probe。 | 不逐點比對 Python 的細節。 |

刻意差異 V1 寫入 PORTING_PLAN.md「已知差異與範圍」。

## 步驟

```
 步驟 0  使用者確認 V1–V5
            ▼
 步驟 1  TaiVisited + 單元測試（含 OOM 注入）
            ▼
 步驟 2  接上 app、commit、fragment、loader 快照、mark_visited
            ▼
 步驟 3  dummy SDL 測試（跨 tab、Ctrl+N）＋ e2e_integration 轉綠
            ▼
 步驟 4  ASan、ownership-reviewer 獨立審查、文件
```

## 優點與缺點

- **改善：** 已造訪的連結會換顏色；E2E 可以不帶任何例外通過。
- **優點：** 範圍小；沿用書籤的「SDL 擁有、loader 拿快照」模式；不需要新的 oracle probe。
- **缺點：** 每次導覽多一次集合複製；載入失敗的 URL 不算已造訪（和 Python 不同，但和真實瀏覽器相同）。

# 書籤的兩顆星：實作計畫

**狀態：已實作，`VALIDATING`（2026-09-26）。** 驗證證據見 [ACCEPTANCE.md](../ACCEPTANCE.md)，與 Python 的刻意差異見 [PORTING_PLAN.md](../PORTING_PLAN.md)。 兩個入口共用同一份收藏網址。網址列**內**的星星負責收藏／取消收藏目前網頁；網址列**左側**的獨立星星開啟書籤清單，清單連結以正常 navigation 載入網址。左側入口讓使用者無須記住 `about:bookmarks`。

## 現有依據

- Python `tests/reference/browser.py` 的 `BrowserApp.bookmarks` 是執行期間共用的 `set`。原本單一 toolbar 星星切換收藏；`about:bookmarks` 產生排序、HTML 逸出的連結，`about:blank` 與書籤頁本身不可收藏。
- Native 的 `TaiTabSet` 已擁有 active tab、非同步導覽與 history；`TaiTabSetView` 提供 page/URL 狀態。`src/presentation.c` 在同一個 Cairo chrome texture 繪製 toolbar 與地址列，SDL click 在此分流。`src/network.c` 目前對所有 `about:` 網址回傳空內容，因此直接導向 `about:bookmarks` 尚無清單。

## 使用者操作

| 控制 | 外觀與位置 | 點擊結果 |
|---|---|---|
| 收藏星 | 地址欄**內右側**；未收藏為灰色，已收藏為亮色實心星 | 切換目前已載入網頁的收藏狀態；不導覽、不新增 history entry。不能把地址草稿當成收藏網址。 |
| 書籤入口 | 地址欄**左側的獨立按鈕**，用星星加清單提示與欄內星區分 | 在 active tab 開啟 `about:bookmarks` 清單；點清單中的網址會真的導覽該網址，Back 可回到清單。 |

收藏的鍵是目前 committed page 的完整序列化 URL，所有 tabs 讀取同一份集合；切換 tab、Back/Forward 與 page commit 後重新計算亮／灰狀態。沒有已載入頁、載入 pending、`about:blank` 或 `about:bookmarks` 時，收藏星不可操作。書籤入口可在空清單時打開顯示「No bookmarks yet.」的頁面。兩顆星的 click 先於地址欄命中判斷；地址文字、游標與可見裁切避開欄內星星。一般寬度與現有窄寬斷點都要保留明確、不重疊的命中區。

`about:bookmarks` 保留為可直接輸入的相容網址，但不再是唯一入口。清單由收藏資料快照產生，網址以排序順序顯示，HTML 文字與 `href` 均逸出；點擊沿用 tab 的既有導覽、history、失敗處理與取消規則。收藏資料（`include/tai/bookmarks.h`）由 `TaiTabSet` 擁有，chrome 只透過 view 讀取狀態；清單快照在 SDL thread 複製成 HTML 後才交給 loader，避免兩個 thread 共用可變字串。

## 實作順序與完成條件

1. **固定 oracle。** 用本機兩頁與特殊字元網址記錄 Python 的收藏切換、排序、內部頁 HTML、連結導覽及跨 tab 共用；另固定兩顆星新增的 native UX。完成條件：可重跑 probe 明確區分 Python 既有行為與使用者指定的新控制。
2. **共用收藏資料。** 建立單一 owner、去重/查詢/切換/列舉介面與 deterministic cleanup；驗證完整 URL、空頁與分配失敗。完成條件：元件測試證明 tabs 共用收藏、取消後不再列出，且失敗不留下半筆資料。
3. **網址列內收藏星。** 在 Cairo chrome 加入灰／亮狀態與獨立 hit region，連到 active committed URL；更新文字裁切與游標定位。完成條件：SDL dummy 測試涵蓋切換、跨 tab/history 更新、pending/內部頁 no-op、錯視窗 click 與 1×/2× 座標。
4. **書籤清單。** 讓 `about:bookmarks` 產生安全的內部 HTML，清單的絕對網址連結沿用正常 navigation。完成條件：空清單、排序/逸出、HTTP fixture `GET`、地址欄更新及 Back/Forward 都在整合測試中通過。
5. **左側入口與真實視窗。** 畫出可辨別的獨立星星按鈕，點擊時導向書籤清單；驗證一般與窄寬佈局、地址草稿處理及非空白前後截圖。完成條件：真實視窗可從星星入口開清單、點收藏 URL 到目標頁、返回清單，再取消收藏並見到灰星。

每一項完成後建置並執行其 focused 測試；最後執行相關 CTest、Python oracle、sanitizer 與原生視窗流程。實作證據寫入 [ACCEPTANCE.md](../ACCEPTANCE.md)，新 UX 相對 Python 的刻意差異留在 [PORTING_PLAN.md](../PORTING_PLAN.md)。本計畫的幾何與事件細節由 [presentation 契約](reference-presentation.md) 承接，ownership 由 [native runtime](architecture/native-runtime.md) 承接。

## 已決定：跨重啟保存

使用者於 2026-09-26 選擇「共用且跨重啟保存」。資料層讀取私有檔案，並以暫存檔、fsync、rename 原子寫入；壞檔不阻擋啟動也不覆寫，本次改為只存在記憶體。檔案 I/O 只在點擊處理時同步執行，不在 SDL 繪製路徑。重啟、壞檔、寫入失敗與分配失敗由 `tests/test_bookmarks.c` 與 `tests/test_tabset_bookmarks.c` 覆蓋。

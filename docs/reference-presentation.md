# SDL3 視窗呈現、Chrome 與輸入契約

## SDL3 presentation slice

`tai-browser --window URL` 以 `TaiTabSet` 開啟可調整大小的 SDL3 視窗，建立時要求
800×600 SDL 視窗座標，尺寸包含 tab row 和 toolbar；實際繪製尺寸取自
`SDL_GetWindowSizeInPixels()`，高密度視窗可有更多實體像素。初始文件在視窗建立後非同步載入；SDL event loop
持續處理切換、地址列、resize 和 close。Cairo 先在不透明白底繪製 native-endian premultiplied ARGB32；
因最終 alpha 皆為 255，其數值布局可直接上傳至 SDL `ARGB8888` texture。每次 page raster
呼叫交付獨立擁有的 pixel copy，SDL adapter 在上傳後釋放。Chrome toolbar 由另一個 Cairo
surface 繪製並上傳至獨立 texture；page 與 chrome texture 在 SDL scene 中組合。Headless
`--screenshot` 契約仍是 page-only 800×532px，未變更。

舊 `tai_present_window_with_chrome` 單頁入口的 toolbar bottom/address y 依視窗寬度分段：≥232px 為 68.34/49.072，
128–231px 為 82/62.732，79–127px 為 112/92.732，<79px 為 142/122.732。這些 transition
與 Python `Chrome` probe 相同。單頁入口的 page viewport 高度為 `max(1, window_height - bottom)`；
Headless `--screenshot` 仍是 page-only 800×532，不包含 Chrome。
Forward 控制項在寬度 ≥94px 時位於 (49,36)，低於 94px 時位於 (0,66)，與 Python probe 的
位置一致。Window resize event 若任一維 ≤10px 會忽略並保留上一有效畫面；dummy 測試
覆蓋 0×0、10×10、10×200、200×10 resize no-op。低寬度控件限制
由 [migration plan](../PORTING_PLAN.md) 追蹤；最新視窗檢查及 acceptance evidence 見
[`ACCEPTANCE.md`](../ACCEPTANCE.md)。

Tabbed `--window` 的 tab Chrome bottom/address y 在寬度 ≥232px 時為 74.82/55.552。
Toolbar 各列（Back/Forward、書籤按鈕、地址欄）與 chrome bottom 只在 tab 列換行時下移
20px，而是否換行取決於最後一個 tab 標籤的右緣（`tai_pres_tab_row_wraps()`，用
`tab_link_left/width`）：一個 tab 右緣 84，<84px 換行；兩個 tab 右緣 125，<125px 換行；
≥3 個超寬時使用 native 單行編號格，不換行。因此 120px 時一個 tab 為 118.48/99.212，
兩個 tab 為 138.48/119.212；127px 為 118.48/99.212。換行狀態改變（例如 New Tab）時，
所有 tab 的 page viewport 立即改為新 chrome bottom 以下的高度。
`tests/tab_strip_differential.py` 以 live Python oracle 比對一／兩個 tab、各 active、
17 個寬度的 chrome bottom、地址欄 y 與 Back y（容差 0.15px，見 PORTING_PLAN）。800px 寬、兩個 tabs 時，New Tab button rect 為
`[0,6,30,30]`，Tab 0 link text 為 `[34,19.072,71,35.072]`，Tab 1 為
`[75,19.184,125,35.184]`（Tab 1 作用中）。切到 Tab 0 後，粗體標籤擴大，兩個 link rect
分別為 `[34,19.184,84,35.184]` 與 `[88,19.072,125,35.072]`；Tab 1 的起點隨前一個
標籤寬度移動，`x=75` 仍命中 Tab 0。120px 寬、Tab 1 作用中時，link rectangles 為
`[34,19.072,71,35.072]` 和 `[0,19.184,108,55.184]`；切到 Tab 0 後，
Tab 1 的換行 link 右界延伸到 x=113。Back/Forward 在 800px 的頂端為 42.48，
120px 時為 62.48，讓第二行 tab 文字露出；完整矩形由 frozen fixture 保存。
Chrome fixture 固定左界命中、右界
不命中；active link 標示為粗體黑字，其他 tab 為藍字。New Tab 建立並選取
`https://browser.engineering/`；New Tab 與有效 Tab N 選取都清除 dirty address draft。
完整 oracle 結果在 [`tabs_oracle.json`](../tests/fixtures/tabs_oracle.json)，可用
`python3 tests/tabs_oracle_probe.py --check` 重跑。

使用者指定 native 同時最多 25 個 tabs。至少三個 tabs 且自然文字列超出實體像素寬時，
Chrome 轉成從 x=34 到右緣的等寬 tab 方框；每格寬 `(pixel_width - 34) / tab_count`，
命中區使用同一格的半開 x 範圍與 `[6,30)` y 範圍。方框只顯示 tab 編號，作用中格為
淺底粗體黑字，其他格為藍字。800px 寬、25 個 tabs 時格寬 30.64px，Tab 24 的
命中區為 `[769.36,800)×[6,30)`。第 25 個 tab 建立後 New Tab 按鈕變灰且點擊
不增加 tab，也不清除地址草稿或焦點；TabSet API 再建立會回傳明確的上限錯誤。兩個 tabs 的 Python oracle 幾何
維持上述契約；等寬壓縮與 25 個上限是使用者指定的 native 差異，見
[`PORTING_PLAN.md`](../PORTING_PLAN.md)。極窄視窗可能無法讀出每個編號，仍列為可用性缺口。

Tabbed Chrome 有兩個書籤控制，只存在於 tabbed `--window`，是使用者指定的 native UX。
書籤清單按鈕是 26×24 的獨立方框（灰星加三條清單線），寬度 ≥128px 時位於
(98, Back y)；94–127px 時位於 (0, Back y+30)；79–93px 時位於 (49, Forward y)；
<79px 時位於 (0, Forward y+30)。點擊會丟棄地址草稿，並在 active tab 以一般 navigation
開啟 `about:bookmarks`。收藏星畫在地址欄內右側，中心 (field right−12, field 中線)，
外半徑 7px；未收藏為灰色，已收藏為金色。命中區是地址欄最右 23px，並先於地址欄判定；
地址文字裁切寬度為 field−28，避免文字壓到星星。只有已提交、未 pending、且不是
`about:blank`／`about:bookmarks` 的頁面可切換收藏；不可收藏時點擊只丟棄地址草稿。
Tabbed 地址欄寬度另夾限為不超過視窗寬度，讓 <100px 視窗仍看得到星星；單頁 Chrome
維持 Python 的最小 100px。各寬度下兩個控制與 Back/Forward、地址欄都不重疊。

安全頁面（`TaiTabSetView.secure`）的 tabbed 地址欄依 Python `Chrome`（`SECURITY_ICON_SLOT`
＝30）右移：鎖頭槽為 [address_x, address_x+30)，欄位起點為 address_x+30，自然寬度
在 ≥232px 時為 `max(100, width−180)`、否則 100；地址欄換行斷點不因鎖頭改變。native
再把寬度夾限為 `window−x`（見上段）。唯一來源是 `tai_tabs_address_field()`
（`src/presentation_geometry.c`），繪製、文字裁切（field−28）、游標定位、地址欄命中與欄內
收藏星都用它。oracle 矩形（`tests/fixtures/https_oracle.json`）：800px 安全時欄位
`[162,55.552,782,71.552]`、鎖頭 `[140,56.552,154,70.552]`；不安全時欄位起點 132；
232px 安全時 Python 為 `[162,…,262,…]`、native 夾限到 232；231/120/70px 安全時欄位起點
30、鎖頭 `[8,…,22,…]`。鎖頭是 14×14 黑色外框（鎖身＋尖頂鎖環，線寬 1.8，不填滿），中心
為 (slot_x+15, 欄位中線)。鎖頭槽不是命中區：點擊只丟棄地址草稿，不聚焦地址欄。

Tabbed page viewport 高度使用 `max(1, window_height - tabs_chrome_bottom(width))`；800×600
外框因此為 800×525.18，raster height 向上取整為 526px，page 從 y=74.82 開始。Page layout、
page raster 和 scrollbar geometry 均使用內容區高度；scrollbar thumb 在內容區計算後，呈現時
加回 tab Chrome y origin。

初次取得的 physical pixel size 與寬、高皆大於 10px 的 `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` 都先以新的
內容區尺寸原子地重建 page layout/display list；一般明確 scroll 操作夾住 page scroll，resize
則保留既有 top-level scroll offset，即使超過新 max。然後 raster、upload、present
並替換 page texture；若 page replacement 或 raster/upload/present 失敗，舊 texture 保留。
Chrome state 改變時只重建 chrome texture；page 或 chrome 任一改變都重組 scene。超過單邊
或總像素限制的事件在 reflow 前拒絕。expose 重新呈現現存 textures；quit/close 結束事件迴圈，
依 textures→renderer→window→SDL 順序釋放。寬或高 ≤10px 的 resize 忽略，超過單邊 8192 或
25,000,000 pixels 的 raster 明確失敗。

SDL button 的視窗座標先在 presentation 邊界以實際視窗尺寸與像素尺寸換成實體像素；chrome、
tab 與 page 共用轉換後的座標進行命中測試。只有本視窗且兩軸有限的 button event 會
轉換，無效尺寸或結果不可表示時忽略該次 click。SDL 視窗使用
`SDL_WINDOW_HIGH_PIXEL_DENSITY` 以請求高密度像素緩衝；顯示內容縮放比例不充當座標轉換倍率。
Chrome y 小於 bottom 的 click 由 toolbar 處理；page y 不小於 bottom 的 click 扣除 bottom
一次，再經 `tai_page_activate_viewport` 套用 page-scroll-to-document 轉換。地址列 click
會顯示目前 URL 並依 x 放置游標；在地址列 focus 期間 SDL text input、Backspace、Left/Right
及 Return 編輯/提交地址，其他 key 不送給頁面。page click 會讓地址列失焦但保留 dirty draft；
重新點地址列會繼續編輯該草稿，URL-changing page navigation、Back/Forward 或其他非地址列 toolbar
控制項會丟棄它。Back/Forward 按鈕使用
session history availability 決定外觀與是否 traversal。普通文字以 DuckDuckGo query 導覽，
URL-like 文字直接導覽；`about:blank` 的 query/path 形式保留輸入。地址列未聚焦時，Alt+Left/
Alt+Right 送往 history callback。直接網址的拒絕策略、mailto 外部啟動與其他未實作控制項的
範圍見 [migration plan](../PORTING_PLAN.md)。

同一視窗的 `SDL_EVENT_MOUSE_WHEEL` 與 `SDL_EVENT_KEY_DOWN` 的 `PageUp`/`↑`、
`PageDown`/`↓` 直接透過 `TaiPage` 的既有 page-scroll seam。Frozen Python oracle 的 step
是 100px：normal wheel 正 tick/`PageUp`/`↑` 為 −100，負 tick/`PageDown`/`↓` 為 +100；
flipped wheel 先反向。
Python 將 wheel y 轉為 int，因此 native 對絕對值小於一的 SDL3 float delta 不動。非有限、未知
direction、非本視窗或不支援的 key 都是 no-op。只有 clamped scroll 實際改變才 raster、upload、
present 並在成功後替換 texture；夾限 no-op 保留既有 texture。

每次 texture 呈現（含 expose）均依當前 `TaiPage` content viewport、scroll 與 max scroll，
在 page texture 上方繪製右緣不佔 layout 寬度的 opaque blue scrollbar thumb；Chrome window
模式將它下移 toolbar bottom，沒有垂直 overflow
時不繪製。其寬度為 12px，長度依 frozen Python 的 viewport²/document-height 比例，
最短 20px 並裁切於 viewport；只顯示，不接受點擊或拖曳。此 overlay 不進入 Cairo raster、
display list、headless JSON 或 `--screenshot` PNG。

`tests/test_browser.c` 覆蓋窄 viewport 的文字換行、viewport/scroll 更新與無效尺寸 no-op；
`tests/test_presentation.c` 以 SDL dummy driver 在 owner-thread event filter 注入 resize、wheel、
PageUp/PageDown、↑/↓ 後 quit，驗證 page 使用新 viewport、100px scroll direction、clamp、flipped
direction，以及 fractional、非有限、未知 direction、unrelated window 的 no-op；輸入後的非零
scroll 值也能抓出事件全被忽略的回歸。超限 resize event 不會改寫 page viewport。
同一測試另以 Python 公式固定 thumb 的 top/middle/bottom、最短 20px、窄視窗裁切、
無 overflow 與非有限輸入幾何。
`tests/layout_differential.py` 另以 80px 寬度的換行案例比對 frozen Python/C layout geometry。
`tests/test_presentation.c` 另以 SDL dummy driver 注入 tabbed New Tab、Tab 0/Tab 1 選取、
地址草稿輸入與切換、New Tab 和 tab link 的半開 hit boundaries，驗證 active index、tab count
與 home URL；120px 換行案例驗證 Tab 1 右側命中，同檔的座標換算案例覆蓋
1×/2×、非有限輸入和零尺寸。`test_tabset.c` 與 `tabset_integration.py` 覆蓋 async pending、delayed HTTP/CSS、
pending navigation replacement/late response、Referer、history 與各類失敗回滾。`test_tabs_window.c`
透過真實 tabbed SDL event loop 在 delayed document/CSS 載入期間注入 new-tab/switch input，並在
文件仍 pending 時關閉視窗。這些 dummy-window 案例驗證互動路徑，並不構成 native chrome pixel diff。
`tests/https_integration.py` 以每次產生的測試 CA 與 127.0.0.1 HTTP／HTTPS fixture 驅動
`test_tabs_secure_window.c`：`--geometry` 輸出各斷點欄位／鎖頭矩形並與 oracle 比對；視窗模式在
800/232/120/70px 點擊鎖頭槽（Return 不導覽）、欄內收藏星與右移後的欄位（Return 重新導覽），
以及不安全頁面同一 x（會聚焦）。
`tests/test_chrome.c` 以 SDL dummy driver 注入地址列 focus/edit/Unicode cursor/Return、
Back/Forward toolbar click、toolbar/content click 分界、頁面 y offset、地址列失焦後 page input、
page navigation 後清除舊地址草稿、320×240 一般寬度、200/100px resize、232/231px、128/127px、
94/93px、79/78px 窄列，以及 0×0、10×10、10×200、200×10 resize no-op；另有 60px 高度 resize。
`tests/test_address.c` 覆蓋地址正規化和 `about:blank` query/path。這些案例定義 rendering/presentation
測試範圍；最近一次 build、CTest、sanitizer 和 native-window 驗證結果只記錄於
[`ACCEPTANCE.md`](../ACCEPTANCE.md)。
`--window` 不輸出 JSON，且不得與
`--screenshot` 併用；既有無視窗 JSON/PNG 行為保持原契約。

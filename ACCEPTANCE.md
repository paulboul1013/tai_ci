# C17 Browser 驗收條件

整體狀態：未完成。以下皆是待驗證條件，非已達成聲明。

- [ ] Linux/WSL2 以 CMake + Ninja 建置 C17 程式，無自有 C++ 程式碼；strict warnings 無新增警告。
- [ ] 執行 native browser 不需 Python runtime；Python 僅用於開發 oracle/tests。
- [ ] 使用 SDL3、Cairo、HarfBuzz、FriBidi、FreeType、QuickJS-NG、libcurl multi/OpenSSL、zlib 與適用 image/Unicode stack；替換均有理由與影響紀錄。
- [ ] 同輸入 DOM、CSS、computed style、URL differential 覆蓋正常、空白、Unicode、malformed、priority、inheritance 與已知非標準行為。
- [ ] Layout geometry、text measurements、display ordering、scroll/clip/hit testing 與 reference 可比較；raster 差異有量測與界限。
- [ ] 本機 HTTP fixtures 驗證 GET/POST、redirect、gzip/chunked、cache、cookies、referrer、CSP/CORS 與失敗路徑。
- [ ] JS-visible DOM mutation、query、event propagation/default prevention、XHR 與實際可用 scheduling APIs 通過 reference validation。
- [ ] Navigation→resource loading→script/style→layout→raster→presentation 的 native E2E 通過。
- [ ] Link navigation、form input/checkbox/password/submit、history、fragment、scroll、tabs、windows、bookmarks、view-source 與 resize 有代表性 E2E。
- [ ] Slow network、navigation replacement、tab/window close 不接受 stale callbacks 或 snapshots；priority/frame scheduling 可重現驗證。
- [ ] CTest/cmocka tests 通過，ASan/UBSan 重複 load/mutate/render/close 無 memory error、use-after-free 或 systematic leaks。
- [ ] 獨立 verifier 已 review subsystem contracts、ownership、lifetime 與 integration，可追蹤 findings 均已解決或明確記錄。
- [ ] ARCHITECTURE.md 與 PORTING_PLAN.md 反映實際完成狀態，所有 intentional differences 明列。

目前已增加一個未完成但可驗證的 paint slice：block background/text display commands、單軸
overflow scroll clip/translation、subtree blur、opacity/mix-blend compositing、display-list hit query 與
Cairo PNG 輸出；structural differential 比較 Python/C 的支援 leaves、透明 hit region、Scroll
與 Blend nesting，key-pixel tests 覆蓋 subtree alpha、multiply、difference、destination-in、
sibling isolation 與 rounded clip/scroll effect order，hit differential 比較 paint
order、半開邊界、clip、非零與巢狀 scroll，`tests/test_render.c` 驗證非零 scroll key regions 及 display
list 在來源 DOM/layout 釋放後仍可 raster；page-scroll differential 另驗證 clamp 與單次
viewport→document hit conversion，並以 nested element scroll 交叉檢查 raster/hit。
`tests/test_cli.c` 驗證 `TaiPage`→display list→800×532 page viewport PNG 的成功、尺寸、
opaque background、anchor pixel 與 CLI 失敗路徑。這不
勾選上述完整 paint/raster acceptance；blur 已以 3σ separable Gaussian 的結構與穩定
key-region comparison boundary 驗證，但仍缺互動 scroll input、
一般 `<img>`/remote image/WebP、input/event dispatch、完整 SDL browser orchestration 與 Python
display/raster differential 尚未移植。

不以外網網站可用性作 deterministic acceptance；人工 test.md scenarios 改為本地 fixtures。
Pixel-perfect 只在字型、版本、backend 與環境固定時使用；優先比較 DOM/layout/display 結構。

截至 2026-09-23，原生瀏覽器另有仍屬 `VALIDATING` 的互動、history 與 tabbed window/chrome 切片。Focused presentation/session/browser tests 涵蓋 window input、地址列編輯、history/fragment、GET/POST/Referer 及失敗保留；幾何與 presentation case 清單見 [presentation contract](docs/reference-presentation.md)。

**Prior pre-tabs validation snapshot (2026-09-23; superseded by the tabs results below):** focused CTest `presentation_chrome`、`presentation_dummy`、`browser_address`、`browser_history`、`browser_navigation` 5/5 通過；完整 CTest 28/28 通過（207.97s）。前次與 sanitizer loopback 測試併跑時，`display_differential` subprocess 曾因 `PermissionError` 無法啟動 `build/tai-browser`（檔案 mode 確認為 755）；隔離完整重跑未重現，推測是併行造成的暫時執行限制，根因未確認。ASan/UBSan focused 5/5：`presentation_dummy`、`browser_history`、`presentation_chrome` 在 sandbox 內通過，`browser_address` 與 `browser_navigation` 在允許 loopback 的 elevated 執行下通過；均設 `ASAN_OPTIONS=detect_leaks=0`，LeakSanitizer 未執行。`presentation_chrome` 覆蓋 toolbar 寬度切換與小尺寸 resize 邊界；完整案例與幾何見 [presentation contract](docs/reference-presentation.md)。800×600 原生視窗截圖 `/tmp/tai-chrome-window.png` 已擷取並目視確認 toolbar 與 page content 可見，尚未做 Python/native pixel diff；dummy SDL 狀態測試及目視檢查均不構成 raster parity 證據。此舊 snapshot 不勾選任何整體 acceptance 條件。

使用者於 2026-09-23 手動開啟 `https://browser.engineering/`，回報原生視窗中的基本 chrome／導覽測試成功。具體操作序列未記錄，因此此回報補充代表性手動 smoke evidence，不替代逐項自動化、Python 行為比對或 pixel diff。

已知刻意差異、目前移植切片與剩餘功能缺口的唯一紀錄見 [PORTING_PLAN.md](PORTING_PLAN.md)。SDL dummy automation 涵蓋程式輸入、地址列、history 和 tabs 操作；真實 X11/Wayland 鍵盤輸入與事件順序仍未自動化驗證。

## Tabs slice verification — 2026-09-23

目前 tabbed window 切片仍為 `VALIDATING`，不勾選任何整體 acceptance 條件。`cmake --build build -j 4` 成功；更新後完整 CTest `ctest --test-dir build --output-on-failure -j 4` 通過 30/30（54.25s）；`python3 tests/tabs_oracle_probe.py --check` 與凍結的 Python tabs oracle fixture 相符。

Tabs local HTTP integration 覆蓋 delayed document/CSS 期間 New Tab 與 tab selection、SDL resize event、window close request；tabset cases 覆蓋初次與後續載入失敗、Back/Forward rollback、superseded response、late completion cancellation 與 per-route Referer。Dummy SDL presentation inputs 以後續提交 URL 驗證 New Tab 和 tab switch 會丟棄 dirty address draft。這些測試使用 local fixture，不依賴外網站點。

Focused ASan/UBSan CTest `presentation_dummy`、`presentation_chrome`、`browser_tabs`、`browser_history`、`browser_headless` 5/5 通過。設 `ASAN_OPTIONS=detect_leaks=0`；LeakSanitizer 未執行。

此 2026-09-23 snapshot 當時仍缺低寬度／大量 tabs 的 chrome clipping 驗證、兩個以上 tabs 的 native active/inactive 樣式 pixel comparison、真實 X11/Wayland 輸入事件順序，以及 allocation failure injection。SDL dummy window assertions 不能取代原生桌面 pixel comparison；此切片不宣稱 visual parity。

## Tabs real-window acceptance pass — 2026-09-25

在 WSLg X11（`DISPLAY=:0`、`WAYLAND_DISPLAY=wayland-0`）以
`SDL_VIDEO_X11_XINPUT2=0` 啟動 `tai-browser --window http://127.0.0.1:8765/index.html`。
本機固定頁面現存於 `tests/fixtures/tabs_window/`；重現時用
`python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/fixtures/tabs_window`
供應。使用 `xdotool search --name '^Tai Gar$'` 取得本次視窗 ID；以下 ID `8388661`
僅屬本次執行，不可沿用。截圖均以 screenshot skill 的 `--window-id` 擷取，並用
`xwd -silent -id 8388661` 核對窄視窗完整尺寸，未擷取桌面。字型
`fc-match 'Times New Roman'` 為 Liberation Serif Regular；SDL 從程式的
`SDL_GetWindowSizeInPixels()` 初始查詢及 `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`
事件更新實體像素尺寸。

可重現的 OS 滑鼠事件序列：在 800×600 視窗依序點 New Tab `(15,18)`、Tab 0
`(50,27)`、本機頁面連結 `(64,122)`、Back `(20,48)`、Forward `(70,48)`、Tab 1
`(100,27)`；作用中 Tab 0 時點 `(75,27)` 保持 Tab 0，點 `(100,27)` 切到 Tab 1。
上述每步均用 `xdotool mousemove --window <本次 ID> X Y click --window <本次 ID> 1`
送往指定視窗。可見 URL／頁面順序為本機 `index.html` → `second.html` →
`index.html` → `second.html`，切到 Tab 1 後仍保有其 `https://browser.engineering/`
URL。外網站點內容僅為 New Tab smoke，不作 deterministic acceptance。

兩分頁 native 截圖：[`wide-tab0.png`](/tmp/tai-tabs-acceptance/wide-tab0.png)、
[`wide-tab1.png`](/tmp/tai-tabs-acceptance/wide-tab1.png)、
[`link-second.png`](/tmp/tai-tabs-acceptance/link-second.png)、
[`back-first.png`](/tmp/tai-tabs-acceptance/back-first.png)、
[`forward-second.png`](/tmp/tai-tabs-acceptance/forward-second.png)。800×600 擷取在
`x=799` 的 y=74 是 toolbar 邊，y=75 為白色頁面；與 oracle 的 74.82 相符。
Tab 0 作用中圖的 `[34,84)×[18,35)` 有 159 個黑色、20 個藍色像素，
`[88,125)×[18,35)` 有 0 個黑色、108 個藍色像素；Tab 1 作用中圖對應的
`[34,71)` 區域有 0 個黑色、118 個藍色，`[75,125)` 有 143 個黑色、16 個藍色。
此處黑色定義為 RGB 每通道 <90，藍色為 B > 1.5R、B > 1.5G 且 B >100。
這是受控字型下的 key-region 樣式與邊界證據，不是完整 Python/native pixel diff。

以 `xdotool windowsize <本次 ID> 120 1200` 測窄寬；WSLg 實際提供 120×786
實體像素，`xwd` 與 screenshot helper 尺寸一致。截圖
[`narrow-tab1.png`](/tmp/tai-tabs-acceptance/narrow-tab1.png) 和
[`narrow-tab0.png`](/tmp/tai-tabs-acceptance/narrow-tab0.png) 顯示標籤與地址裁切；
`x=110` 的 y=138 是 chrome，y=139 是白色頁面，與 120px oracle bottom 138.48
相符；點 Tab 0 `(50,27)` 後恢復該 tab 的本機頁面／URL。oracle 的 120×1200
參考高度未由此視窗管理器提供，因此窄寬僅比較寬度相關 chrome 幾何和 key regions。

本次在 C 中修正 Tab 0 啟用後 Tab 1 仍固定從 x=75 開始、造成文字重疊及錯誤命中；
Python oracle 顯示 Tab 1 應移到 x=88。新增 active-first oracle 幾何和 dummy SDL
回歸案例，分別驗證 x=75 留在 Tab 0、x=88 切到 Tab 1。修正後 `cmake --build build --target tai-browser test_presentation -j 4`
成功；`ctest --test-dir build -R '^(presentation_dummy|presentation_chrome|browser_tabs|tabs_oracle_probe)$' --output-on-failure -j 2`
通過 4/4。修正前新增的 dummy 案例確實失敗，修正後通過。
`cmake --build build-asan --target test_presentation -j 4` 成功；
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^presentation_dummy$' --output-on-failure`
通過 1/1。未設定此選項的 LeakSanitizer 在 Fontconfig/Cairo 字型快取回報 23 筆、1771 bytes；
目前不能由此宣稱無洩漏，ASan/UBSan 的通過也不代替 leak 驗證。

剩餘缺口：`xdotool` 指定視窗的合成鍵盤輸入沒有改變地址列，
`windowactivate --sync` 查詢焦點回報 `XGetWindowProperty[_NET_ACTIVE_WINDOW] failed`；
故真實 OS 鍵盤／地址輸入的事件順序仍未驗證，dummy SDL 只覆蓋其程式事件語義。
120px 時第二個 tab 的換行文字受 toolbar 覆蓋，三個以上 tab 的窄寬命中與視覺裁切、
高像素密度下的滑鼠座標轉換、完整 Python/native chrome pixel diff 及 allocation failure
仍待驗證。Tabs 切片維持 `VALIDATING`，整體 acceptance 勾選狀態不變。

## 25-tab native chrome — 2026-09-25

使用者在修正前的 800×600 真實視窗展示第 17 個分頁已貼住右緣：
[`pre-compact-17.png`](/tmp/tai-tabs-acceptance/pre-compact-17.png)。因舊 UI 只向右堆放文字，
後續 tabs 雖存在卻超出視窗。依使用者要求，native 在自然 tab row 超過視窗且至少三個
tabs 時，改繪製等寬編號方框；所有方框共用同一條 y=6..30 命中列。TabSet 限制為
最多 25 個 tabs；第 26 次 New Tab 不建新 session/載入，停用按鈕點擊保留地址草稿與焦點。
這些是刻意與無上限 Python chrome 不同的產品行為，記錄於 `PORTING_PLAN.md`。

修正後以 `cmake --build build --target tai-browser test_presentation test_tabset -j 4`
建置，`ctest --test-dir build -R '^(presentation_dummy|presentation_chrome|browser_tabs|tabs_oracle_probe)$' --output-on-failure -j 2`
通過 4/4。新增的 TabSet 測試先觀察到第 26 次建立錯誤，再驗證總數與 active index
不變；dummy SDL 測試覆蓋第 25 個、額外的停用按鈕點擊、首／中／末格命中，及停用
按鈕不清掉編輯中的地址。兩項測試均先在修正前失敗，修正後通過。
`cmake --build build-asan --target test_presentation test_tabset -j 4` 與
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^(presentation_dummy|browser_tabs)$' --output-on-failure -j 2`
通過 2/2；本次仍未驗證 LeakSanitizer。

新 binary 的另一個 X11 視窗（本次 ID `12582965`，不沿用）在 800×600 顯示
0–24 共 25 個格子，第 25 次額外 New Tab 點擊沒有新增格子：
[`compact-25-active24.png`](/tmp/tai-tabs-acceptance/compact-25-active24.png)。
用 `xdotool mousemove --window <ID> 49 18 click --window <ID> 1` 選到最左格後，
本機頁面與地址回來：[`compact-25-active0.png`](/tmp/tai-tabs-acceptance/compact-25-active0.png)；
點 `(784,18)` 回到 Tab 24，確認右端格也可由 OS 輸入命中。這個新視窗保留供手動檢查，
再用 `xdotool windowsize <本次 ID> 1200 600` 放寬後 25 個標籤切回一般文字列
[`compact-25-wide1200.png`](/tmp/tai-tabs-acceptance/compact-25-wide1200.png)；還原
800×600 後全部重回編號格
[`compact-25-restored800.png`](/tmp/tai-tabs-acceptance/compact-25-restored800.png)，
Tab 24 仍作用中。測試視窗已以最新 binary 重開並保留供手動檢查；X11 ID 可在重開時重用，
須每次重新搜尋。極窄視窗中每格可能小到無法顯示編號；完整
Python/native pixel diff 與 OS 鍵盤輸入順序仍是獨立缺口，tabs slice 維持 `VALIDATING`。

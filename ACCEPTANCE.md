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

## Pointer pixel coordinates and follow-up validation — 2026-09-25

SDL3 的 button event 使用視窗座標；presentation 的 chrome、page layout 和 SDL renderer
使用實體像素。依 [SDL high-DPI guide](https://wiki.libsdl.org/SDL3/README-highdpi)，
兩條視窗路徑現在於 button event 進入時量測 `SDL_GetWindowSize()` 與
`SDL_GetWindowSizeInPixels()`，轉換一次後才分流 chrome/page/tab 命中，並以
`SDL_WINDOW_HIGH_PIXEL_DENSITY` 請求高密度緩衝。`tests/test_presentation.c` 的
2×/1×/非有限值/無效尺寸案例在實作前因缺少轉換函式無法連結，實作後通過。

另以從 Ubuntu 套件解出到 `/tmp/tai-weston` 的 Weston 13 headless compositor 建立
`--scale=2` 隔離 Wayland socket，執行
`TAI_PRESENTATION_HIDPI_PROBE=1 ./build/test_presentation`。SDL 回報 logical
300×100、physical 600×200；注入視窗座標 `(7.5,9)` 建立 New Tab，再以 `(20,13)`
命中實體座標 `(40,26)` 的 Tab 0，結果 `tabs=2 active=0`，程式 exit 0。
舊未轉換路徑的第二下會落在 New Tab 區；此探針驗證實際 2× SDL event→presentation→
tab selection 邊界，不是原生視窗截圖。

更新後 `cmake --build build -j 4` 成功；完整 CTest 30/30 通過，Python
`tests/tabs_oracle_probe.py --check` 與凍結 fixture 相符。`build-asan` focused
`presentation_dummy`、`presentation_chrome`、`browser_tabs` 3/3 通過，設定
`ASAN_OPTIONS=detect_leaks=0`；此結果仍不代表 LeakSanitizer 通過，先前
Fontconfig/Cairo 報告與 tab-set allocation failure injection 缺口未關閉。

本次 X11 視窗的 800×600 兩分頁截圖
[`hidpi-fix-two-tabs-800.png`](/tmp/tai-tabs-acceptance/hidpi-fix-two-tabs-800.png)
顯示 active Tab 1；120×600 的
[`active1.png`](/tmp/tai-tabs-acceptance/hidpi-fix-two-tabs-120-active1.png)、
[`active0.png`](/tmp/tai-tabs-acceptance/hidpi-fix-two-tabs-120-active0.png)
顯示 Tab 0 可切回本機頁面，但窄寬文字裁切仍存在。800px 的 25 格
[`active24.png`](/tmp/tai-tabs-acceptance/hidpi-fix-25-active24.png)、
[`active0.png`](/tmp/tai-tabs-acceptance/hidpi-fix-25-active0.png)、
[`active12.png`](/tmp/tai-tabs-acceptance/hidpi-fix-25-active12.png)
分別核對末、首、中格；放寬至 1200px 後
[`wide1200.png`](/tmp/tai-tabs-acceptance/hidpi-fix-25-wide1200.png)
切回一般文字列。120px 截圖在 x=110 的 y=138 為 chrome、y=139 為白色頁面，
符合 frozen oracle 的 bottom 138.48；這些是 key-region／命中證據，仍非完整 pixel diff。

本次再次嘗試 `xdotool windowfocus`，但 `xdotool getwindowfocus` 回報
`xdo_get_focused_window_sane failed`。針對地址列的 `xdotool type` 命令成功結束，
截圖中沒有輸入字串；因此真實 X11 鍵盤／地址事件交付仍未驗證，不以此修改
`windowID=0` 的輸入規則。切片保持 `VALIDATING`，整體 acceptance 勾選狀態不變。

## Chrome comparison, narrow layout and resource probes — 2026-09-25

以 frozen Python `Chrome.paint()` 的 display list 離屏輸出固定兩分頁 chrome，和相同
800px／120px 狀態的 X11 `Tai Gar` 視窗截圖比較。比較範圍只含 chrome；Python Skia
與 native Cairo 的字型光柵化不同，因此全像素差異率是診斷訊號，驗收以控制項、幾何與
key regions 為主。修正前 800×75 active Tab 1 有 18,568／60,000 不同像素；
120×139 active Tab 0／1 分別有 8,889／9,245 不同像素。背景純色區域一致，
但 Python 有橘色按鈕、書籤與 HTTPS 鎖頭，native 對應外觀仍缺；這些明確差異由
`PORTING_PLAN.md` 追蹤。Python 的 Back/Forward 在 800px 從 y=42.48、120px
從 y=62.48 開始；native 原先都從 y=36 開始，蓋住 120px 第二行 Tab 1 文字。

以 Python oracle 新增的 Back/Forward/address 矩形凍結至
[`tabs_oracle.json`](tests/fixtures/tabs_oracle.json)。Native tabbed chrome 現在使用對應的
toolbar/address y，兩分頁在 125px 寬改為單行，120px 的 inactive Tab 1 命中右界為
x=113。新增 120px dummy SDL 點擊案例在修正前失敗、修正後通過。修正後 120px
active Tab 0 的第二行藍字由 0 增為 17 像素（同區 Python 21 像素）；
[`active0.png`](/tmp/tai-tabs-acceptance/narrow-overlap-fixed-active0.png)
與 [`active1.png`](/tmp/tai-tabs-acceptance/narrow-overlap-fixed-active1.png)
顯示真實視窗在 `(110,45)` 可選取 Tab 1。127px
[`unwrapped.png`](/tmp/tai-tabs-acceptance/narrow-127-unwrapped.png)
的 x=110 欄 y=118 為 chrome、y=119 為 page，對應 Python bottom 118.48。
修正後與 Python 的不同像素降為 800px 10,994／60,000、120px active0
6,533／16,680、active1 6,961／16,680；800px
[`active1.png`](/tmp/tai-tabs-acceptance/chrome-geometry-fixed-800-active1.png)
仍可見未實作的視覺控制項。這不宣稱完整 chrome parity。

LeakSanitizer 在 sandbox 外以 Fontconfig suppression `leak:libfontconfig.so`
執行 focused `test_chrome`、`test_presentation` 和
`tests/tabset_integration.py build-asan/test_tabset build-asan/test_tabs_window`，
均 exit 0，沒有未抑制的洩漏報告。分別抑制 Fontconfig 13 筆／1035 bytes、
23 筆／1771 bytes、23 筆／1751 bytes；這只支持上述路徑沒有被 LSan 偵測到的
專案洩漏，Fontconfig/Cairo 全域快取仍需獨立清理策略。另以 `/tmp` 的 linker
`--wrap=malloc/calloc/realloc` harness 掃描 0–79 的配置失敗 budget，覆蓋
TabSet create/start/new-tab 240 個主執行緒建構／立即關閉序列；失敗後 retry 和 tab
數量不變的斷言通過，ASan 與上述 LSan suppression 都無其他報告。loader thread 的
配置故障與完成提交尚未注入，切片繼續 `VALIDATING`。

## Keyboard delivery, loader-thread allocation failure and QuickJS OOM — 2026-09-25

以 `SDL_EVENT_LOGGING=1` 啟動 `tai-browser --window` 記錄 SDL 實收事件。WSLg `:0`
上點擊地址列後，`xdotool key --window` 產生 `windowid=0` 的 KEY_DOWN 且沒有
TEXT_INPUT；`xdotool windowfocus` 使 SDL 在約 0.8ms 內先 FOCUS_GAINED 再
FOCUS_LOST，其後 XTEST 按鍵沒有任何 SDL 事件。WSLg compositor 會撤回 X11 焦點，
此路徑仍無法作為鍵盤證據，瀏覽器維持忽略 `windowID=0`。

改在無 window manager 的隔離 `Xvfb :99` 執行同一 binary 與 fixture，XTEST 按鍵經
X server 焦點分派。點擊地址欄右端、80 次 BackSpace、輸入
`http://127.0.0.1:8765/second.html` 後按 Return：267 個鍵盤／文字事件全為
`windowid=2`，`windowid=0` 為 0，TEXT_INPUT 33 筆與 URL 長度一致，順序為每鍵
KEY_DOWN→TEXT_INPUT→KEY_UP；server 記錄 `GET /second.html 200`，截圖
[`xvfb-keyboard-address.png`](/tmp/tai-tabs-acceptance/xvfb-keyboard-address.png)
依序顯示清空、輸入與 Second Page，事件記錄在
[`xvfb-keyboard-events.txt`](/tmp/tai-tabs-acceptance/xvfb-keyboard-events.txt)。先前一次
點在 URL 中段再按 Ctrl+A 的嘗試送出 `/indhttp://…second.htmlx.html`；frozen Python
同樣以點擊 x 決定游標且不處理 Ctrl+A，因此這是等價行為，不是缺陷。這驗證真實 X11
server→SDL→地址列的鍵盤順序；WSLg／Wayland compositor 的焦點交付仍未驗證。

loader thread 配置故障以 linker `--wrap=malloc,calloc,realloc,pthread_create` harness
注入：只標記 TabSet loader thread，從第 i 次配置起持續失敗，涵蓋專案與 QuickJS 靜態碼，
不含 curl/Cairo/Fontconfig 共享函式庫。每個 i 檢查 pending 會結束、失敗時保留原頁與
history、成功時 history 前進，再以同 tab 無故障導覽恢復。第 190 點在
`JS_NewContextRaw()` 的 `class_proto` 配置失敗時，QuickJS-NG `df836d1`（上游 master
`19dbe85` 仍相同）已 `add_gc_object()` 卻直接 `js_free_rt(ctx)`，其後
`JS_FreeRuntime()` 走訪已釋放 GC 節點，ASan 回報 SEGV。這是可重現的依賴端
use-after-free。以 scratch 複本在釋放前加 `remove_gc_object(&ctx->header)` 後，
`data:` 導覽／初始載入分別掃過 444／396 點、本機 HTTP 分別掃過 791／835 點至無故障
完成，ASan/UBSan/LSan（僅 Fontconfig suppression）全部 exit 0。

此修補已成為 tracked
[`patches/quickjs/0001`](patches/quickjs/0001-unlink-context-on-class-proto-oom.patch)，
CMake configure 時套用至 `deps/quickjs`，已存在則略過，無法套用則停止；無自身
`.git` 的 checkout 也以 `GIT_CEILING_DIRECTORIES` 驗證可套用與偵測。新增兩個 CTest：
`quickjs_oom` 以毒化並延後歸還的自訂 QuickJS allocator 逐點失敗 `JS_NewContext()`，
未修補的 libqjs 在 `gc_decref` 斷言 abort，修補後 57 點通過；
`tabset_loader_oom` 為上述 `data:` loader-thread 掃描（本機 HTTP 掃描未納入 CTest）。
`build` 完整 CTest 32/32 通過；`build-asan` 兩個新測試在 ASan/UBSan/LSan
（Fontconfig suppression）下通過。上游 quickjs-ng 尚未回報。

code review 指出 button 座標以即時視窗尺寸換算、命中以已處理尺寸判斷。換算比例是
pixel density，resize 不改變它；命中使用使用者點擊當下已呈現的版面。剩餘風險只在
display scale 改變與 click 同時排隊，未另建自動測試。

## WSLg manual keyboard delivery — 2026-09-25

WSLg `:0` 上以 `SDL_EVENT_LOGGING=1 SDL_VIDEO_X11_XINPUT2=0` 啟動 `tai-browser --window`
本機 fixture，由使用者以實體鍵盤操作：點擊地址欄右端、BackSpace 編輯、輸入
`.htmX`、修正，最後清空並輸入 `http://127.0.0.1:8765/second.html` 後按 Enter。SDL
記錄在點擊前已 FOCUS_GAINED；266 個 KEY_DOWN／KEY_UP／TEXT_INPUT 全為 SDL
`windowid=2`，無 `windowid=0`；每筆 TEXT_INPUT 都緊接在其 KEY_DOWN 後，含 61 次
BackSpace 與 1 次 Enter，文字依序為 `.htmX`、`l`、`second` 與完整 URL。Enter 後
server 記錄 `GET /second.html 200`，截圖
[`wslg-manual-keyboard-after.png`](/tmp/tai-tabs-acceptance/wslg-manual-keyboard-after.png)
顯示地址欄與 Second Page，事件見
[`wslg-manual-keyboard-events.txt`](/tmp/tai-tabs-acceptance/wslg-manual-keyboard-events.txt)。
FOCUS_LOST 出現在 Enter 之後，對應使用者切回其他視窗。這驗證 WSLg compositor→
X11→SDL→地址列的實體鍵盤交付；自動注入仍受 WSLg 焦點撤回限制，Wayland 原生
backend 未驗證。

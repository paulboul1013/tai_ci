# Tabs real-window acceptance pass — 2026-09-25

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

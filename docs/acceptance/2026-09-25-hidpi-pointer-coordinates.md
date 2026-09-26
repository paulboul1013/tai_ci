# Pointer pixel coordinates and follow-up validation — 2026-09-25

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

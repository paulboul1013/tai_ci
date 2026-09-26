# Tabs slice verification — 2026-09-23

目前 tabbed window 切片仍為 `VALIDATING`，不勾選任何整體 acceptance 條件。`cmake --build build -j 4` 成功；更新後完整 CTest `ctest --test-dir build --output-on-failure -j 4` 通過 30/30（54.25s）；`python3 tests/tabs_oracle_probe.py --check` 與凍結的 Python tabs oracle fixture 相符。

Tabs local HTTP integration 覆蓋 delayed document/CSS 期間 New Tab 與 tab selection、SDL resize event、window close request；tabset cases 覆蓋初次與後續載入失敗、Back/Forward rollback、superseded response、late completion cancellation 與 per-route Referer。Dummy SDL presentation inputs 以後續提交 URL 驗證 New Tab 和 tab switch 會丟棄 dirty address draft。這些測試使用 local fixture，不依賴外網站點。

Focused ASan/UBSan CTest `presentation_dummy`、`presentation_chrome`、`browser_tabs`、`browser_history`、`browser_headless` 5/5 通過。設 `ASAN_OPTIONS=detect_leaks=0`；LeakSanitizer 未執行。

此 2026-09-23 snapshot 當時仍缺低寬度／大量 tabs 的 chrome clipping 驗證、兩個以上 tabs 的 native active/inactive 樣式 pixel comparison、真實 X11/Wayland 輸入事件順序，以及 allocation failure injection。SDL dummy window assertions 不能取代原生桌面 pixel comparison；此切片不宣稱 visual parity。

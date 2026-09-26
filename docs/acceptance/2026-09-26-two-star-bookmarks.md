# Two-star bookmarks slice — 2026-09-26

切片狀態 `VALIDATING`；不勾選整體 acceptance 條件（bookmarks E2E 條目仍需與 windows、
view-source 等其他項目一併達成）。設計與決定見 [書籤計畫](../bookmarks-plan.md)，
刻意差異見 [PORTING_PLAN.md](../../PORTING_PLAN.md)。

**建置注意：** 本機曾兩度出現原始碼 mtime 早於既有 `.o`，Ninja 因此沿用過期物件
（`tabset.c.o` 缺少持久化呼叫）。以下結果是在 `touch` 全部專案原始碼後重建
`build/` 與 `build-asan/`，並以 `objdump`／字串比對確認產物含最終程式碼後取得。

- 完整 CTest `ctest --test-dir build -j 3`：35/35 通過。新增 `bookmarks`、
  `bookmarks_integration`、`bookmarks_oracle_probe`。
- `python3 tests/bookmarks_oracle_probe.py --check` 與凍結 fixture 相符。
- `bookmarks_integration`（本機 HTTP）：跨 tab 共用、pending／`about:blank`／
  `about:bookmarks` 不可收藏、排序與逸出、點擊清單連結以原 URL 送出 GET（不含
  `&amp;`）、Back/Forward，以及透過 `tai_tabset_create` 的重啟讀回、取消後持久化、
  壞檔不阻擋啟動且不被覆寫。
- ASan/UBSan **含 LeakSanitizer**（`ASAN_OPTIONS=detect_leaks=1`，先以刻意洩漏程式確認
  LSan 可執行）：`bookmarks`、`bookmarks_integration`、`browser_headless`、
  `browser_history`、`presentation_dummy`、`presentation_chrome`、`browser_tabs`、
  `tabset_loader_oom` 8/8 通過。SDL 視窗測試的 LSan 報告只剩 Cairo toy font／fontconfig
  全域快取（`cairo_select_font_face`→`FcPatternDuplicate`，未涉及書籤的
  `presentation_chrome` 也相同），以 `leak:libfontconfig.so`、`leak:libcairo.so`
  suppression 排除；排除後沒有專案程式碼的洩漏。
- 真實視窗：隔離 `Xvfb :99`、`SDL_VIDEO_X11_XINPUT2=0`、fixture
  `python3 -m http.server 8766 --bind 127.0.0.1 --directory tests/fixtures/bookmarks_window`、
  暫存 `XDG_DATA_HOME`。點欄內星 (769,63)：灰→金，檔案目錄 0700、檔案 0600，內容為
  magic 加一筆 URL。點左側按鈕 (110,54) 開啟清單，點清單連結後 server 收到
  `GET /index.html`，Back 回到清單且 Forward 啟用。SIGTERM 正常結束後以同一目錄重啟，
  星星仍為金色；取消後檔案筆數為 0，清單顯示「No bookmarks yet.」，沒有殘留暫存檔。
  200／110／85／70px 寬度下兩顆星與 Back/Forward、地址欄都不重疊。最終 binary 另以
  壞檔啟動：stderr 警告、可在記憶體中收藏、原檔不變。截圖存於
  `/tmp/tai-bookmarks-acceptance-2026-09-26/`（已目視檢查；`/tmp` 可能被重置）。
  在沒有視窗管理器的 Xvfb 上，`xdotool windowclose` 會直接銷毀視窗，SDL 收到
  BadWindow 後以 1 結束，因此這裡改用對已核對 PID 送 SIGTERM。
- 獨立 reviewer 指出的問題中，壞檔或缺少 HOME 時拒絕啟動、單頁 Chrome 窄寬改變，
  以及未記錄的刻意差異已修正或寫入紀錄。多 process 後寫者覆蓋與暫存檔殘留列為已知
  限制。
- 尚未涵蓋：79–93、<79、125–127、128–231px 的 dummy SDL 命中測試（只做了真實視窗
  目視）、長地址文字接近星星時的游標位置，以及 WSLg／Wayland 下的書籤點擊。

# HTTPS lock and address-field width — 2026-09-26

切片狀態 `VALIDATING`；不勾選整體 acceptance 條件。設計見 [計畫](../https-lock-plan.md)，
刻意差異見 [PORTING_PLAN.md](../../PORTING_PLAN.md)，幾何契約見
[presentation 契約](../reference-presentation.md)。

**建置注意：** 本 session 中 WSL 時鐘倒退，`.o` 的 mtime 晚於「現在」，`touch` 不會觸發
重建（一次突變測試因此誤判為通過）。以下結果都在刪除受影響的 `.o`（或所有未來時間的
`.o`）後重建取得。時鐘倒退也讓剛簽發的憑證「尚未生效」，因此 `tests/https_fixture.py`
以 `openssl ca -startdate` 把有效期往前回推兩天。

- **Oracle：** `tests/https_oracle_probe.py` 在 dummy SDL 下用 frozen Python 與三個
  127.0.0.1 伺服器（HTTP、以 `SSL_CERT_FILE` 信任的測試 CA、未受信任的 CA）凍結
  `tests/fixtures/https_oracle.json`，三次輸出相同，`--check` 納入 CTest。結果：成功的
  HTTPS 為安全；pending 期間已不安全；憑證錯誤、其他傳輸錯誤、`about:bookmarks` 不安全；
  `http→https` redirect 不安全、`https→http` redirect 安全（跟隨請求 URL）；Back/Forward
  依 history URL；兩個分頁各自獨立、跟隨 active tab。另凍結 800/232/231/120/70px 安全與
  不安全時的地址欄、鎖頭、收藏按鈕矩形與鎖頭槽命中（點擊不聚焦）。
- **Native 網路與 tab set（`https_integration`）：** `test_network --ca` 能載入自簽
  `https://127.0.0.1`（含 `http→https` redirect）；不給 CA 或連到未受信任伺服器時
  `certificate_error` 為 true；HTTPS 上的連線中斷為一般錯誤；`--ca` 指向不存在的檔案時同樣回報憑證錯誤（不會退回系統信任根）。`test_tabset_secure.c` 驗證
  `TaiTabSetView.secure` 的首次載入前、成功、pending（保留舊鎖頭並確認 barrier 仍擋住）、
  之後的憑證錯誤與傳輸錯誤回滾、HTTP、`about:bookmarks`、兩種 redirect、Back/Forward、
  New Tab 與切換分頁；首次憑證錯誤與首次傳輸錯誤為不安全；以
  `tai_tabset_create_with_home_url` 建立時（正式信任路徑）測試 CA 不被信任。
- **幾何與 dummy SDL：** `test_tabs_secure_window --geometry` 的欄位與鎖頭矩形和 oracle
  比對：x 與右緣（右緣取 `min(oracle, 視窗寬)`）、鎖頭 x 與相對欄位的 y 全部相符；y 在
  120px 除外（既有的單 tab 窄寬 y 差異，已記入 PORTING_PLAN）。視窗模式在 800/232/120/70px
  點鎖頭槽後 Return 不導覽、點欄內收藏星會收藏、點右移後的欄位後 Return 重新導覽；不安全頁面
  點同一 x（147）會聚焦。
- **突變測試：** 把 `page->secure` 改成忽略錯誤、以及讓點擊處理忽略 `view.secure`，
  `https_integration` 都會失敗；還原後通過。
- **完整 CTest：** `ctest --test-dir build -j 3` 37/37 通過（新增 `https_oracle_probe`、
  `https_integration`）。
- **ASan/UBSan 含 LeakSanitizer**（`build-asan`、`ASAN_OPTIONS=detect_leaks=1`，
  `leak:libfontconfig.so`、`leak:libcairo.so` suppression）：`https_oracle_probe`、
  `https_integration`、`network`、`network_differential`、`browser_tabs`、
  `presentation_dummy`、`presentation_chrome`、`browser_navigation` 與 bookmarks 三項，共
  11/11 通過；已用 `nm` 確認新測試執行檔有 ASan instrumentation。
- **真實視窗（Xvfb `:97`，`tests/tools/window_session.sh`）：** `tai-browser` 不信任測試 CA，
  因此把 `test_tabs_secure_window` 複製到 scratch 目錄（命名為 `tai-browser`，旁邊放
  `test-ca.pem` 與 `assets/browser.css`），以 `--bin` 啟動其 `--window` 模式，PID／視窗 ID
  已核對。HTTPS 頁面在 (140–154, 57–71) 顯示黑色外框鎖頭，地址文字從 x=162 起；點鎖頭槽
  (147,63) 再按 Return：沒有游標、Back 仍停用（未導覽）。New Tab 後在地址欄輸入 HTTP URL：
  該分頁沒有鎖頭、地址欄回到 x=132；切回 Tab 0 鎖頭恢復。33 個字元對應 33 個
  `SDL_EVENT_TEXT_INPUT`，沒有 windowid=0 的鍵盤事件；`stop` 回報 `browser exit=0`。第一次
  嘗試得到「certificate is not yet valid」，促成上述有效期回推。截圖存於
  `/tmp/tai-https-lock-acceptance-2026-09-26/`（已目視檢查；`/tmp` 可能被重置）。
- **獨立審查：** `ownership-reviewer` 檢查 CA 字串跨執行緒的所有權（在 `pthread_create` 前寫入）、`tabset_create` 各失敗路徑、libcurl 對 `CURLOPT_CAINFO` 的複製、redirect 重設與測試的執行緒邊界，沒有發現缺陷；依其建議補上不存在 CA 檔的測試。
- **尚未涵蓋：** 與 Python 的 chrome 像素比對、WSLg／Wayland 下的鎖頭點擊、單 tab 79–124px
  的地址列 y 差異（切片外既有問題）。

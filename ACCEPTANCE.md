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

不以外網網站可用性作 deterministic acceptance；人工 test.md scenarios 改為本地 fixtures。
Pixel-perfect 只在字型、版本、backend 與環境固定時使用；優先比較 DOM/layout/display 結構。

已知刻意差異、目前移植切片與剩餘功能缺口的唯一紀錄見 [PORTING_PLAN.md](PORTING_PLAN.md)。SDL dummy automation 涵蓋程式輸入、地址列、history 和 tabs 操作；真實 X11/Wayland 鍵盤輸入與事件順序仍未自動化驗證。

## Slice evidence

新增切片證據時，只新增一個 `docs/acceptance/<YYYY-MM-DD>-<slug>.md` 檔案與下表一列；
不要把長篇證據直接寫回本檔（見 [docs/acceptance/README.md](docs/acceptance/README.md)）。

| Slice | Date | State | Summary (proven / main gap) | Detail |
| --- | --- | --- | --- | --- |
| HTTPS 鎖頭與地址欄寬度 | 2026-09-26 | VALIDATING | 本機 HTTPS oracle（成功／pending／憑證與傳輸錯誤／redirect／history／跨 tab）與 5 個寬度的幾何凍結；native 整合、dummy SDL 命中、ASan-UBSan-LSan 與 Xvfb 真實視窗鎖頭都已驗證；chrome 像素比對與單 tab 窄寬 y 差異未處理 | [detail](docs/acceptance/2026-09-26-https-lock.md) |
| 兩顆星書籤 | 2026-09-26 | VALIDATING | oracle/integration/ASan-UBSan-LSan（fontconfig/cairo suppression）全過，重啟後持久化、多寬度真實視窗星星與清單導覽已驗證；79–231px dummy SDL 命中與 WSLg/Wayland 點擊未覆蓋 | [detail](docs/acceptance/2026-09-26-two-star-bookmarks.md) |
| 原生 Wayland 地址列鍵盤輸入 | 2026-09-26 | VALIDATING | 隔離 Weston（X11 backend）驗證 XTEST→Wayland seat→SDL3→地址列真實鍵盤輸入；WSLg 自身 Wayland seat 未覆蓋 | [detail](docs/acceptance/2026-09-26-native-wayland-keyboard.md) |
| WSLg 手動鍵盤輸入 | 2026-09-25 | VALIDATING | 使用者於 WSLg 以實體鍵盤驗證 X11→SDL→地址列輸入；自動化合成輸入仍受 WSLg 焦點撤回限制 | [detail](docs/acceptance/2026-09-25-wslg-manual-keyboard.md) |
| 鍵盤輸入、loader-thread 配置故障與 QuickJS OOM | 2026-09-25 | VALIDATING | Xvfb 真實鍵盤輸入、loader-thread 配置故障掃描通過，發現並修補 QuickJS-NG class_proto OOM use-after-free（已 tracked patch，新增 2 個 CTest）；本機 HTTP OOM 掃描未納入 CTest | [detail](docs/acceptance/2026-09-25-keyboard-loader-oom-quickjs.md) |
| Chrome 比對、窄寬版面與資源探針 | 2026-09-25 | VALIDATING | 依 frozen oracle 矩形修正 toolbar/address 幾何後與 Python 像素差異下降，LeakSanitizer（fontconfig suppression）與 0–79 配置故障注入通過；仍缺按鈕／書籤／鎖頭等視覺控制項 | [detail](docs/acceptance/2026-09-25-chrome-comparison-narrow-layout.md) |
| HiDPI 指標像素座標 | 2026-09-25 | VALIDATING | button event 改為量測後轉換實體像素再命中，2× Weston headless 探針驗證；xdotool 焦點／鍵盤交付仍未驗證 | [detail](docs/acceptance/2026-09-25-hidpi-pointer-coordinates.md) |
| 25-tab native chrome | 2026-09-25 | VALIDATING | 超過視窗的 tab 改編號方框、上限 25 個且真實視窗命中已驗證；完整 pixel diff 與 OS 鍵盤輸入順序仍缺 | [detail](docs/acceptance/2026-09-25-25-tab-native-chrome.md) |
| Tabs 真實視窗驗收 | 2026-09-25 | VALIDATING | WSLg 真實滑鼠序列（New Tab/切換/Back/Forward）與 800px／120px key-region 像素證據相符；真實鍵盤輸入與三個以上 tab 窄寬裁切未驗證 | [detail](docs/acceptance/2026-09-25-tabs-real-window-acceptance.md) |
| Tabs 切片驗證 | 2026-09-23 | VALIDATING | 完整 CTest 30/30、tabs oracle 相符、本機 HTTP 整合與 ASan/UBSan focused 5/5；低寬度/多 tab clipping、pixel comparison、真實輸入與配置故障注入仍缺 | [detail](docs/acceptance/2026-09-23-tabs-slice-verification.md) |
| 使用者手動 smoke | 2026-09-23 | 補充證據 | 使用者手動開啟外網站點回報 chrome／導覽成功，操作序列未記錄；不替代自動化或 pixel diff | [detail](docs/acceptance/2026-09-23-manual-smoke.md) |
| Pre-tabs 驗證快照 | 2026-09-23 | 已被取代 | focused CTest 5/5、完整 CTest 28/28、ASan/UBSan focused 5/5、視窗截圖目視確認；無 pixel diff，已被上方 tabs 結果取代 | [detail](docs/acceptance/2026-09-23-pre-tabs-validation-snapshot.md) |
| Paint/raster 切片 | 2026-09-20 | VALIDATING（早期） | block/scroll/blur/blend/hit-query 與 Cairo PNG 輸出的 structural/key-pixel/hit differential 已驗證；互動 scroll、image、event dispatch、SDL orchestration 與完整 raster differential 尚未移植 | [detail](docs/acceptance/2026-09-20-paint-raster-slice.md) |

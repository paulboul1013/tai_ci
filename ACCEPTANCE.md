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

目前仍缺低寬度／大量 tabs 的 chrome clipping 驗證、兩個以上 tabs 的 native active/inactive 樣式 pixel comparison、真實 X11/Wayland 輸入事件順序，以及 allocation failure injection。SDL dummy window assertions 不能取代原生桌面 pixel comparison；此切片不宣稱 visual parity。

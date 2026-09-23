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

截至 2026-09-23，原生瀏覽器另有一條仍屬 `VALIDATING` 的互動切片：目前視窗 click、文字/password 輸入、checkbox、button/Enter 表單提交與同視窗文件替換。Dummy SDL 覆蓋視窗 ID、focus、Unicode、特殊鍵、preventDefault、替換後新頁的 checkbox 輸入，以及候選載入失敗後仍處理後續事件；loopback HTTP fixture 驗證 GET query、POST payload/headers、Referer、跨文件 fragment scroll、preventDefault 不發請求與舊頁保留。完整 CTest 25/25 與最終 focused CTest 5/5 通過。ASan/UBSan focused `browser_headless`、`browser_navigation`、`presentation_dummy` 3/3 通過並使用 `ASAN_OPTIONS=detect_leaks=0`，不代表 LeakSanitizer 無洩漏證據。這些結果不勾選上述任何整體驗收條件；history/back-forward、tabs、browser chrome、mailto 外部啟動與原生視窗鍵盤驗證仍未完成。

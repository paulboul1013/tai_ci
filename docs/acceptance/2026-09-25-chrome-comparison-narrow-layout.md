# Chrome comparison, narrow layout and resource probes — 2026-09-25

以 frozen Python `Chrome.paint()` 的 display list 離屏輸出固定兩分頁 chrome，和相同
800px／120px 狀態的 X11 `Tai Gar` 視窗截圖比較。比較範圍只含 chrome；Python Skia
與 native Cairo 的字型光柵化不同，因此全像素差異率是診斷訊號，驗收以控制項、幾何與
key regions 為主。修正前 800×75 active Tab 1 有 18,568／60,000 不同像素；
120×139 active Tab 0／1 分別有 8,889／9,245 不同像素。背景純色區域一致，
但 Python 有橘色按鈕、書籤與 HTTPS 鎖頭，native 對應外觀仍缺；這些明確差異由
`PORTING_PLAN.md` 追蹤。Python 的 Back/Forward 在 800px 從 y=42.48、120px
從 y=62.48 開始；native 原先都從 y=36 開始，蓋住 120px 第二行 Tab 1 文字。

以 Python oracle 新增的 Back/Forward/address 矩形凍結至
[`tabs_oracle.json`](../../tests/fixtures/tabs_oracle.json)。Native tabbed chrome 現在使用對應的
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

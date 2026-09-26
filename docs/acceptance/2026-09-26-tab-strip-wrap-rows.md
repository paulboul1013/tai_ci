# Tab-strip wrap and toolbar rows — 2026-09-26

切片狀態 `VALIDATING`；不勾選整體 acceptance 條件。契約見
[presentation 契約](../reference-presentation.md)，差異見 [PORTING_PLAN.md](../../PORTING_PLAN.md)。

**問題：** native 在寬度 <125px 時一律把 toolbar 各列下移 20px（假設第二個 tab 已換行），
但 Python 只有在 tab 標籤真的換行時才下移。一個 tab、79–124px 時 native 地址欄 y 多了
20px（120px：native 119.212，Python 99.212），先前 `https_integration` 以已知差異略過。

**修正：** `tai_pres_tab_row_wraps()` 依最後一個標籤右緣（`tab_link_left/width`）判斷換行；
所有 `tabs_*` 位置函式改以換行狀態計算；視窗迴圈在換行狀態改變時以
`tai_tabset_resize` 調整所有 tab 的 viewport。

**建置注意：** WSL 時鐘仍可能倒退；以下結果都在刪除受影響的 `.o` 後重建取得。

- **Oracle 量測：** 以 frozen Python 掃描 1–3 個 tab、各 active、19 個寬度：一個 tab 在
  <84px 換行，兩個 tab 在 <125px 換行，與 `tab_link_left + tab_link_width` 的右緣
  （84／125）一致；三個 tab 在 native 為單行編號格（既有刻意差異）。
- **`tab_strip_differential`（新 CTest）：** live Python oracle 與 `tab_strip_probe` 比對
  一／兩個 tab、各 active、17 個寬度共 51 個案例的 chrome bottom、地址欄 y、Back y，
  容差 0.15px（Python 粗體／一般標籤混合造成的 ≤0.14px 列高差）。突變成舊規則
  （`width < 125`）時在 `(1 tab, 84px)` 失敗。
- **`https_integration`：** 移除 120px 的 y 例外，地址欄與鎖頭矩形在 5 個寬度都與 oracle
  完全相同。
- **`presentation_dummy`：** 120×300、一個 tab，點 New Tab 後兩個 tab 的 viewport 高度
  都變成 300−138.48；關掉「換行時 resize」的突變會失敗。原本 120px 書籤案例的點擊
  座標改為 oracle 位置（清單按鈕 y 72.48–96.48、地址欄 y 99.212–115.212）。
- **完整 CTest：** 38/38 通過。
- **ASan/UBSan 含 LeakSanitizer**（`leak:libfontconfig.so`、`leak:libcairo.so`）：
  `presentation_dummy`、`tab_strip_differential`、`bookmarks` 三項、`browser_tabs`、
  `https_oracle_probe`、`https_integration`、`presentation_chrome` 9/9 通過。
- **真實視窗（Xvfb `:96`，`tai-browser`，120×300）：** 一個 tab 時地址欄緊接在書籤按鈕
  下方（y≈99）；點 New Tab 後「Tab 1」換行，整排下移 20px（y≈119），頁面內容跟著下移。
  `browser exit=0`。截圖存於 `/tmp/tai-https-lock-acceptance-2026-09-26/narrow-*.png`。
- **尚未涵蓋：** 窄寬 tab 標籤的逐字排版與命中區（見 PORTING_PLAN「Chrome 缺口」）。

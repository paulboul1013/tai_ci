# 字型 fallback 與中文斷行（D-F1、D-F2，2026-11-02）

狀態：VALIDATING。計畫見 [font-fallback-plan.md](../font-fallback-plan.md)。

## 問題

`layout.c` 每個字型 key 只用一個 `FcFontMatch` 結果。預設字型（Times → Nimbus Roman）的頁面中，
中文、希伯來文、阿拉伯文整行空白；指定 `DejaVu Sans` 時中文全是方框。Python oracle 相同
（`get_font` 一個 typeface、`drawString` 無 fallback），而且以 `str.split()` 切 word，整段中文不換行。

## 新增

- `src/layout.c`：`glyph_font`（每個非 ASCII 字元的繪製字型，快取在 `Advance.owner`）、
  `fallback_font`（每個主字型一次 `FcFontSort`，取第一個 charset 有該字元、以名稱重開後仍有 glyph 的字型）、
  `cjk_break`（中日文字前後可換行；禁則標點）、`joins`（組合符號、ZWJ、異體字選擇器跟前一個字）、
  `piece_word`（依字型與換行點切成 TextLayout box，cluster 整塊換行）。ASCII 與主字型已有的字走原路徑。
- `tests/font_fallback_oracle_probe.py`＋`tests/fixtures/font_fallback_oracle.json`：凍結 oracle 缺 glyph
  與整段中文一個 word。
- `tests/font_fallback_integration.py`：切段、字型覆蓋、100px 換行、禁則、組合符號、希伯來文、
  無字型的 U+E000、`pre`。不寫死字型名稱；沒有 CJK 字型時 SKIP。
- `tests/test_layout_oom.c`：PAGE 加入中文 fallback 與強制換行。
- `tests/layout_differential.py`：兩個案例的 `中文` 換成希臘字母（仍測非 ASCII advance 快取）。

## 證明了什麼

- **Oracle：** Times 與 sans-serif 對「中文」都是 glyph 0；100px div 中整段中文是一行一個 word。
- **突變：** 四個一次性突變都讓 `font_fallback_integration` 變紅：關掉 fallback、關掉中文換行點、
  清空行首禁則、清空行尾禁則（第一次「關掉禁則」沒有變紅，因此補上 `check_kinsoku`）。每次以
  `clear_stale_objects.sh` 強制重編，還原後轉綠。
- **真實截圖：** `tai-browser --screenshot` 的中文頁面，中文、粗體中文、希伯來文、阿拉伯文都有畫出，
  長段落在 800px 換行。（希伯來文與阿拉伯文的順序與連字仍錯，屬 BiDi／shaping 切片。）
- **Xvfb 真實視窗：** `window_session.sh` 開 `127.0.0.1` 中文頁，標題 `中文測試`，resize 到 420px 後
  段落重新換行、行首沒有 `，。`；`GET /index.html` 200。trace：1 次 `Tab.load`、1 次
  `_finish_document_load`（18.1 ms）、1 次 `render`（15.4 ms）、8 次 `raster_and_draw`。
- **時間（Debug、`--screenshot`、7 次中位數）：** 英文 1 萬字 65.8 → 67.6 ms；中文 1 萬字
  36.9 → 67.5 ms（每個中文字一個 box，和同字數英文相近）。
- **回歸：** CTest 82/82（含 2 個新測試）。ASan-UBSan-LSan：本切片相關的 `layout`、`layout_oom`、
  `layout_differential`、`font_fallback_*` 5/5；整套 66/82，失敗的 16 個與修改前（同 commit 的
  main checkout）完全相同，都是 Cairo toy font／fontconfig 的既有洩漏。
- **審查：** `ownership-reviewer` 無 critical/high；採納 3 項（註解、無效 UTF-8 一致、組合符號
  跟前一字），並依建議把 CJK 加入 `layout_oom`。

## 剩餘缺口

chrome 文字（分頁標題、地址列）、input 值、`abbr` 沒有 fallback；不是完整 UAX #14；BiDi 與 shaping
未做；韓文沒有系統字型。

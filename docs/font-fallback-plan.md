# 字型 fallback（切片 A）：實作計畫

**狀態：已實作，`VALIDATING`（2026-11-02）。** F1–F7 依下表完成；決定 A1–A4 經使用者 2026-11-02 確認（A1：fontconfig；A3：中文斷行併入 A；字型用系統現有的 Droid Sans Fallback）。

真實 `tai-browser --screenshot` 截圖顯示：預設字型（DejaVu Serif）的頁面中，中文、希伯來文、阿拉伯文
整行空白；指定 `font-family: "DejaVu Sans"` 時，中文全部是方框。原因是 `layout.c` 每個 font key 只用
`FcFontMatch` 選**一個**字型（`src/layout.c:181`），缺字時不換字型；`render.c` 的
`cairo_select_font_face`（`src/render.c:1192`）也不換。這台機器有 Droid Sans Fallback，但 native 不用它。

## Python oracle 的行為

```
 get_font(family, weight, style, size)        browser.py:1520-1558
   └─ skia.Typeface(family, style) → TYPEFACES 快取（每個 key 一個 typeface）
 DrawText.execute → canvas.drawString(...)    browser.py:2924，沒有 shaper、沒有 fallback

 實測（skia 144.0.post2）：
   Typeface("serif")       → DejaVu Serif，textToGlyphs("中文") = [0, 0]，measureText = 20.0
   Typeface("DejaVu Sans") → DejaVu Sans， textToGlyphs("中文") = [0, 0]
```

Oracle 同樣不做 fallback。native 加上 fallback 後與 oracle 不同，記為刻意差異 **D-F1**。

## Native 的設計

```
 word(in, node, s, n)                                  src/layout.c:670
   │  逐字元：
   ├─ 主字型有 glyph（FT_Get_Char_Index ≠ 0，結果與 advance 一起快取） → 主字型
   └─ 沒有 → fallback(l, 主字型, c)
               ├─ 快取（每個主 Font 一張 codepoint → Font* 表）
               └─ 未命中：FcFontSort(同 family/weight/slant 的 pattern)
                          取第一個 FcCharSetHasChar(c) 的字型 → font_get(實際 family 名)
                          找不到 → 主字型（畫出 notdef，與現在相同）
   │
   ├─ 整個 word 只用主字型 → 現在的單一 box 路徑（ASCII 頁面完全不變）
   └─ 用到 fallback → 依字型切成連續段，每段一個 TextLayout box，
                       只有最後一段帶 space，整個 word 一起換行（沿用 abbr_word，src/layout.c:608）
 Box.family = 該段字型的實際 family 名（例：Droid Sans Fallback）
   │
   ▼
 display list：每段一個 TAI_DRAW_TEXT，font_family = 實際 family 名
   ▼
 render.c 不改：cairo_select_font_face(實際 family 名) → fontconfig 解析到同一個檔
```

- Fallback Font 由 `TaiLayout` 的字型快取擁有，與現有 Font 相同的生命週期；Box 只借用 family 字串。
- 行高：每段 box 用自己字型的 ascent／descent（現有的 per-box 規則）。
- `FcFontSort` 的結果每個主 Font 只算一次並保留，之後只查 charset。

## 中文斷行（使用者 2026-11-02 決定併入 A）

Python `recurse` 以 `tree.text.split()` 切 word（browser.py:4034），整段中文是一個 word，不換行、
超出視窗。native 改成：

```
 recurse()：依空白切 word（不變）
   ▼
 word()：含中日文字時切成 cluster
   ├─ 每個中日文字（漢字、假名、全形標點）前後是換行點；其他字元連在一起
   ├─ 禁則：，。、！？」）』：；黏在前一個字後；「（『黏在後一個字前
   └─ 每個 cluster 再依字型 fallback 切段，每段一個 TextLayout box
   ▼
 放進行：cluster 放不下就換行（沿用 abbr_word 的整塊 append）
         同一 word 內 space = 0，只有最後一段帶空白
```

不含中日文字的 word 走現在的單一 box 路徑。刻意差異 **D-F2**。

## 步驟

| # | 步驟 | 證據 |
|---|---|---|
| F1 | 用 oracle 確認上面的 Skia 行為，凍結成小 probe | probe 輸出 glyph 0、無 fallback |
| F2 | 寫失敗的單元測試：`<p>Tai中文!</p>` 切成 3 段、中文段 family 不是主字型、寬度 > 0、display list `font_family` 正確；窄寬中文段落換成多行、行首沒有禁則標點 | 先紅 |
| F3 | `layout.c`：glyph 存在快取＋`fallback()` 查找 | F2 轉綠 |
| F4 | `layout.c`：cluster 斷行＋依字型切段（共用 abbr_word 的段落 append） | F2 轉綠 |
| F5 | 迴歸：全部 CTest、layout／display／browser differential、ASan-UBSan | 全綠，ASCII 結果不變 |
| F6 | Xvfb 真實視窗：中文頁面截圖，中文區域有像素；`BROWSER_TRACE_FILE` trace 分析；layout 時間與 F3 前比較 | 截圖＋trace 摘要 |
| F7 | 文件：PORTING_PLAN（切片列、D-F1、D-F2 差異列）、ACCEPTANCE | 文件同步 |

## 要確認的決定

| # | 決定 | 建議 | 其他選項 |
|---|---|---|---|
| A1 | 怎麼找 fallback 字型 | fontconfig `FcFontSort`＋charset，尊重系統設定 | 固定清單（Noto CJK、Droid…），可預測但換機器就壞 |
| A2 | 範圍 | 只做頁面文字（word box） | 同時做 input 值、chrome 分頁標題 |
| A3 | 中文斷行 | 併入 A（使用者 2026-11-02 決定），見上節 | — |
| A4 | 都找不到字型時 | 用主字型畫 notdef（方框），與現在相同 | 改畫空白 |

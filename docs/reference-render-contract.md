# Python oracle 與文字量測契約

本契約以 `tests/reference/browser.py` 的完整 module import 實測，沒有抽取 AST 或 mock Skia/SDL。`tests/oracle.py` 每次執行先驗證 `manifest.json` 的 SHA-256，然後固定 cwd 至 reference，使 stylesheet 不受呼叫端工作目錄影響。原始程式輸出轉至 stderr，stdout 僅輸出完整 JSON；序列化失敗不輸出半份 JSON。

## JSON 觀察格式

DOM element 為 `{tag,attributes,children}`，text 為 `{text}`。style 指令額外加入每個 node 的 `style`。CSS rule 為 `{selector,declarations}`，declarations 保留 Python 的 `[value,important]`；selector 含 `kind`、`priority`，名稱使用 `name`，巢狀 selector 使用 `children`。kind 為 `tag/class/id/visited/has/sequence/descendant`。

layout 指令保留 layout tree 的類別、DOM path、geometry、font metrics 與 display list。DOM path 從 `0` 開始，子節點以 `/index` 延伸。synthetic node 保留其 tag 與 parent path。只序列化 instance 資料，不能將同名 method（例如 BlockLayout.word）誤當資料。Skia Rect 本身使用 float32，所以 display rect 與 Python double layout geometry 可有數個 ULP 的差距。

## 2026-09-06 字型實測

環境：FreeType pkg-config 26.1.20、fontconfig 2.15.0；以系統 fontconfig 解析 family。以下數值來自同一次比較，字型均為 regular、16px。

| 請求 family | resolved family | ascent | descent | space | Hello | world | AV | office |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Times New Roman | Liberation Serif | 14.2578125 | 3.4609375 | 4 | 35 | 37 | 24 | 36 |
| Arial | Liberation Sans | 14.484375 | 3.390625 | 4 | 38 | 39 | 22 | 38 |
| Courier New | Liberation Mono | 13.3203125 | 4.8046875 | 10 | 50 | 50 | 20 | 60 |

檔案位於 `/usr/share/fonts/truetype/liberation/Liberation{Serif,Sans,Mono}-Regular.ttf`。比較使用 FreeType `FT_Set_Char_Size(face,0,16*64,72,72)` 與逐字 `FT_Load_Char(...,FT_LOAD_DEFAULT)`；glyph `advance.x/64` 求和在以上 15 個案例與 Skia `measureText` 完全相等。`AV` 和 `office` 顯示這條 reference 量測路徑不應額外加入 kerning 或 ligature shaping；native HarfBuzz 路徑必須保留相同可觀察 advance，而不能直接使用預設 OpenType shaping 改變斷行。

垂直 metrics 使用未 grid-fit 的 `face->ascender * size / units_per_EM` 與 `-face->descender * size / units_per_EM`。不可直接使用 `face->size->metrics`：該值在 Serif/Sans 為 15/4，Mono 為 14/5，與 Skia 不同。這項等價已驗證上述三個 TTF，不應未驗證便推廣到所有格式或 variable font。

初次量測時系統曾將 Times New Roman 解析到 Nimbus Roman，所得 ascent/descent 為 10.928000450134277 / 5.072000026702881；因此不可跨字型安裝狀態比較 golden。Differential runner 應讓 Python/C 共用 fontconfig 設定，並記錄 resolved family 與字型檔。此處數值不是跨機器的固定期望。

## Layout rounding 與基線

`css_font_size_to_skia` 使用 `max(1,int(round(css_px * FONT_SCALE)))`，Python `round` 為 ties-to-even；C 的一般 `round()` 不等價。`get_font` 再將 size 截斷為正整數。sup 使用 `int(size/2)`，small caps 使用 `int(size*0.8)` 並選 bold。

TextLayout width 為整段 word 的 `measureText`，height 為 ascent+descent；space_after 通常單独量測空白，特定流程可覆寫。LineLayout baseline 為 `y + 1.25 * max_ascent`，正常 child y 為 baseline-child.ascent，sup 則使用正常文字的最大 ascent。line height 為 `1.25 * (max_ascent+max_descent)`。應比較浮點幾何而非先四捨五入成像素；raster Rect 的 float32 rounding 應獨立處理。

## 已執行 smoke

`python3 tests/oracle.py` 的 dom、style、css、url、layout 均成功產生可經 `json.loads` 讀取的完整 JSON。layout 用 `<p>Hello world</p>`，CSS 用 `p.x:has(a) {color: red !important}`，DOM 包含 entity decode。這只證明 oracle 可執行，不構成 native migration 完成證據。

## Native C 初始 display-list contract

`tai_layout_visit` 的 callback 參數是 borrowed view，只在 callback 期間有效；它不能被存入
display list 或跨執行緒傳遞。`TaiDisplayList` 僅保留自有 command array，以及複製的文字和
字型名稱，故其 source layout、DOM、stylesheet 可在 list 建立後釋放。初始 Cairo backend
按 paint traversal 順序支援 `fill_rect` 與 `text`，以 opaque white ARGB32 image surface
輸出 PNG；Cairo 的 premultiplied/native-endian 像素格式尚未接 SDL3 的 RGBA conversion。

這個切片刻意沒有宣稱等價於 Python `paint_tree`：尚缺 rounded clip、scroll transform、
opacity/blend、blur、image、hit-test 與完整 display differential。`tests/test_render.c`
用紅色 block、藍色文字、PNG 像素錨點，並在釋放 DOM/layout 後再次 raster，驗證目前的
ownership contract。

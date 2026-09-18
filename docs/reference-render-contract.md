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

## Native C display-list contract

Display-list ownership 與 borrowed lifetime 的權威定義在
[`docs/architecture/native-runtime.md`](architecture/native-runtime.md)。初始 Cairo backend
按 paint traversal 順序支援 `fill_rect`、`text`、不 raster 的 `hit_test`，以及成對的
`push_clip`/`pop_clip`、`push_clip_scroll`/`pop_clip_scroll`，以 opaque white ARGB32 image surface 輸出 PNG；Cairo
的 premultiplied/native-endian 像素格式尚未接 SDL3 的 RGBA conversion。
Opacity 與 mix-blend-mode 另以成對 `push_blend`/`pop_blend` 表示；push 複製 clamp 後 alpha
與 effective source-over/multiply/difference/destination-in mode，pop 將 isolated Cairo group
一次合成回 parent。normal、src-over、source-over 且 opacity 1 的 no-op subtree 不產生命令；
未知 mode 仍隔離但以 source-over 執行，符合 frozen oracle。
CSS `filter: blur(...)` 以成對 `push_blur`/`pop_blur` 表示，push 僅複製 sigma，並位於
overflow group 內、subtree leaves 外，因此固定順序為 subtree → blur → rounded overflow clip
→ opacity/blend。sigma zero 與 invalid/negative/unitless non-zero 值不產生命令。Cairo group 的
premultiplied ARGB32 channels 使用截斷於 3σ 的 separable Gaussian kernel；這與 Skia 使用相同
sigma 語意但不宣稱逐像素相同，acceptance 限於結構與穩定 key regions。Exact convolution
另有每次 effect 25,000,000 channel-tap work budget；超限時 PNG raster 明確失敗，不會靜默換成
不同濾鏡或讓不受信任 CSS 長時間佔用 raster thread。display list 仍保留原 sigma。

`tests/display_differential.py` 遞迴穿過 Python `Blend.children`，將可比較的 `DrawRect`、
`DrawText` 正規化為 native RGBA 與 x/y/width/height，以 `0.0001` 絕對誤差比較順序、種類、
文字、顏色及幾何；案例包含多 block、換行和巢狀 styled element。字型 family 與 glyph
pixel 不在此 gate。overflow scroll 案例另將 Python `DrawHitTest`/`Scroll` 正規化為 native
hit-only leaf 與 push/pop，
驗證 empty-container elision 與 nested subtree ordering；rounded-fill differential 則涵蓋
`10px`、`10.5px`、`1e999px` 的 reference DrawRRect/DrawRect 分流。rounded fill/clip 的 key-region
raster 與 corner/center hit 由 `tests/test_render.c` 和 `tests/hit_differential.py` 覆蓋。

座標契約如下：layout leaves 和 clip rect 都儲存 document coordinates；scroll child 不先改寫
為 local coordinates。`push_clip_scroll` 在當前 document transform 下先套用矩形 border-box
clip、再 `translate(0, -scroll_y)`，直到配對 pop restore；`push_clip` 則先隔離整個 subtree，
在配對 pop 以 rounded destination-in mask 合成，避免 parent 背景在 antialiased corner 漏入 child。
`tai_display_list_hit_test` 接受 document-space
座標，從最前景 leaf 反向搜尋；scroll 子樹先拒絕 clip 外點，再將 y 加上 `scroll_y` 後遞迴，
巢狀 transform 依序合成。`push_clip` 是 raster-only rounded overflow mask；它不改變
descendant point-hit traversal，因 frozen Python 對 `Blend` mask 也不做 hit recursion。
`push_clip_scroll` 則保留 Python `Scroll.clip_rect.contains` 的矩形 hit gate，再轉換 child
coordinates。rounded fill 或 hit-only scroll leaf 以 Skia `Rect.contains` 的半開外框加上 clamp
後的 quarter-ellipse 判定。每個可命中 leaf 複製 stable node ID；透明 scroll container 在其 Scroll 之前
加入 hit-only leaf，使可見 child 未覆蓋時仍可命中。raw display query 不解析 DOM；`TaiPage`
才在其 document 存活時將 ID 解析成 live `TaiNode`。page scroll 由 `TaiPage` 擁有並 clamp 至
`[0, max(document_height + 2 * VSTEP - viewport_height, 0)]`；
`tai_page_viewport_hit_test` 唯一一次把 viewport `(x,y)` 轉成 document
`(x,y + scroll_y)`。viewport PNG 使用相同 state，以 `-scroll_y` translation raster，沒有
改寫或污染 immutable display list。

`tests/hit_differential.py` 以 frozen `hit_test_paint_commands` 比較最上層 paint leaf、clip
內外、非零與巢狀 scroll、transparent scroll container、矩形與 rounded corner 邊界，包含 fractional/huge
radius 的寬鬆 effect parser。`tests/test_render.c` 另以同一個
雙層非零 scroll fixture 交叉檢查 Cairo key pixel 與 hit target，並在其外疊加 page scroll。
`tests/page_scroll_differential.py` 比較 frozen Python 的 page clamp、viewport conversion、零與
非零 scroll 及半開邊界。

Opacity/blend differential 涵蓋 numeric/percentage、invalid fallback、clamp、case/whitespace、
unknown/normal elision；raster key pixels 涵蓋 subtree alpha、multiply、difference、destination-in、
sibling isolation，以及 rounded clip/scroll 在 outer blend 內的 nesting。Blend hit traversal 只反向
遞迴，不因 opacity zero 拒絕，也不新增 clip 或座標轉換。成對效果共用同一 Cairo stack，錯配
或未閉合會安全失敗。

Blur differential 涵蓋 px、case/whitespace、scientific notation、zero forms、negative、NaN、
unsupported unitless non-zero 與 invalid suffix；oracle probe 另鎖定 `blur(infpx)` 會解析成
infinity，但 frozen oracle 的 strict JSON serializer 隨即失敗。Native 為維持有限配置與合法
JSON，刻意將所有 non-finite sigma 視為 `none`。Blur raster key regions 驗證完整 subtree 只
隔離一次、expanded pixels 再被 rounded overflow 裁切、outer opacity/blend ordering 與 sibling
isolation；Blur hit traversal 不 clip、不 reject、不轉換座標。
大 finite sigma 的 regression 另驗證 work-budget error 與 deterministic cleanup；這是相對
Skia oracle 的刻意資源限制。

OpenMoji image slice 對 Python length 恰為一個 code point 的普通 word，依序查找目前工作目錄
`openmoji/{UPPERCASE_HEX}_color.png`、`openmoji/{UPPERCASE_HEX}.png`。只快取成功 decode；缺檔不
negative-cache；確定會拋例外的 corrupt/unreadable 候選立即 fallback 文字。Skia 回傳 `None` 後繼續
plain candidate 的分支沒有可攜 deterministic fixture，因此不在本 slice 的 comparison boundary。
asset layout 固定寬 22，
高度使用 Python ties-to-even rounding，image ascent 等於高度、descent 為零。display command 自有
premultiplied pixels，沒有 DOM/layout/path/decoder borrow；Cairo bilinear scaling 走既有
scroll/blur/rounded clip/opacity/blend stack，且 image leaf 不新增 hit target。普通 `<img src>` 仍忽略。
PNG decoder 使用既有 Cairo dependency，這個 slice 不宣稱一般 image/WebP/remote fetch 支援。
Native cache 由單一 `TaiLayout` 擁有並在 layout destruction 清除，不像 frozen Python module-global
cache 跨 layout 存活；差異只在同一 process 中 asset 被外部修改／刪除後另建 layout 時可觀察，換取
明確 owner 與 deterministic cleanup。解碼前另限制 64 MiB 檔案、16384 單邊與 25,000,000 pixels；
超限會依 decoder rejection 路徑 fallback，避免不受信任本地 asset 在驗證前造成無界配置。

這個切片刻意沒有宣稱完整等價於 Python `paint_tree`；blur 僅在上述 Gaussian comparison boundary
驗證，一般 HTML image 與 SDL presentation 仍未實作。
`tests/test_render.c` 用非零 scroll
驗證 command order、clip/translation raster key regions，並在釋放 source DOM/layout 後再次
raster，作為自包含 display-list ownership contract 的測試證據。

## Headless screenshot contract

`tai-browser --screenshot PATH URL` 使用同一個 `TaiPage` display list，同步輸出不透明白底
PNG 且不輸出 JSON。headless page viewport 固定為 800×532px；高度由 frozen Python 實際
render 後的 `Chrome.bottom`（約 68.34px）計算 `ceil(600 - Chrome.bottom)`。輸出只含目前
page viewport，並套用相同 page scroll。未指定 screenshot 時維持原本 JSON contract。

`tests/test_cli.c` 透過 subprocess 執行 CLI，驗證成功輸出、800×532 viewport 尺寸、白底與
紅色區塊 anchor pixels、無法寫檔、缺少參數、option-as-value 與 unknown option。這是
`TaiPage → display list → Cairo PNG` 的間接 E2E，不代表 SDL presentation 或完整 Python
paint-tree 等價。

# Native display list、Cairo raster 與 headless screenshot 契約

## Native C display-list contract

Display-list ownership 與 borrowed lifetime 的權威定義在
[`docs/architecture/native-runtime.md`](architecture/native-runtime.md)。初始 Cairo backend
按 paint traversal 順序支援 `fill_rect`、`text`、不 raster 的 `hit_test`，以及成對的
`push_clip`/`pop_clip`、`push_clip_scroll`/`pop_clip_scroll`，以 opaque white ARGB32 image surface 輸出 PNG；Cairo
的 premultiplied/native-endian 像素格式由 [SDL3 presentation](reference-presentation.md) 以不透明 ARGB8888 texture 接收。
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
驗證；一般 HTML image 仍未實作。SDL presentation 的範圍與驗證界線見
[視窗呈現契約](reference-presentation.md)。
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

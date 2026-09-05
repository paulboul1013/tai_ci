# tai_gar rendering、layout、window 原始行為分析

分析來源：`/home/paulboul/tai_gar/browser.py`；已完整讀取 rendering/fonts/geometry 1471–2498、BrowserApp 2529–2902、display/layout 2902–4260、Chrome/CommitData 5163–5700、Tab rendering/scroll 6219–6555、Raster/Window 6845–8545，以及 README.md、test.md、tai_ci/AGENTS.md。這份報告描述現有行為，並非 Web 標準要求。沒有修改來源。

## 實際 dependency graph

```text
HTMLParser + CSSParser + style + DOM
    ├─> DocumentLayout -> BlockLayout -> LineLayout -> Text/Input/Button/EmojiLayout
    │                        └─> font cache / Skia metrics / openmoji asset cache
    └─> Chrome 的合成 HTML -> 同一套 BlockLayout
layout -> paint_tree -> tree-shaped display list
    ├─> DrawText/Rect/HitTest/RRect/Line/Outline/RRectOutline/VectorIcon/Image
    ├─> Blur -> Blend（clip mask / opacity / mix blend）
    └─> Scroll（固定 clip + 子內容座標 transform）
display list ─> pointer/touch hit testing ─> DOM ancestor ─> Tab event/default action
Tab render -> CommitData -> BrowserWindow -> RasterWork
RasterWork -> RasterWindowState -> RasterResult -> BrowserWindow SDL present
BrowserApp SDL event -> BrowserWindow chrome event 或 Tab TaskRunner INPUT task
```

重要架構特性：Chrome 並非另一套手工 widget layout，它產生 HTML、解析 DOM、套用 default CSS，再使用 page 共用的 layout/paint/hit-test；vector icons 是無 hit target 的 overlay。Tab 本地 render 不會 commit，只有 RAF frame boundary commit。scroll-only frame 以 `display_list=None` 表示 reuse 先前 snapshot。

## Layout 的可觀察契約

預設 viewport 800×600；Document 起點 `(13,18)`，寬 `viewport-26`，Document.height 為子 Block 高度；commit 的 document height 再加 `2*18`。預設字型 16px、FONT_SCALE=1；input/button 寬 200、checkbox 13、button padding 4。

Block 判定為「是否有 display:block 的直接子元素」，不是依自身 display 決定。連續 inline 子節點合成 anonymous block group，`head` 在 child_groups 跳過；`h6` 後接 p 會將 h6 與 p.children 合併 run-in group，跳過兩者間純空白。li 的 x 加 20、可用寬減 20，paint 另畫 5×5 黑 bullet，位於 x-15/y+8。nav.links 背景 lightgray；nav#toc 另加 22px header 與固定文字 Table of Contents。

寬高只接受整數 px；width=0 因 truthiness 退回 parent width，height=0 仍有效。Block 無一般 margin/padding/border box 模型。content_height 保存自然高度，height 可被 CSS height 覆蓋。overflow:scroll 且存在合法固定高度才為 scrollable；scroll_y 存在 DOM element，relayout 後 clamp 保留；max_scroll=content_height-height。

正常文字以 Python str.split() 分字（Unicode whitespace），移除 soft hyphen U+00AD；每字附字型空白寬。超寬單字在空行不再切分。pre 使用 Courier New、不自動換行，保留空白/tab；newline 形成 line，必要空字串保留空白行高度。sup 字號減半，abbr 將 lowercase 轉 uppercase、80% size、bold，逐字 layout，整個 abbr word 作 wrap 判斷。sup/abbr/pre 為 mutable bool，巢狀同 tag 並非 stack。

Line width 為 parent width，實際內容寬=sum(width+space_after)-末尾 space；center/right 調 x；`--rtl` **僅強制右對齊**，不 reverse/reorder。baseline=y+1.25*max_ascent，height=1.25*(max_ascent+max_descent)。sup y 使用 normal text ascent；emoji/button 以全高 ascent、descent 0。所有 inline 子項先 measure，再排 x/y，button 在 layout_final 重建其內部內容 layout。

Button 的 synthetic button-content 節點借用真 DOM children，不改真 DOM parent；style copy 後清除自身 effects/background。C 不能把其 children 當 owned DOM 釋放。

Input hidden 不 layout；checkbox 畫白底黑框與兩筆 check；password 按 Python 字元長度顯示星號；cursor 按 Unicode code point index 與 prefix measure 決定。text input 沒有自動 clipping。

## Font / image

get_font 將 family list 只取第一項；serif→Times New Roman、sans-serif→Arial、monospace→Courier New。weight >=600 視 bold，其餘 normal；italic/oblique 視 italic。Typeface cache key `(family,weight,slant)` 不含 size，Font 每次 new。css font size 使用 Python round（ties-to-even），minimum 1。

目前 Skia measureText/drawString 並沒有 HarfBuzz 或 FriBidi 整合。C 的核准 stack 需要保留目前基線 measure/render 契約；新增 ligatures/BiDi reorder 不能默默改變行寬、wrap、cursor。

一般 `<img src>` 沒有 load/layout 分支。只有正常 word **恰為一個 Unicode code point** 時，嘗試 `openmoji/{HEX}_color.png`、再 `{HEX}.png`；成功 cache asset，顯示寬固定 22，高依比例 Python round，image raster 使用原圖縮放。missing 不 negative-cache。WebP/stb_image 是否使用應依實際 asset/擴充範圍決定，不能誤稱原版具備一般 HTML image 支援。

## Display list / effects / hit testing

Leaf command 包含幾何、繪圖資源與 originating layout pointer。DrawText rect 用 measureText 與 ascent/descent；DrawLine 保存原 endpoints（不可把向上斜線轉成 bbox diagonal）。DrawHitTest 為透明 scroll container 提供不可見點擊區域。

paint_tree 分 own commands / child commands，再套 node effects。scrollable Block 背景固定，仅 descendants 包 Scroll。effect 順序：subtree -> Blur -> overflow clip mask -> opacity/mix blend。Blur bounds 擴 3*sigma；blur sigma 僅 px 或 unitless zero。clip/scroll 使用白 rounded rect 的 destination-in，需外部 isolated source-over layer，否則會誤抹 sibling。Opacity/mix blend 共用 outer layer；normal opacity1 不需 layer。支持 multiply/difference/destination-in/src-over，其餘 mode 回 source-over。color 支持有限 named colors、#RGB/#RRGGBB/#RRGGBBAA，未知（如 purple/pink）**回 black**。

Hit test reverse paint order、遞迴 effects。Scroll 先 clip reject，再 y += scroll_y；一般 Blend/Blur 不改座標。leaf 必須含 layout target，再檢查 target border-radius shape。mask 沒 layout target，所以不直接 hit。**非 Scroll 的 overflow clip 並無完整祖先 clip hit rejection**，此差異不能誤當標準 CSS 行為。

Touch exact hit 若 interactive(a/button/input ancestor) 直接返回；否則半徑矩形 collect，依 interactive、距離平方、面積、paint order 排序。rounded overlap 用九個 sample 點。Scroll touch rect 逐層裁切/平移。

## Threads / snapshot / lifetime

Browser thread（process main）owns SDL init/event/present、Chrome DOM/layout、window registry、tab switching；每 Tab Main Thread owns DOM/CSS/layout/JS；process-wide raster worker owns CPU root/chrome/tab surfaces；network coordinator 另有 thread。CPU 可 sync/threaded A/B，GPU POC 強制 Browser-thread sync GL/Skia。

Window RLock 保護跨 thread window flags/committed_states；每窗 one RAF gate（animation_timer 到 frame measurement 完成才 release）與 one raster_in_flight。frame cost 完成 observation 後 rearm，避免 stale cadence。epoch 在 tab switch/resize 加一；完成 raster 若 epoch/size 不符或 closed 即 drop。

CommitData 移轉 display list ownership，成功後 Tab 清空 reference；但 list leaves **仍指向 layout objects，layout 又指 DOM**，Python GC 讓舊 snapshot 活著。C 若僅轉移命令 array 而立即 free layout/DOM，就會 UAF。建議 snapshot 真正 immutable，自有 text/font refs、獨立 hit metadata（node id + document generation）；或者 refcount scene arena 包含 DOM/layout 引用。Browser committed_link_at 會讀祖先 href，因此只保留 primitive rect 不夠。root 必須明定這個 contract。

RasterWork 捕捉 CommitData reference、chrome list tuple、size/epoch/dirty flags；不得讀 live Tab state。Raster worker cache key 為不重用 raster_id，避免 SDL ID recycled。close 清 pending/completed、讓既有 work 隔離完成；C 必須 deterministic join/cancel 與 references 回收，不能照搬 Python 1秒 join 後退出的假設。

## Raster 與 window flow

Raster cache root viewport、chrome height、tab interest region；region 高=min(documentheight,4*windowheight)，以 viewport 周圍居中且 clamp。page scroll-only 若 viewport 仍在 region，reuse tab surface，只重 compose。tab 切換清 tab cache，resize 清 root/chrome/tab。top-level commands 以 rect y cull，Cairo surface clip 為最終 guard。

Compose 順序 white root -> tab(surface offset=chrome_bottom+interest_start-scroll) -> blue scrollbar -> chrome。scrollbar minimum thumb 20。CPU snapshot 為 RGBA8888 unpremultiplied，SDL2 surface masks 根據 endian；Cairo ARGB32 為 premultiplied native-endian，移到 SDL3 必須明確 format/alpha conversion，否則會色彩與透明度錯誤。

Cairo 沒有直接 Gaussian blur primitive，需要正確 temporary surface/filter pass（注意 stride、premult alpha、3σ support）。Clip + source-over isolation、multiply/difference 支援要保持 subtree effects 層次，不能 flatten 全 list。

Window input：chrome y 內 Browser thread 直接處理並 queue page blur；page y 減 chrome height，交 Tab INPUT task。wheel 只看正負、一步 SCROLL_STEP，focused nested scroll container 飽和時仍吞事件、不 bubble page。middle-click 使用 committed scene 找 href 並開新 tab。Ctrl+N new window；tab/back/forward/bookmark/address controls 行為由 Chrome 處理。text input 過濾 code point<0x20。

Touch SDL normalized coords round 到 `(w-1,h-1)`；FINGERUP 才 click；move 超 threshold 或多指抑制 click；忽略 SDL 合成 touch mouse，避免雙事件；Shift+left click 模擬 touch。

## 已執行 oracle 證據

`python3` 已可 import skia/sdl2/dukpy；在 tai_gar cwd import browser 不開 window。使用原始 HTMLParser/style/DocumentLayout/paint_tree，非 mock，得到：

- `<p>hello world</p>`：document.height=20.000000596046448；hello rect `(13,20.73200035095215,44,36.73200225830078)`，world `(48,20.73200035095215,85,36.73200225830078)`。
- `<p style="display:none">visible?</p>`：仍有 DrawText visible?，同高；原版無一般 display:none skip。
- `<p>a<img src="x">b</p>`：只有 a/b DrawText，無 image；b.x=24。
- `<p>abc אבג</p>`：abc.x=13、Hebrew word.x=39，以 logical order 相鄰。

測試輸出帶 fontconfig warning，量測受系統字型影響；native differential 應 pin 相同實際 font file，且把文本/命令結構 exact 比對與 geometry tolerance 分開。

## 建議 acceptance / differential contracts

1. 將 test.md 全部 data/file fixture 分類取樣；該檔是手動命令清單，沒有自動 assertion，README 只記 CLI、多視窗、bookmarks。repository 未見 Python test*.py。
2. oracle adapter serialize layout tree：kind、DOM node path/id、x/y/w/h、content_height、scroll、word、font family/weight/size、line child order；display serialize effect tree/opcode/color/rect/endpoints/scroll/sigma/opacity/blend/target node id。排除 Python addresses/Skia objects。
3. layout fixtures：mixed inline/block、h6 run-in、pre blank line/tab、nested li/nav toc、sup+abbr、fixed zero height/width、hidden/password/checkbox/button complex content、emoji existing/missing、多 font size；以固定 metrics adapter 隔離 wrap 算法差異，再用真 font metrics 測 representative geometry。
4. effects fixtures：blur+rounded overflow+opacity+multiply 與前 sibling，nested Scroll hit coordinate、transparent hit target、upward DrawLine、未知 color fallback。
5. window integration：GUI load data/file/local HTTP、URL address edit/cursor、click navigate/back/forward/bookmark/new tab/new window、resize 每 tab relayout、page scroll/nested focus scroll、keyboard edits、touch suppression。
6. concurrency：raster blocked 時新 commit dirty 不丟；resize/tab switch drop stale result；close during raster/network/RAF 釋放全部；scroll-only no reraster inside interest region。ASan/UBSan、thread ownership review 獨立執行。
7. 不要求 Skia/Cairo bit-identical pixels；要求 command tree/geometry 合理 tolerance，raster 用 anchor pixel、alpha compositing、clip 邊界驗證。明確紀錄 font backend、antialiasing、GPU POC 與新 CPU native backend 差異。

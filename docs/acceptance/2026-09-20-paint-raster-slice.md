# Paint/raster slice — 2026-09-20

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

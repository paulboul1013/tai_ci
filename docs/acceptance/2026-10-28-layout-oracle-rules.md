# Layout／paint oracle 規則補齊（2026-10-28）

狀態：VALIDATING。

## 問題

`PORTING_PLAN.md` 在 2026-10-04 記錄 4 個沒有解釋的 layout 差異。重現後找到 3 個根本原因，並在同一類問題中再找到 7 個：

| 規則 | Oracle（`tests/reference/browser.py`） | 修正前的 native |
|---|---|---|
| `sup` | 字型 `int(size/2)`，頂端對齊一般文字 | 沒有縮小 |
| `button` | 內容排成 block，高度＝max(內容, 行高)＋8 | 只有字串，高度少 4–5px |
| `abbr` | 逐字 small caps（小寫→粗體大寫，0.8 倍） | 一般文字 |
| `h6`＋`p` | 合併成同一個 block | 兩個 block |
| `nav#toc` | block 模式上方 22px 標題帶與「Table of Contents」 | 沒有 |
| `nav.links`、`li` | 淺灰底、5×5 項目符號 | 沒有 |
| input 文字 | display list 寬度＝文字寬度 | 控制項寬度 200 |
| display:block 控制項的 block、button 內容的 block 與行 | 不自繪（`should_paint`／`button-content`） | 自繪底色與效果 |
| input／button 的 `overflow` | 以自己的框裁切 | 只有 block 裁切 |
| 缺字型（Georgia） | Skia 預設字型 | fontconfig 替代字型（刻意差異，見 `PORTING_PLAN.md`） |

原因相同：沒有機制檢查「oracle 的每條規則都有一個 differential 案例」。

## 修正

- `src/layout.c`：`sup`／`abbr`／`pre` 旗標比照 `open_tag`／`close_tag`；`abbr_word`（含由 Python 產生的 75 個多字元大寫表）；button 內容以 `layout_block(…, contents)` 排在原點後隨按鈕移動；`block_children` 比照 `child_groups`；`nav#toc` 標題帶與 caption；`TaiLayoutItem.wrapper` 標記不自繪的 block 與行；text 與 input 回報量測用的字型與寬度。
- `src/render.c`：文字使用 layout 量測的字型（刪除 render 自己的字型解析）；`make_block` 繪製 li／nav 裝飾；`make_own` 統一各種 item 的自繪；input／button 依 `overflow` 裁切。
- 新測試閘門 `tests/layout_rule_coverage.py`（CTest `layout_rule_coverage`）：以 AST 掃描 oracle 的 *Layout class 與它們呼叫的模組函式，抽出 74 條 tag／attribute／style 規則；每條規則都要有案例，attribute 規則要和同一條件中的 tag 一起出現，效果類 style 只能由比對該效果結構的清單滿足。
- 新測試 `tests/test_layout_oom.c`（CTest `layout_oom`）：以 `--wrap=malloc` 逐一讓 layout 與 display list 的每個配置失敗。

## 證明了什麼

- **修正前為紅：** `layout_differential` 在 `<sup>` 失敗（寬度 oracle 11、native 22）；`display_differential` 在 li 項目符號失敗。
- **Oracle 比對：** `layout_differential` 71 個案例＋2 個缺字型案例、`display_differential` 新增 11 個 leaf、2 個 rounded、2 個 effect、1 個 blur 與 7 個 clip 案例，全部相符。
- **閘門可以變紅：** 拿掉 CLIP／SCROLL 案例、把 `nav id=toc` 改成 `div`、拿掉 BLUR 案例，各自讓閘門失敗並指出要加的案例。
- **配置故障：** 每次 438 個專案配置，876 次注入（單次與持續）皆通過；ASan/LSan 下無錯誤、無洩漏。刻意在 `abbr_word` 失敗路徑製造洩漏時，LSan 在 `layout.c:645` 報錯（之後還原）。
- **Unicode：** `utf8proc_islower` 與 Python `str.islower()` 在所有碼位相同；`utf8proc_toupper` 只在 Python 結果超過一個碼位的 75 個字元不同，已由產生的表補上並有案例。
- **CTest：** 68/68。
- **ASan-UBSan-LSan**（suppression：`libfontconfig.so`、`libcairo.so`）：平行 `-j4` 66/68；`timer_window`（timer 提早觸發檢查）與 `presentation_dummy`（延遲 300ms 的書籤輸入）在負載下失敗，單獨重跑兩次皆通過，與本修正的程式路徑無關。
- **Xvfb 真實視窗：** 截圖顯示 toc 標題帶、links 底色、上標、small caps、項目符號、含粗體的 button 與 h6 合併；點擊 button 內的粗體文字送出表單（`GET /second.html?q=hello` 200），`browser exit=0`。
- **獨立審查：** Standards、Spec、ownership 三軸。Spec 找到 input／button 沒有 `overflow` 裁切與閘門盲點，已修正並加上 clip 案例；ownership 沒有記憶體或所有權缺陷。

## 剩餘缺口

- 閘門是字面檢查：不證明案例走到規則的條件分支，也不涵蓋不是比較式的規則（例如 `get_font` 的字型別名表）。
- `paint_tree` 本身的通用規則（例如所有 layout 物件的效果）不在 *Layout class 中，由既有 effect／clip 案例涵蓋。
- li 項目符號在 li 有 `border-radius` 時的命中測試：Python 以 li 的圓角框判斷，native 以項目符號自己的矩形判斷。

## 過程中的發現

WSL2 時鐘會倒退，用 Bash 修改的原始碼可能比物件檔舊，Ninja 因而連結舊程式碼。本次以 `tests/tools/clear_stale_objects.sh` 清除後重建，所有證據都在乾淨的物件上重跑。

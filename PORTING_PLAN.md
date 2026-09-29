# C17 瀏覽器移植計畫

## 目前工作：Chrome 與 History

**狀態：`VALIDATING`；下列四項都已實作並驗收（2026-09-28）。其後的
[新視窗（Ctrl+N）](docs/new-window-plan.md) 已實作並驗收（2026-09-29），`VALIDATING`，見下方第 5 項。
其後的 [視窗標題跟隨頁面與改名 Tai Ci](docs/window-title-plan.md) 已實作並驗收（2026-09-29），
`VALIDATING`，見下方第 6 項。** `--window` 已有 tabs、地址列、Back/Forward、每個 tab 的 URL history、fragment、表單導覽與非同步載入。下一個切片補齊 Python 可見的 chrome 狀態，並檢查 history 在切換 tab、分支導覽與載入期間的行為。以 [`tests/reference/browser.py`](tests/reference/browser.py) 的 `Chrome`、`Tab`、`BrowserWindow` 為 oracle；已驗證的結果見 [ACCEPTANCE.md](ACCEPTANCE.md)。

### 下一個垂直切片

1. **兩顆星書籤：已實作，`VALIDATING`。** 依 [書籤實作計畫](docs/bookmarks-plan.md) 完成共用且跨重啟保存的收藏、網址列內灰／亮切換星、左側書籤清單入口與可真正導覽的連結；oracle、整合、dummy SDL、sanitizer 與 Xvfb 真實視窗證據見 [ACCEPTANCE.md](ACCEPTANCE.md)。
2. **HTTPS 鎖頭與地址欄寬度：已實作，`VALIDATING`。** 依 [計畫](docs/https-lock-plan.md) 完成；
   oracle（`tests/https_oracle_probe.py`）、HTTPS 整合、dummy SDL、sanitizer 與 Xvfb 真實視窗證據見
   [ACCEPTANCE.md](ACCEPTANCE.md)。
3. **Chrome 與 History：已實作，`VALIDATING`。** 依 [計畫](docs/history-plan.md) 完成：
   `tests/history_oracle_probe.py` 凍結 Python 答案，`tests/history_integration.py` 逐點比對
   native（30 個檢查點）；之後的載入失敗改為比照 Python 提交錯誤頁；地址草稿改為 active tab
   URL 改變時統一丟棄。證據見 [ACCEPTANCE.md](ACCEPTANCE.md)。
4. **驗收：完成（2026-09-28）。** 補上地址草稿的自動 oracle 比對（dummy SDL 分頁迴圈）與
   憑證錯誤後的 history 斷言；CTest、整套 ASan-UBSan-LSan、Xvfb 真實視窗串接操作與獨立審查
   都通過，證據見 [ACCEPTANCE.md](ACCEPTANCE.md)。剩餘差異都記在下表。整體 browser 尚未符合
   [專案狀態規則](.agents/skills/project-records/SKILL.md) 的 `COMPLETE`：外部開啟仍未完成。
   完整 chrome 視覺比對經使用者 2026-09-29 決定不做，外觀差異記在下表「Chrome 外觀」。
5. **新視窗（Ctrl+N）：已實作，`VALIDATING`（2026-09-29）。** 依 [計畫](docs/new-window-plan.md)
   拆出共用的 `TaiBrowserApp`（loader、cookie、書籤）與單一 SDL 迴圈管理多視窗；
   `tests/new_window_oracle_probe.py` 凍結 Python 9 個情境，`tests/new_window_integration.py`
   逐點比對 23 個檢查點。證據見 [ACCEPTANCE.md](ACCEPTANCE.md)。
6. **視窗標題跟隨頁面、改名 Tai Ci：已實作，`VALIDATING`（2026-09-29）。** 依
   [計畫](docs/window-title-plan.md) 新增 `tai_page_title()`（Python `Tab.get_title` 規則），每個
   `--window` 視窗顯示 active 分頁已提交頁面的標題；`tests/title_oracle_probe.py` 凍結 Python 8 個
   情境，`tests/title_integration.py` 比對 18 種 markup 與 32 個視窗檢查點。證據見
   [ACCEPTANCE.md](ACCEPTANCE.md)。

### 已知差異與範圍

| 項目 | 目前 native 行為與移植影響 |
|---|---|
| Tabs 上限與呈現 | 使用者指定最多 25 個 tabs；超寬時用等寬編號格。Python 無上限且讓文字換行。極窄格的可讀性仍待改善。 |
| 載入失敗與 pending | 載入失敗（首次或之後、網路或憑證）兩者都在請求 URL 顯示錯誤頁：一般導覽截斷 forward 項目，Back/Forward 停在目標 index（使用者 2026-09-26 決定，取代原回滾策略）。Pending 期間 native 仍顯示舊 page 與其 scroll；Python 開始導覽時 scroll 已歸零。可見 URL、history 與按鈕狀態在 pending 期間兩者相同。 |
| Pending 時按 Back | Python 已把 pending URL 寫入 history，Back 後它成為 forward 項目；native 取消尚未提交的導覽並丟掉該項目，Forward 不可用（使用者 2026-09-26 決定維持）。`tests/history_integration.py` 只對這兩個欄位套用此差異。 |
| 地址與外部開啟 | Native 拒絕 malformed/unsupported 直接網址，尚未啟動 `mailto:` 外部程式；Python 的 URL 解析與外部啟動不同。一般文字仍轉為 DuckDuckGo 查詢。 |
| 快捷鍵與 wheel | Native 在地址欄未聚焦時支援 Alt+Left/Alt+Right（Python 沒有 history 快捷鍵）；Ctrl+N 見下列「新視窗」；尚無 Escape 專用操作。未知 wheel direction 或非有限 y 為 no-op，與 frozen Python 不同。 |
| History 保存 | 兩者均保存 URL，Back/Forward 以 GET 重載（含 POST 結果頁與同頁 fragment 項目，重新 GET 後捲到 fragment）；不保存 POST body、舊 DOM 或 scroll snapshot。跨 tab 與 pending 狀態已由 history oracle 比對。 |
| Chrome 外觀 | 使用者 2026-09-29 決定 native 與 Python 各自保留自己的 chrome 外觀，不做完整視覺比對。控制項的位置、命中區、狀態與行為仍以 oracle 比對（`tabs_oracle_probe`、`tab_strip_differential`、`https_oracle_probe` 等）。已知外觀差異：Python 按鈕為 `browser.css` 的橘底加實線邊框，native 為淺灰立體按鈕；Back／Forward 圖示 Python 為 `<`／`>`，native 為箭頭；非作用中分頁連結 Python 純藍、native 深藍；網址列 Python 無邊框 16px 襯線字，native 有邊框 12px 無襯線字；native 在 chrome 底部多一條分隔線。同樣不再追的排版差異：窄寬 tab 標籤本身的排版仍是近似：Python 逐字換行（例如 84–119px、Tab 0 作用中時 Tab 1 整個移到第二行 `[0,39.2,37,55.2]`，<84px 時 `[Tab` 與 `0]` 分兩行），native 以固定規則放置標籤與命中區；<70px 的 Python 列高也未建模。 |
| Tab 列換行時的 viewport | Python 只在視窗 resize 或建立新 tab 時以當下 chrome bottom 計算 tab 高度，New Tab 造成換行後，既有 tab 的 viewport 仍是舊高度（下緣超出視窗 20px）；native 在換行狀態改變時立即把所有 tab 的 viewport 調成新 chrome bottom 以下的高度，讓捲動範圍與可見區一致。Python 依粗體／一般標籤混合，換行後列高另有 ≤0.14px 的差異，native 使用單一行高。 |
| HTTPS 鎖頭時機 | Python 導覽一開始就清除 `secure`，pending 期間沒有鎖頭；native 在新頁面 commit 前保留舊頁面的鎖頭（使用者於 2026-09-26 決定），延伸既有「pending 時顯示舊頁面」策略。載入失敗（含憑證錯誤）時兩者都顯示錯誤頁、沒有鎖頭。 |
| 測試信任根 | 本機 HTTPS 測試需要信任每次產生的 CA。Python oracle 以 `SSL_CERT_FILE` 設定；native 只透過測試用 `tai_network_set_ca_file()`／`tai_tabset_create_for_test()`，不讀環境變數，`tai-browser` 從不呼叫（使用者於 2026-09-26 決定）。 |
| 書籤控制 | Python 以單一 toolbar 星星（黃／白底）切換收藏，須手動輸入 `about:bookmarks` 看清單。Native 以地址欄內灰／金星切換收藏，並以地址欄左側獨立按鈕開啟清單；`about:bookmarks` 仍可直接輸入。可收藏條件、排序與逸出與 Python 相同。幾何見 [presentation 契約](docs/reference-presentation.md)。 |
| 書籤跨重啟保存 | Python 只在執行期間以 `set` 保存。使用者於 2026-09-26 選擇共用且跨重啟保存：native 寫入 `$XDG_DATA_HOME/tai-browser/bookmarks`（預設 `~/.local/share/tai-browser/bookmarks`），每次切換都原子寫入。檔案無法讀取或格式錯誤時不阻擋啟動，只在 stderr 警告、不覆寫原檔，本次改為只存在記憶體。多個 browser process 同時使用時後寫者覆蓋（無檔案鎖、不重讀）；寫入在點擊處理中同步執行。寫入中途崩潰可能留下 `bookmarks.tmp.*`，目前不會自動清除。 |
| 書籤連結 URL | Python 產生清單時 HTML 逸出 `href`，但其 parser 不解碼屬性，點擊含 `&` 的收藏會請求 `&amp;`（`tests/bookmarks_oracle_probe.py` 已記錄）。Native 只在內部書籤頁解碼 `href`，讓點擊請求原本收藏的 URL；一般網頁的屬性解析不變。 |
| 新視窗 | Ctrl+N 行為、共用 cookie／書籤、事件路由與關閉與 Python 相同（`tests/new_window_integration.py`）。使用者 2026-09-29 決定：最多 10 個視窗，達上限不動作（Python 無上限）；按住 Ctrl+N 的 key repeat 不開視窗（Python 會連續開）；建立失敗只在 stderr 報錯、既有視窗繼續（Python 崩潰）。新視窗開 app 的 New Tab URL，正式版與 Python 同為 `https://browser.engineering/`。 |
| 視窗標題與位置 | 標題規則與 Python 相同：active 分頁已提交頁面的第一個非空 `<title>`（只串接直接子文字、依 Python `str.strip()` 去空白），錯誤頁、書籤頁、pending、切換分頁與多視窗皆比對通過。使用者 2026-09-29 決定改名：沒有可用標題時 native 顯示 `Tai Ci`（Python `Tai Gar`），User-Agent 送 `Tai_Ci/1.0`（Python `Tai_Gar/1.0`）。腳本以 `innerHTML` 改寫標題（oracle 問題 12）已由 `tests/title_integration.py` 比對；點擊 listener 改寫標題由 `tests/js_dom_integration.py` 比對；視窗每次重繪都重算標題。視窗位置仍交給視窗系統（Python 置中，新視窗與舊視窗重疊），不在範圍內。 |
| Inline script（D6） | Python 只執行有 `src` 的 script；native 也執行 inline `<script>`，與外部 script 依 source 順序執行（使用者 2026-09-29 決定保留，真實網頁大量依賴）。頁面有有效 CSP `default-src` 時一律不執行 inline script，比照真實瀏覽器；`'unsafe-inline'`、nonce、hash 不支援，因為兩邊的 CSP 解析都只接受 URL 來源。影響：含 inline script 的頁面 DOM 可能與 Python 不同，oracle 比對頁面以外部 script 為主。證據：`tests/inline_script_integration.py`。 |
| Listener 丟錯（D7）與 JS 診斷輸出（D1） | Python 的 listener 丟錯會中斷整個派送：同節點其後 listener 與冒泡都不執行、先前的 `preventDefault` 失效、default action 照做。native 比照真實瀏覽器逐一隔離 listener：印出 `Event <type> crashed <error>` 後繼續，`preventDefault`／`stopPropagation` 保持有效（使用者 2026-09-29 決定）。凍結 `runtime.js` 不變，由 native 覆寫 `Node.prototype.dispatchEvent`。無法攔截的錯誤（2 秒上限中斷、記憶體不足）中止該次派送並照做 default action，同 Python。修正前 native 會把 listener 錯誤一路回傳，讓視窗事件迴圈結束。crash 訊息與 `log()` 寫到 stderr（Python stdout），因為 headless CLI 以 stdout 輸出 JSON；非字串的 `log` 值印成 JSON（`true`、`null`、`[1,"a"]`），Python 印成 repr（`True`、`None`、`[1, 'a']`）。影響：oracle 比對只對「丟錯 listener」情境套用此規則。證據：`tests/test_js.c`、`tests/test_browser.c`。 |
| 文件節點上限（D2） | Python 以 GC 回收脫離文件的節點；native 的節點由 `TaiDocument` 擁有到文件銷毀，JS handle 因此永遠有效。為避免腳本迴圈建立節點讓 C 堆無界成長，`createElement` 在文件已有 1,000,000 個節點時、`innerHTML = s` 在解析出的節點（含包裝用的 `html`／`head`／`body`）會讓文件超過 1,000,000 個節點時，丟 JS `Error('Document node limit reached')` 且樹不變（使用者 2026-09-29 決定）；頁面 HTML 解析不受此限。影響：只有建立百萬節點的頁面可觀察。證據：`tests/test_dom.c`、`tests/test_js.c`。 |
| 移除焦點節點（D8） | Python 在 `removeChild`／`innerHTML`／移動節點後仍把焦點留在脫離文件的 input：之後的按鍵照樣派送 `keydown` 並改寫它的 `value`，再接回文件時仍顯示為焦點。native 比照真實瀏覽器：被移除的子樹含焦點 input 時清除焦點（同一操作又把它接到別處也一樣），之後按鍵沒有目標；點擊 listener 把被點的 input 移出文件時也不聚焦（使用者 2026-09-29 指定為 JS DOM 切片 4 範圍）。另一個相關細節：點擊 listener 改動 DOM 後，native 以重建後的 layout 計算 caret，Python 以派送前命中的 input box 幾何量測新值；只有 listener 改變該 input 位置或字型時可觀察。影響：`tests/js_dom_integration.py` 只對 `focus.after_key.focus` 套用此規則。證據：`tests/test_browser.c`、`tests/js_dom_integration.py`。 |
| `innerHTML` 字串含 U+0000 | Python 把 NUL 當一般字元留在文字節點（`'a\u0000b<i>c</i>'` 解析成文字 `a\x00b` 與 `<i>`）；native HTML parser 以 NUL 結尾的 C 字串為輸入，`innerHTML` 設定值在第一個 NUL 處截斷（同例只剩文字 `a`）。影響：只有腳本刻意放入 NUL 的頁面可觀察；要消除須讓 parser 與 `TaiNode` 文字改為帶長度的字串。以臨時 oracle probe 實跑確認（切片 3 紀錄），未列入固定情境。 |
| 窄寬地址欄 | Tabbed Chrome 把地址欄寬度夾限為不超過視窗寬度，<100px 時仍看得到收藏星；Python 與單頁 Chrome 固定最小 100px，右端會超出視窗。安全頁面欄位右移 30px 後同樣夾限：Python 在 232–261px 與 <130px 時右端超出視窗，native 不超出。 |

書籤、新視窗與外部網址啟動是不同邊界；書籤、history 與新視窗已實作。精確幾何與事件路由在 [presentation 契約](docs/reference-presentation.md)，page/session/SDL/loader 所有權在 [native runtime](docs/architecture/native-runtime.md)。

## 子系統地圖

每列保留來源、目的地、依賴、狀態、證據與下一個缺口。`COMPLETE` 的門檻由 [project records](.agents/skills/project-records/SKILL.md) 定義；沒有整體驗收證據的列維持 `VALIDATING`。

| 子系統 | Python → C | 依賴 | 狀態 | 證據 | 下一個缺口 |
|---|---|---|---|---|---|
| Core / ownership | builtins → `src/core.c` | C17 | VALIDATING | map/string/file/JSON tests | allocation failure 與 destruction paths；C error returns 是刻意差異 |
| DOM / HTML | parser/nodes → `src/dom.c` | core, Unicode | VALIDATING | parser/mutation 與 DOM differential | JS DOM breadth、mutation、detached lifetime |
| CSS / style | cascade → `src/css.c` | DOM, core | VALIDATING | parser/cascade 與 CSS differential | [compatibility semantics](docs/architecture/compatibility-semantics.md) 中的未支援規則 |
| URL / HTTP | URL/cookie/referrer → `src/url.c`, `src/network.c` | core, libcurl multi | VALIDATING | URL differential、local HTTP integration | CORS/XHR/fetch、TLS/error limits、browser cancellation |
| Fonts / layout | block/line/text → `src/layout.c` | DOM, CSS, fonts, utf8proc | VALIDATING | layout differential、overflow tests | HarfBuzz/FriBidi、controls、完整 shaping/BiDi |
| Paint / raster | display/raster → `src/render.c` | Cairo, layout | VALIDATING | render differential、PNG/key-region tests | remote/general images、WebP；見 [raster 契約](docs/reference-display-raster.md) |
| JavaScript / events | JS runtime → `src/js.c` | QuickJS-NG, DOM, network | VALIDATING | bridge、cancellation、OOM tests | bubbling、mutation、timers/fetch；QuickJS OOM UAF 由 [tracked patch](patches/quickjs/0001-unlink-context-on-class-proto-oom.patch) 修補，待上游整合 |
| Scheduling | tasks/clocks → `src/scheduler.c` | threads, network | VALIDATING | priority/FIFO/aging/generation tests | browser/network/frame integration |
| Browser / window | app/tab/chrome → `src/browser.c`, `src/session.c`, `src/tabset.c`, `src/presentation*.c`, `src/main.c` | page, threads, network, Cairo, SDL3 | VALIDATING | [acceptance](ACCEPTANCE.md)、[tabs oracle](tests/tabs_oracle_probe.py)、[new-window oracle](tests/new_window_oracle_probe.py)、[title oracle](tests/title_oracle_probe.py)、native tab/window tests | `mailto:` 外部開啟 |

## 依工作分支讀取

- **Chrome 幾何、SDL input、視窗截圖：** [presentation 契約](docs/reference-presentation.md)。
- **History、tab/page、thread 與資源所有權：** [native runtime](docs/architecture/native-runtime.md)。
- **Python/C 行為比對與刻意差異：** [oracle 規約](.agents/skills/oracle-and-porting/SKILL.md) 與 [`tests/reference/browser.py`](tests/reference/browser.py)。
- **測試結果與整體完成宣告：** [ACCEPTANCE.md](ACCEPTANCE.md)。
- **已完成切片的歷史背景：** [handoff 紀錄](docs/handoff/)。

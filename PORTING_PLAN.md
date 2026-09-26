# C17 瀏覽器移植計畫

## 目前工作：Chrome 與 History

**狀態：`VALIDATING`。** `--window` 已有 tabs、地址列、Back/Forward、每個 tab 的 URL history、fragment、表單導覽與非同步載入。下一個切片補齊 Python 可見的 chrome 狀態，並檢查 history 在切換 tab、分支導覽與載入期間的行為。以 [`tests/reference/browser.py`](tests/reference/browser.py) 的 `Chrome`、`Tab`、`BrowserWindow` 為 oracle；已驗證的結果見 [ACCEPTANCE.md](ACCEPTANCE.md)。

### 下一個垂直切片

1. **兩顆星書籤：已實作，`VALIDATING`。** 依 [書籤實作計畫](docs/bookmarks-plan.md) 完成共用且跨重啟保存的收藏、網址列內灰／亮切換星、左側書籤清單入口與可真正導覽的連結；oracle、整合、dummy SDL、sanitizer 與 Xvfb 真實視窗證據見 [ACCEPTANCE.md](ACCEPTANCE.md)。
2. **HTTPS 鎖頭與地址欄寬度：已實作，`VALIDATING`。** 依 [計畫](docs/https-lock-plan.md) 完成；
   oracle（`tests/https_oracle_probe.py`）、HTTPS 整合、dummy SDL、sanitizer 與 Xvfb 真實視窗證據見
   [ACCEPTANCE.md](ACCEPTANCE.md)。
3. **續做 Chrome 與 History。** 對照 Python 的 Back/Forward 可用狀態、history 分支截斷、fragment、POST 後 traversal、dirty 地址草稿及 pending/失敗時的可見 URL；修正觀察到的缺口。既有後續載入失敗回滾依下表保留並測試。完成條件：每一項都有可重跑的 oracle/native 對照，active/inactive tabs 的 URL、index、按鈕狀態和請求方法互不串線。
4. **驗收。** 建置、相關 CTest、Python oracle、代表性原生視窗與適用的 sanitizer 均通過；將結果寫入 [ACCEPTANCE.md](ACCEPTANCE.md)。完成條件：上述 chrome/history 情境有通過證據、資源清理已審查，且剩餘差異在本計畫有唯一紀錄。整體 browser 仍依 [專案狀態規則](.agents/skills/project-records/SKILL.md) 判定是否可升為 `COMPLETE`。

### 已知差異與範圍

| 項目 | 目前 native 行為與移植影響 |
|---|---|
| Tabs 上限與呈現 | 使用者指定最多 25 個 tabs；超寬時用等寬編號格。Python 無上限且讓文字換行。極窄格的可讀性仍待改善。 |
| 載入失敗與 pending | 已有文件的後續 navigation 失敗時 native 保留舊 page/URL/history，pending 時舊 page/scroll 仍可見；Python 在開始時更新 URL/history、scroll 歸零，失敗顯示 Network Error。首次失敗兩者均保留請求 URL/history 並顯示錯誤頁。這是目前刻意保留的回滾策略。 |
| 地址與外部開啟 | Native 拒絕 malformed/unsupported 直接網址，尚未啟動 `mailto:` 外部程式；Python 的 URL 解析與外部啟動不同。一般文字仍轉為 DuckDuckGo 查詢。 |
| 快捷鍵與 wheel | Native 在地址欄未聚焦時支援 Alt+Left/Alt+Right；尚無 Ctrl+N／新視窗／Escape 專用操作。未知 wheel direction 或非有限 y 為 no-op，與 frozen Python 不同。 |
| History 保存 | 兩者均保存 URL，Back/Forward 以 GET 重載；不保存 POST body、舊 DOM 或 scroll snapshot。此項是既有契約，下一切片驗證跨 tab 與 pending 狀態。 |
| Chrome 缺口 | 完整 chrome 視覺比對尚待完成。窄寬 tab 標籤本身的排版仍是近似：Python 逐字換行（例如 84–119px、Tab 0 作用中時 Tab 1 整個移到第二行 `[0,39.2,37,55.2]`，<84px 時 `[Tab` 與 `0]` 分兩行），native 以固定規則放置標籤與命中區；<70px 的 Python 列高也未建模。 |
| Tab 列換行時的 viewport | Python 只在視窗 resize 或建立新 tab 時以當下 chrome bottom 計算 tab 高度，New Tab 造成換行後，既有 tab 的 viewport 仍是舊高度（下緣超出視窗 20px）；native 在換行狀態改變時立即把所有 tab 的 viewport 調成新 chrome bottom 以下的高度，讓捲動範圍與可見區一致。Python 依粗體／一般標籤混合，換行後列高另有 ≤0.14px 的差異，native 使用單一行高。 |
| HTTPS 鎖頭時機 | Python 導覽一開始就清除 `secure`，pending 期間沒有鎖頭；native 在新頁面 commit 前保留舊頁面的鎖頭（使用者於 2026-09-26 決定），延伸既有「pending 時顯示舊頁面」策略。之後的載入失敗（含憑證錯誤）回滾到舊 HTTPS 頁面時鎖頭跟著恢復；Python 顯示錯誤頁、沒有鎖頭。首次失敗兩者都沒有鎖頭。 |
| 測試信任根 | 本機 HTTPS 測試需要信任每次產生的 CA。Python oracle 以 `SSL_CERT_FILE` 設定；native 只透過測試用 `tai_network_set_ca_file()`／`tai_tabset_create_for_test()`，不讀環境變數，`tai-browser` 從不呼叫（使用者於 2026-09-26 決定）。 |
| 書籤控制 | Python 以單一 toolbar 星星（黃／白底）切換收藏，須手動輸入 `about:bookmarks` 看清單。Native 以地址欄內灰／金星切換收藏，並以地址欄左側獨立按鈕開啟清單；`about:bookmarks` 仍可直接輸入。可收藏條件、排序與逸出與 Python 相同。幾何見 [presentation 契約](docs/reference-presentation.md)。 |
| 書籤跨重啟保存 | Python 只在執行期間以 `set` 保存。使用者於 2026-09-26 選擇共用且跨重啟保存：native 寫入 `$XDG_DATA_HOME/tai-browser/bookmarks`（預設 `~/.local/share/tai-browser/bookmarks`），每次切換都原子寫入。檔案無法讀取或格式錯誤時不阻擋啟動，只在 stderr 警告、不覆寫原檔，本次改為只存在記憶體。多個 browser process 同時使用時後寫者覆蓋（無檔案鎖、不重讀）；寫入在點擊處理中同步執行。寫入中途崩潰可能留下 `bookmarks.tmp.*`，目前不會自動清除。 |
| 書籤連結 URL | Python 產生清單時 HTML 逸出 `href`，但其 parser 不解碼屬性，點擊含 `&` 的收藏會請求 `&amp;`（`tests/bookmarks_oracle_probe.py` 已記錄）。Native 只在內部書籤頁解碼 `href`，讓點擊請求原本收藏的 URL；一般網頁的屬性解析不變。 |
| 窄寬地址欄 | Tabbed Chrome 把地址欄寬度夾限為不超過視窗寬度，<100px 時仍看得到收藏星；Python 與單頁 Chrome 固定最小 100px，右端會超出視窗。安全頁面欄位右移 30px 後同樣夾限：Python 在 232–261px 與 <130px 時右端超出視窗，native 不超出。 |

書籤、新視窗與外部網址啟動是不同邊界；書籤已實作，目前續做 history。精確幾何與事件路由在 [presentation 契約](docs/reference-presentation.md)，page/session/SDL/loader 所有權在 [native runtime](docs/architecture/native-runtime.md)。

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
| Browser / window | app/tab/chrome → `src/browser.c`, `src/session.c`, `src/tabset.c`, `src/presentation*.c`, `src/main.c` | page, threads, network, Cairo, SDL3 | VALIDATING | [acceptance](ACCEPTANCE.md)、[tabs oracle](tests/tabs_oracle_probe.py)、native tab/window tests | 目前 Chrome 與 History 切片；之後新視窗、外部開啟及剩餘視覺差異 |

## 依工作分支讀取

- **Chrome 幾何、SDL input、視窗截圖：** [presentation 契約](docs/reference-presentation.md)。
- **History、tab/page、thread 與資源所有權：** [native runtime](docs/architecture/native-runtime.md)。
- **Python/C 行為比對與刻意差異：** [oracle 規約](.agents/skills/oracle-and-porting/SKILL.md) 與 [`tests/reference/browser.py`](tests/reference/browser.py)。
- **測試結果與整體完成宣告：** [ACCEPTANCE.md](ACCEPTANCE.md)。
- **已完成切片的歷史背景：** [handoff 紀錄](docs/handoff/)。

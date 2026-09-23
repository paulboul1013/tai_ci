# C17 移植狀態

## 使用方式

這份文件是移植工作的導航，不是行為契約或歷史日誌。先從「目前切片」完成一個可驗證的垂直路徑，再依下表選擇下一個 subsystem。每次變更只更新受影響列的 state、evidence 與 gap。

`COMPLETE` 僅在功能、build、相關測試、Python oracle、ownership review、差異記錄與上下游整合皆有證據時使用；其餘進行中的可執行切片維持 `VALIDATING`。完整定義見 [`docs/agents/project-records.md`](docs/agents/project-records.md)。

## 目前切片：window 單視窗 chrome、網址列與 history

**State:** `VALIDATING`

`tai-browser --window URL` 的目前範圍是單視窗 chrome、可編輯地址列、Back/Forward 與 URL history；既有頁面 click、表單輸入/提交、同視窗文件導覽、fragment 與 scroll 路徑仍整合於此視窗。精確幾何、viewport、事件路由與呈現契約見 [render contract](docs/reference-render-contract.md)；page/session、SDL 資源與 callback ownership 見 [native runtime](docs/architecture/native-runtime.md)。本切片維持 `VALIDATING`，整體驗收仍未完成。

Python 參考實作另有 tabs、bookmark、新視窗及外部網址啟動，這些不屬於本切片。窄寬地址欄會被裁切且 bookmark row 尚未實作；tabs 是下一個切片。

**刻意差異與限制：** malformed/unsupported direct address 會被 native 拒絕；`mailto:` 不啟動外部程式；載入失敗時 native 保留舊 page/history，而 Python 會先更新 URL/history 並顯示 Network Error。Back/Forward 只保存 URL 並以 GET 重載，不保存 POST body、舊 DOM 或 scroll。Native 支援地址列未聚焦時 Alt+Left/Alt+Right；沒有 Ctrl+N、新視窗或 Escape 專用操作。Frozen Python 對未知 wheel direction 與非有限 y 的處理和 native no-op 不同。低寬度控件可用性、chrome pixel diff、自动化原生視窗輸入順序仍是缺口；Python event dispatcher 未見 Escape 分支。行為細節見 [render contract](docs/reference-render-contract.md)。

`TaiPage` 提供 navigation intent；session/window owner 負責候選頁與 history 的提交。具體狀態、已知差異與下一個移植 seam 由本計畫追蹤；驗證結果只記錄於 [ACCEPTANCE.md](ACCEPTANCE.md)，chrome 幾何與測試案例清單記錄於 [render contract](docs/reference-render-contract.md)。

**既有輸入/history 邊界與下一個 seam：** 文字/password、checkbox、button/Enter 表單、同視窗文件替換、fragment、page scroll/wheel 與 session history 均已納入目前 window slice。尚未涵蓋 tabs、bookmark、新視窗與 mailto 外部啟動；地址列低寬度可用性、native chrome pixel diff、自動化原生視窗輸入順序及配置故障注入仍待驗證。舊 `tai_present_window` 與 `tai_present_window_with_history` 仍提供不帶可見 chrome 的相容入口。

## 下一個切片：tabs

Python `BrowserWindow` 擁有有序 tabs 與 active tab；Chrome 的 New Tab 按鈕建立預設首頁 `https://browser.engineering/`，Tab N 連結切換 active tab，Back/Forward/地址列作用於目前 tab。參考 UI 未找到單一 tab 關閉控制；關閉視窗會關閉其所有 tabs。開始實作前先以 Python oracle 固定 tab row 的高度／窄寬排版與點擊邊界。

1. **定義 tab owner 與生命週期。** 建立 window/tab-set 邊界，讓每個 tab 擁有獨立 `TaiSession`（live page、history、scroll），由 window 共用 network、default CSS 與尺寸。驗收：切換 tab 不複製 `TaiPage *` 或重用另一 tab 的 history；window close 逐一釋放所有 session。
2. **新增與切換 tab 的垂直路徑。** 點 New Tab 建立並選取預設首頁 tab；點 Tab N 切換目前內容、網址列與 Back/Forward availability。驗收：兩個 tab 可各自導覽和返回，來回切換後 page URL、scroll、history index 保持隔離；關閉窗口完整清理。
3. **補齊 oracle 與整合驗證。** Dummy SDL 覆蓋新建、切換、toolbar/page hit boundary 和窄寬排列；本地 HTTP fixture 驗證各 tab 的 history/referrer；完成 focused/full CTest、ASan/UBSan、Python 幾何比對與原生手動 smoke test。Bookmark、新視窗和單 tab close 不擴入此切片，除非 oracle 顯示屬於 tab 基本契約。

## Subsystem dashboard

| Subsystem | Python → C destination | Dependencies | State | Evidence | Gap / next seam |
|---|---|---|---|---|---|
| Core / ownership | builtins → `src/core.c` | C17 | VALIDATING | map unit tests；shared string/file/JSON primitives | allocation-failure 與 destruction paths；C error returns are intentional |
| DOM / HTML | `browser.py` parser/nodes → `src/dom.c` | core, Unicode | VALIDATING | parser/mutation unit；normalized DOM differential | JS DOM breadth、mutation differential、detached lifetime |
| CSS / style | parser/selectors/style → `src/css.c` | DOM, core | VALIDATING | parser/cascade unit；CSS differential | supported subset and compatibility rules → [`compatibility semantics`](docs/architecture/compatibility-semantics.md) |
| URL / HTTP | URL/cookie/referrer → `src/url.c`, `src/network.c` | core, libcurl multi | VALIDATING | URL differential；local HTTP integration | CORS/XHR/fetch、header/TLS/error limits、browser-level cancellation |
| Fonts / layout | document/block/line/text → `src/layout.c` | DOM, CSS, FreeType, fontconfig, utf8proc | VALIDATING | geometry/layout differentials；overflow clamp tests | HarfBuzz/FriBidi, controls, complete shaping/BiDi |
| Paint / raster | display commands/raster → `src/render.c` | Cairo, layout | VALIDATING | structural differentials；scroll/blur/blend/image key-region tests；viewport PNG | general/remote images and WebP; exact scope → [`render contract`](docs/reference-render-contract.md) |
| JavaScript / events | JS runtime/context → `src/js.c` | QuickJS-NG, DOM, CSS, network | VALIDATING | bridge, cancellation, exception tests | bubbling, broad DOM mutation, timers/fetch, browser integration |
| Scheduling | task runners/clocks → `src/scheduler.c` | threads, network | VALIDATING | priority/FIFO/aging/generation unit tests | browser/network/frame integration and close protocol |
| Browser / window | app/window/tab/chrome → `src/browser.c`, `src/session.c`, `src/presentation.c`, `src/main.c` | page, Cairo, SDL3 | VALIDATING | verification record: [`ACCEPTANCE.md`](ACCEPTANCE.md); geometry and presentation cases: [`render contract`](docs/reference-render-contract.md) | low-width address/control clipping and unimplemented bookmark row; native chrome pixel diff, tabs/new window, mailto external launch, automated real-window keyboard/event-order validation |

## Disclosure map

Read the indicated source only when its branch is active:

| Trigger | Authoritative reference |
|---|---|
| Observable Python/C behavior or intentional divergence | [`docs/agents/oracle-and-porting.md`](docs/agents/oracle-and-porting.md) |
| Page, SDL, thread, texture, display-list ownership | [`docs/architecture/native-runtime.md`](docs/architecture/native-runtime.md) |
| Rendering coordinates, supported paint, screenshot/window behavior | [`docs/reference-render-contract.md`](docs/reference-render-contract.md) |
| CSS compatibility difference | [`docs/architecture/compatibility-semantics.md`](docs/architecture/compatibility-semantics.md) |
| Whole-project completion claim | [`ACCEPTANCE.md`](ACCEPTANCE.md) |
| Historical slice context or a resumed incomplete slice | [`docs/handoff/`](docs/handoff/) |

## Evidence constraints

- The Python browser is the behavioral authority; compare at the affected boundary before declaring equivalence.
- Keep `ARCHITECTURE.md` as the directory index and put architecture, rendering, and acceptance details in their disclosed references rather than duplicating them here.

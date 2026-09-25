# C17 移植狀態

## 使用方式

這份文件是移植工作的導航，不是行為契約或歷史日誌。先從「目前切片」完成一個可驗證的垂直路徑，再依下表選擇下一個 subsystem。每次變更只更新受影響列的 state、evidence 與 gap。

`COMPLETE` 僅在功能、build、相關測試、Python oracle、ownership review、差異記錄與上下游整合皆有證據時使用；其餘進行中的可執行切片維持 `VALIDATING`。完整定義見 [`docs/agents/project-records.md`](docs/agents/project-records.md)。

## 目前切片：tabbed window、chrome 與 history

**State:** `VALIDATING`

`tai-browser --window URL` 現在進入 tabbed window，提供可編輯地址列、Back/Forward、URL history、New Tab、Tab N selection，以及作用於 active tab 的 page click、表單輸入/提交、fragment 與 scroll。tab documents 和 CSS 在 loader thread 建構，SDL owner 持續處理輸入、resize 與 close。原有單頁 chrome APIs 仍保留供相容呼叫。本切片維持 `VALIDATING`；低寬度／多 tab 視覺排版、native chrome pixel comparison 與 native X11/Wayland 下完整事件順序仍待補足。SDL dummy integration 已在 delayed document/CSS 載入中注入 tab 操作及 close；TabSet tests 另覆蓋 resize、failure routes、Referer 與 late completion。精確幾何、viewport、事件路由與呈現契約見 [presentation contract](docs/reference-presentation.md)；page/session、SDL 資源與 callback ownership 見 [native runtime](docs/architecture/native-runtime.md)。整體驗收仍未完成。

Python 參考實作另有 bookmark、新視窗及外部網址啟動，這些不屬於本切片。窄寬地址欄會被裁切且 bookmark row 尚未實作。

**刻意差異與限制：** malformed/unsupported direct address 會被 native 拒絕；`mailto:` 不啟動外部程式；已有文件後續 navigation 載入失敗時 native 保留舊 page/history，而 Python 會先更新 URL/history 並顯示 Network Error。初次載入失敗時兩者都提交 Network Error page 與請求 URL/history。Back/Forward 只保存 URL 並以 GET 重載，不保存 POST body、舊 DOM 或 scroll。Native 支援地址列未聚焦時 Alt+Left/Alt+Right；沒有 Ctrl+N、新視窗或 Escape 專用操作。Frozen Python 對未知 wheel direction 與非有限 y 的處理和 native no-op 不同。低寬度控件可用性、chrome pixel diff、自动化原生視窗輸入順序仍是缺口；Python event dispatcher 未見 Escape 分支。行為細節見 [presentation contract](docs/reference-presentation.md)。

`TaiPage` 提供 navigation intent；session/window owner 負責候選頁與 history 的提交。具體狀態、已知差異與下一個移植 seam 由本計畫追蹤；驗證結果只記錄於 [ACCEPTANCE.md](ACCEPTANCE.md)，chrome 幾何與測試案例清單記錄於 [presentation contract](docs/reference-presentation.md)。

**既有輸入/history 邊界與下一個 seam：** 文字/password、checkbox、button/Enter 表單、同視窗文件替換、fragment、page scroll/wheel、session history 與 tabs 均已納入目前 window slice。Bookmark、新視窗與 mailto 外部啟動不在本切片；地址列低寬度可用性、native chrome pixel diff、自動化原生視窗輸入順序及配置故障注入仍待驗證。舊 `tai_present_window` 與 `tai_present_window_with_history` 仍提供不帶可見 chrome 的相容入口。

## Tabs slice contract and acceptance

Python oracle 是 `tests/reference/browser.py`。`BrowserWindow` 擁有有序 tabs 和 active tab；Chrome 的 New Tab 建立 `https://browser.engineering/`，Tab N 連結切換 active tab，Back/Forward/地址列作用於目前 tab。Python 每個 tab 有獨立 page、history、scroll 和 task runner；載入失敗會以 Network Error 文件呈現並保留 URL/history。Python resize 更新全部 tabs。參考 UI 未找到單 tab close 控制；window close 關閉所有 tabs。

**本切片行為契約：**

- New Tab 立即成為 active，並以 `https://browser.engineering/` 作為第一個 navigation/history entry。
- Navigation 一開始就讓 pending URL/history 在其 tab 的網址列與 history state 可見；成功時轉為 committed entry。已有文件的後續載入失敗時，暫存 entry 回復，保持本專案延續的 page/history 失敗差異。
- 頂層文件載入不得阻塞 SDL window event loop；以 `--window URL` 啟動時，初始文件尚未完成也要先顯示可操作的 window。載入期間仍可選取或建立 tabs、resize、close；完成結果交回發起該 navigation 的 tab，即使 active tab 改變也不得更新其他 tab。
- 文件載入及其子資源請求不得在 SDL event loop 同步等待網路；所有完成通知須依已定義的 network owner/thread contract 安全送回 page/session owner。
- 新 tab 或 `--window URL` 的首次 navigation 失敗時，依 Python oracle 顯示 Network Error 文件並保留該次請求 URL/history；probe 需固定可比較的畫面與 history 證據。
- 已有文件的 tab 後續 navigation 失敗時，延續目前 native 的刻意差異：保留舊 page、URL、history 與 index；Python 則更新 URL/history 並顯示 Network Error。兩種失敗狀態都只能影響其目標 tab，並各自驗證。
- 切換 tab 或建立 New Tab 會丟棄 dirty address draft。Page click/navigation、address、Back/Forward 都作用於 active tab。
- 每個 tab 的 page、URL history、document scroll 保持隔離。Window resize 更新所有 tabs 的 viewport，但保留 tab-level document scroll offset，即使它超過新最大值；凍結的 Python `Tab.resize`/layout/commit 路徑不會 clamp，之後明確 scroll 操作才套用新範圍。
- **Pending navigation difference:** native keeps the last committed page and scroll visible while the replacement is pending; Python assigns the requested URL and resets scroll to zero at `Tab.load` start. Native keeps the old page/scroll after a later transport failure because the requested slice rolls back that navigation. This is an intentional extension of the documented rollback difference.
- Navigation Referer is captured from the committed page URL, or from the prior provisional URL when a new request supersedes a pending navigation, matching Python's capture-before-assignment behavior.
- 不提供單 tab close；window close 負責關閉並清理所有 tabs。Bookmark 與新視窗不屬於本切片。

**實作順序與驗收：**

1. **凍結並保存 Python oracle。**

   - 建立可重複執行的 probe/test fixture，記錄 tab row 高度、窄寬排版、New Tab 與 Tab N hit boundary、active 樣式、切換時的 address draft、背景 tabs resize/scroll，以及載入期間和失敗時的可見狀態。
   - 使用可控延遲的 local HTTP fixture，驗證 Python 在 `--window URL` 初始載入及後續 pending load 期間仍可操作 window；再讓外部 CSS 或 JavaScript 回應延遲，確認子資源載入期間也可操作。以 transport failure 分別記錄首次 navigation 與已有文件 tab 的 URL/history/page 結果。
   - **驗收：** oracle probe 可重跑並保留比較輸出；在 probe 固定前不定 native 幾何或改寫 C interface。
   - **依賴：** 無。

2. **定義 tab-set module 與 async load seam。**

   - Window owner 持有有序 tab slots 與 active index；每個 slot 唯一擁有一個 `TaiSession`。`TaiTabSet` 擁有 loader thread 和共享 `TaiNetwork`；thread 在啟動後建立、獨佔使用並釋放 network。Tab set 借用 default CSS；SDL owner 持有 sessions，loader 暫時擁有載入中的 page，completion 只在 owner thread 驗證 ID/generation 後轉移 page ownership。
   - 在 presentation adapter 與 owner module 間定義小而可測的 interface：tab commands 和 active-page/history state 經此 seam 路由，session array 與 load machinery 保留在 owner 內部。
   - Pending navigation 綁定發起 tab 的穩定身分與 generation；completion 不得只查當時的 active index。定義 initial page 未完成時的 tab/session state、failed construction、cancellation、shutdown，以及 DOM/page 與 SDL state 的 owner thread。
   - 先追蹤 `TaiNetwork` 的 `submit/poll` 和同步 `tai_network_request`，再選實作方式；`TaiScheduler` 目前尚未接上 window/network orchestration（見 `ARCHITECTURE.md`）。明確定義 SDL event delivery 如何與 network progress 同時前進、request/response callback 的 owner、completion dispatch/wakeup，以及取消和關閉順序。不得讓文件或子資源的同步網路等待卡住 SDL event loop，也不得從非 SDL owner thread 操作 window/renderer。
   - **驗收：** owner interface 有 session creation/selection、active navigation target、completion target、viewport update 和 shutdown 的可測契約；network/event-loop owner 與 dispatch/wakeup 策略明確；跨 thread/owner 的 borrow、transfer、cancellation 與 partial construction cleanup 均有定義。`--window URL` 延遲啟動 probe 證明 response 前 window 已可操作；不得以 event-loop 同步請求作 fallback。
   - **依賴：** Task 1。

3. **完成 nonblocking navigation 和 New Tab 載入路徑。**

   - New Tab 立即建立並選取 loading tab；network 等待在 event loop 之外完成，response/result 回到正確 tab owner。不得機械複製 Python 每 tab thread；保留其可觀察的非阻塞行為和狀態隔離即可。
   - Navigation 啟動即提供 pending URL 和暫存 history index；載入成功後以 page/session commit 取代暫存狀態。新 tab 或 `--window URL` 的首次 navigation transport failure 顯示 Network Error 文件並保留該 URL/history，符合 Python。已有頁面的 tab 若後續 navigation 失敗，丟棄暫存 entry、保留舊 page/URL/history/index，記錄並測試此刻意差異。
   - **驗收：** 延遲 fixture 尚未釋放時，window 仍能切換/建立 tabs、resize 和 close；釋放後結果只更新發起它的 tab。另以延遲外部 CSS/JavaScript 回應確認子資源 pending 期間 window 仍可操作。也覆蓋 `--window URL` 初始延遲/失敗、新 tab 首次失敗、已有頁面的 link/address/Back/Forward 失敗，以及已取消/已關閉 tab 的 late completion。
   - **依賴：** Task 2。

4. **新增 tab row 和 active-tab routing。**

   - 依 oracle 建立 New Tab 與 Tab N 顯示/點擊路徑；切換時同步 page、active URL、Back/Forward availability 和 page input target。切換 tab 和 New Tab 時清除 dirty address draft。
   - **驗收：** 至少兩個 tabs 反覆切換並各自導航、Back/Forward；URL、history index、scroll 與 pending navigation completion 都保持 tab 隔離。Dummy SDL 輸入測試以後續地址列提交的 URL 驗證 New Tab/tab switch 會丟棄 dirty draft；`test_presentation` 驗證 New Tab/tab-link hit boundaries、tab count 與 active index。Native chrome active-style pixel comparison、真實視窗的事件順序與 toolbar/page 分界仍是待補的視覺／事件證據。
   - **依賴：** Task 3。

5. **整合 resize、lifecycle 和完整驗證。**

   - 每次有效 resize 更新所有 tab page viewport，同時依 Python oracle 保留各 tab document scroll offset；切換 away/back 後每個頁面使用新尺寸。Window close 停止/取消所有 pending work 並逐一釋放 session；部分初始化失敗也完整清理。
   - **驗收：** local HTTP fixture 驗證 per-tab history/referrer、delayed/failing document and external subresource responses、window 在 document/subresource 載入時可操作及 late completion 安全；focused/full CTest、ASan/UBSan、Python geometry/event comparison 與原生手動 smoke test 通過。驗證結果寫入 `ACCEPTANCE.md`，architecture/ownership 只在 `docs/architecture/native-runtime.md` 有實作證據後更新。
   - **依賴：** Task 4。

Bookmark、新視窗和單 tab close 不擴入此切片，除非 oracle 顯示它們是 tab row 的必要契約。舊單頁 `TaiSession` API 仍以同步 `tai_network_request` 載入；新的 `--window` tabs path 由 `TaiTabSet` loader thread 使用 `TaiNetwork` submit/poll 非同步完成文件和子資源，不會在 SDL event loop 使用同步 fallback。不要機械複製 Python 每 tab thread；保持其可觀察的非阻塞與狀態隔離行為即可。單 tab close 由 window close 一併清理，不提供未經 oracle 證明的控制項。

## Subsystem dashboard

| Subsystem | Python → C destination | Dependencies | State | Evidence | Gap / next seam |
|---|---|---|---|---|---|
| Core / ownership | builtins → `src/core.c` | C17 | VALIDATING | map unit tests；shared string/file/JSON primitives | allocation-failure 與 destruction paths；C error returns are intentional |
| DOM / HTML | `browser.py` parser/nodes → `src/dom.c` | core, Unicode | VALIDATING | parser/mutation unit；normalized DOM differential | JS DOM breadth、mutation differential、detached lifetime |
| CSS / style | parser/selectors/style → `src/css.c` | DOM, core | VALIDATING | parser/cascade unit；CSS differential | supported subset and compatibility rules → [`compatibility semantics`](docs/architecture/compatibility-semantics.md) |
| URL / HTTP | URL/cookie/referrer → `src/url.c`, `src/network.c` | core, libcurl multi | VALIDATING | URL differential；local HTTP integration | CORS/XHR/fetch、header/TLS/error limits、browser-level cancellation |
| Fonts / layout | document/block/line/text → `src/layout.c` | DOM, CSS, FreeType, fontconfig, utf8proc | VALIDATING | geometry/layout differentials；overflow clamp tests | HarfBuzz/FriBidi, controls, complete shaping/BiDi |
| Paint / raster | display commands/raster → `src/render.c` | Cairo, layout | VALIDATING | structural differentials；scroll/blur/blend/image key-region tests；viewport PNG | general/remote images and WebP; exact scope → [`display/raster contract`](docs/reference-display-raster.md) |
| JavaScript / events | JS runtime/context → `src/js.c` | QuickJS-NG, DOM, CSS, network | VALIDATING | bridge, cancellation, exception tests | bubbling, broad DOM mutation, timers/fetch, browser integration |
| Scheduling | task runners/clocks → `src/scheduler.c` | threads, network | VALIDATING | priority/FIFO/aging/generation unit tests | browser/network/frame integration and close protocol |
| Browser / window | app/window/tab/chrome → `src/browser.c`, `src/session.c`, `src/tabset.c`, `src/presentation.c`, `src/main.c` | page, threads, libcurl multi, Cairo, SDL3 | VALIDATING | verification record: [`ACCEPTANCE.md`](ACCEPTANCE.md); Python tabs oracle: [`tabs_oracle_probe.py`](tests/tabs_oracle_probe.py) + [`tabs_oracle.json`](tests/fixtures/tabs_oracle.json); native tabset/presentation tests | intentional later-navigation failure rollback; low-width/tab-count clipping, bookmark and new-window support, mailto external launch, native chrome pixel diff, real-window input sequence |

## Disclosure map

Read the indicated source only when its branch is active:

| Trigger | Authoritative reference |
|---|---|
| Observable Python/C behavior or intentional divergence | [`docs/agents/oracle-and-porting.md`](docs/agents/oracle-and-porting.md) |
| BrowserWindow/Tab concepts and observable state | [`docs/architecture/python-reference.md`](docs/architecture/python-reference.md) |
| Page, SDL, thread, texture, display-list ownership | [`docs/architecture/native-runtime.md`](docs/architecture/native-runtime.md) |
| Layout/font measurement and rounding | [`docs/reference-layout-fonts.md`](docs/reference-layout-fonts.md) |
| Display coordinates, paint, raster and headless screenshot | [`docs/reference-display-raster.md`](docs/reference-display-raster.md) |
| SDL window, Chrome, tab input and window screenshot | [`docs/reference-presentation.md`](docs/reference-presentation.md) |
| CSS compatibility difference | [`docs/architecture/compatibility-semantics.md`](docs/architecture/compatibility-semantics.md) |
| Whole-project completion claim | [`ACCEPTANCE.md`](ACCEPTANCE.md) |
| Historical slice context or a resumed incomplete slice | [`docs/handoff/`](docs/handoff/) |

## Evidence constraints

- The Python browser is the behavioral authority; compare at the affected boundary before declaring equivalence.
- Keep `ARCHITECTURE.md` as the directory index and put architecture, rendering, and acceptance details in their disclosed references rather than duplicating them here.

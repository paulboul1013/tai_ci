# tai_gar runtime / orchestration 分析

來源：`/home/paulboul/tai_gar/browser.py`、`runtime.js`、`server.py`、`web_server.py`。本報告僅根據 source 閱讀，尚未執行 oracle。

## 入口與版本辨識

`browser.py:9843` 是目前 Browser executable：解析 `--rtl` 及初始 URL → BrowserApp → new_window → new_tab → 非同步 load → app.run。無參數載入 https://browser.engineering/。`server.py` 其實是另一份較舊 Browser monolith（7,599 行），不是服務端；`web_server.py` 才是 port 8000 留言板 HTTP server。應以目前 browser.py 為首要 oracle，不能將 server.py 的額外 runtime 能力自動合併成現況。

## Dependency graph

```text
BrowserApp → SDL event/presentation、MeasureTime、NetworkTaskRunner、RasterAndDrawRunner、BrowserWindow
BrowserWindow → Chrome、Tab、CommitData、FrameClock、RasterWork/RasterResult
Tab → TaskRunner、URL/NetworkTaskRunner、HTMLParser/DOM、CSSParser/style、JSContext、DocumentLayout/paint
JSContext → DOM handles、CSS selectors、HTML serializer/parser、Tab invalidation、NetworkTaskRunner、timer producers
TaskRunner → TaskPriority queues、MeasureTime、BrowserWindow frame deadline heuristic
NetworkTaskRunner → NetworkTask queue → blocking-I/O workers → completion queue → Tab TaskRunner
RasterAndDrawRunner → immutable work snapshots → RasterWindowState/Skia surfaces → immutable pixel result → BrowserWindow/SDL
web_server.py → socket、session/cookie/CSRF、message_board.json；不被 Browser import
```

## 執行緒與 ownership

* Process 起始 Browser thread 擁有 SDL、native window lifetime、Chrome DOM/layout/hit testing；僅它做 SDL present。
* 每個 Tab 擁有一條非搶占 Main thread；頁面 DOM、JS interpreter、style/layout、focus/history/scroll 在此 serialized。
* 一條 process-wide Networking thread 擁有 dispatch 與 completion routing；每請求短生命 blocking-I/O worker。Completion 只能傳結果或排入 Tab task，不可直接改 DOM。
* 一條 process-wide Raster-and-draw thread（CPU threaded mode）擁有 Skia raster surfaces；sync baseline 以同實作 inline 執行。GPU POC 的 GL/Skia GPU context 則留 Browser thread。
* timer worker 只 enqueue JS timer 或 frame task，不能 eval JS。JS interval 使用 lock + stop event；navigation discard 讓舊 callbacks no-op。
* Window RLock 保護 committed states、dirty bits、active Tab、frame gate、scene epoch。TaskRunner condition 執行任意 task 前放鎖；frame guard 讀 window deadline 採 heuristic lock-free 以免反向鎖序。
* CommitData 是 Tab → Browser rendering snapshot。新 display list 只有 commit 被接受後才從 Tab 移交並設 None；scroll-only commit 的 None 表示重用前一次 list。Hit testing 可以重建 local list，但不因此設 dirty commit。
* Python snapshot 的 paint command 仍保留 layout_object → DOM reference；middle-click committed_link_at 甚至回溯 node parent。C 不可把 Python GC 隱含 lifetime 視為可直接 free：須 scene 保留 document generation，或 snapshot 自帶 hit-test/action 資料。
* 原程式 shared visited/bookmarks、cookie/cache globals 依 Python 操作與 GIL；C 必須建立明確 app/network ownership 或同步。

## Task 排程語意

P0 RENDER、P1 INPUT、P2 NORMAL、P3 JS_TIMER。每 level FIFO；可選 global FIFO baseline。Priority 模式先 render、再 input；timer 未受 frame guard 保護時，可因 starvation age 或 foreground burst quota 超過而優於 NORMAL，其餘 NORMAL 後才 timer。僅下一個 task 按 priority 決定，正在跑的 JS 不可搶占。Task.run finally 釋放 callback/args references，trace begin/end 平衡。

Frame clock 為 absolute monotonic deadlines，落後時跳過 missed slots，不 enqueue catch-up frame。每 Tab estimator 取 max(main EWMA, raster EWMA)，cadence 是 33 ms 整數倍，過載連續達門檻才降頻，恢復需更保守連續證據且每次降一 slot。Window 每次最多一個 frame in flight；gate 於 Tab.run_animation_frame finally 在 observe_main 後解除，可 direct rearm，避免 Browser SDL wait 導致額外延遲。

## Navigation / network flow

1. navigate 或 schedule_load 清除 pending tasks；load 遞增 navigation_generation、reset estimator、discard 舊 JS、更新 URL/visited/history、scroll=0。
2. about:bookmarks 直接生成 HTML；其他 document request 非同步提交 network，Tab thread 立即返回（頂部過時註解說同步，實作已非同步）。
3. network completion enqueue `_finish_document_load(generation, url, result, error)`。舊 generation 無條件丟棄。TLS certificate failure 生成 Certificate Error page；其餘 failure 生成 Network Error page。
4. HTML 或 view-source HTML parse；读取 Referrer-Policy；CSP 僅識別開頭 default-src 與 explicit origins；建立 JSContext。
5. preorder 收集 `<script src>`、`<link rel=stylesheet href>`、inline `<style>`。**inline script 未被收集或執行。** CSP 禁止的 external resource 跳過。
6. external scripts/styles 允許平行 fetch，結果依 source index 保存；全部完成才排一個 finish task。非平行 baseline 同樣經 network coordinator。單筆 error 跳過，其他資源繼續。
7. finish 對 rules 加 default CSS、依 source order 處理 resource；external script 排 NORMAL JS.run tasks，CSS 即時 parse；最後 stable sort specificity、clear focus、pending fragment、request render。
8. frame task 執行 RAF → restyle → layout → paint → commit；Browser poll completed raster → arm next RAF → submit newest dirty scene；result epoch/size 正確才 present。

注意：script task 與 P0 frame 的先後並非簡單「所有 JS 後第一次 paint」；render priority 可提前。測試宜等待 stable observation 或驗證 task source order，避免假定首幀包含所有 mutation。

## JavaScript 可觀察 API

實際 runtime.js 提供 `window=this`、document.querySelectorAll/createElement/cookie、Node get/setAttribute/id/style/children/innerHTML/outerHTML/appendChild/insertBefore/removeChild、Event 與 listeners、同步 XMLHttpRequest、requestAnimationFrame。

**重要差異：browser.py 雖 export setTimeout/setInterval/clearInterval 與 async XHR hooks，目前 runtime.js 沒有 timer JS wrappers，也沒有 runXHROnload；XHR.open(method,url,true) 明確 throw。server.py 額外 eval SCHEDULING_RUNTIME_JS 提供 timeout/async XHR，但目前 browser.py 沒有此層。** C 若增補 timer/async wrappers 必須標 intentional difference，不能宣称與目前 browser.py 原樣一致。

DOM handle 單調配置，JSContext maps 保留連同 detached nodes。children 只回傳 Element；getAttribute missing 與 empty 都回空字串。setAttribute name casefold/value string conversion；id mutation 或 subtree insert/remove 同步 ID globals（第一個 duplicate id 勝；不覆蓋已有 global；舊自動 global 只有仍指向原 Node wrapper 才刪）。innerHTML 透過 `<html><body>` wrapper parse；舊 child parent 設 None，已有 JS handle 仍可訪問；serialize attributes HTML escape，void elements 無 closing tag。

Event path 由 target element 到祖先 root 預先取 handles；無 capturing。每節點 listeners 按插入順序執行，`this/currentTarget` 為該節點，target 固定。stopPropagation 等本節點所有 listeners 完成才停止 bubbling；preventDefault 僅阻止 default action，不停止 bubbling。JS error 被捕捉後 Python dispatch_event 回 False（default action 可執行）。RAF snapshot 整批後先清空，RAF callback 內新請求留下一幀，callback 無 timestamp argument。

XHR method 目前只傳作 label，真正 HTTP method 由 URL.request payload 判斷；source URL/policy 在離開 Main thread 前 capture。CSP 先檢查；cross-origin 加 Origin，回應 ACAO 僅允許 exact origin 或 *；sync XHR 透過 network.run_sync 阻塞 JS；async hook completion 若 context discarded 或 error 不觸發 onload。

Cookie JS getter 隱藏 HttpOnly；單 host 一 cookie record，getter 會 serialize cookie + params（不是完整瀏覽器標準 cookie 字串）。JS setter 不得覆寫既有 HttpOnly、不建立 HttpOnly、忽略 malformed、expired 時刪除。

## Input 與代表性 end-to-end flows

SDL event → BrowserWindow chrome 本地／頁面 P1 task。左鍵在 mouse-up 啟動；Shift+left 模擬半徑 20px touch。真 touch 僅單指、移動未超過 12px 時於 finger-up 啟動；忽略 SDL synthetic touch mouse，避免 double-click。

Tab.click 先 blur → hit test（頁面 scroll 與 nested Scroll transform）→ nearest element → bubble click → 若未 cancel，選 nearest scroll container focus。button ancestor 優先尋 form action 並 submit。checkbox toggle；文字輸入保留 value，按文字測量定位 caret；password 顯示星號。anchor #fragment 同 document navigation，其他 resolve 後 load 或 external handler。

Input keypress 先可取消 keydown，再按 Unicode character index 插入；backspace/left/right 不同樣 dispatch keydown。enter 尋祖先 form/action。Form submit event 可取消；收集所有 named input（包括 hidden），unchecked checkbox 排除、無值 checked checkbox = on，quote_plus 編碼；POST 用 body，其他 method 當 GET append query。disabled 等完整 web semantics 並未實作。

Scroll 聚焦內層 overflow container 時，即使到底也不自動傳到外層；普通 page scroll 不重建 display list，僅 frame commit scroll。fragment 找第一 id，取對應 layout/descendant 最小 y 並 clamp。

代表性驗收：本地頁面 external CSS/JS → mutation/render；同步同源與 CORS XHR；event cancel/bubble + DOM detach/reinsert；form GET/POST/hidden/checkbox/password；頁面與 nested overflow scroll；fragment/back/forward；address editing、bookmark/internal page、多 tab/multiwindow/resize；慢請求被新 navigation 取代後不得覆寫新 document；raster in flight resize/tab switch 不呈現 stale result。

## web_server.py 工作流程

純 Python socket HTTP/1.0 sequential server，port 8000，每連線一 request，GET/POST，Content-Length UTF-8，close。所有 response 有 same-origin Referrer-Policy、default-src http://localhost:8000 CSP，允許 localhost/127.0.0.1:8001 的 CORS origins。每 session token 64 hex，30秒 inactivity 延長 expiry，以 Set-Cookie token/Expires/SameSite=Lax 發回，非 HttpOnly。

GET / 首頁 → GET /login → POST / username/password → authenticated homepage hidden nonce → POST /add-topic 或 GET /topic → POST /add/topic hidden nonce/message。新增內容保存在 cwd message_board.json，測試必須於 temp directory 使用獨立 data fixture，避免改使用者真實資料。show_submit_result helper 未接進 router，因此不能把 /submit 成功當既有 server 行為。Browser 移植本身不需重寫服務端；驗收「無 Python runtime」可另以 C test HTTP fixture 代替 server。

## C lifetime 與 validation 建議

定義 DocumentGeneration refcount arena（含 DOM、rules、layout/hit refs）；Tab 擁有 current generation，JS context handles 擁有同 generation detached nodes；network callback 僅 weak tab ID + generation token；discard invalidates token 並 cancel timers；queued task 有 destructor，全部 clear/shutdown 路徑呼叫。Scene snapshot 不可無保護引用可變 DOM；將 URL action/hit bounds 搬進 immutable snapshot 或 retain generation 且明確排除 data race。

測試拆分 exact data oracle（DOM/serialization/event log/form payload/resource order/frame-clock decisions）、structural rendering（layout/display-list with tolerances）、live end-to-end（native app + C fixture + scripted input）。時計測試以注入 monotonic clock 固定值比較，不以牆鐘 sleep 作主要 correctness assertion。獨立 sanitizer 必須涵蓋 navigation 交疊、detached node 存活、關窗 pending network/JS/raster、重複 context create/destroy。

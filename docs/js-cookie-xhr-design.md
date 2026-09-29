# JS DOM 切片 5：`document.cookie` 與同步 XHR 的執行緒設計

**狀態：設計修正版，已經 `ownership-reviewer` 審查（2026-09-29），findings 全部併入第 10 節，第 10 節優先於前文。** 屬 [JS DOM 計畫](js-dom-plan.md)
切片 5；已確認的決定見該文件「決定」2（注入介面＋佇列）與刻意差異 D4、D9。已實作（2026-09-29），實作與本文的差異見第 11 節。

## 1. 要提供的行為（以凍結 oracle 為準）

- `document.cookie` 讀寫：`JSContext.document_cookie_get/set`（browser.py 4529–4583）。以
  `tab.url.host` 為鍵、每 host 一個 cookie；HttpOnly 讀不到、不能覆寫、JS 不能建立；過期 `Expires`
  刪除；無 host（`about:`、`data:`）時讀 `""`、寫無效。
- `XMLHttpRequest.send`：`JSContext.XMLHttpRequest_send`（4807–4875）。以送出當下的
  `tab.url` 解析 URL、取 `tab.referrer_policy`；CSP 不允許 → `Exception("Cross-origin XHR blocked by
  CSP")`；跨來源帶 `Origin`，回應 `access-control-allow-origin` 不是頁面 origin 或 `*` →
  `Exception("Cross-origin XHR blocked by CORS")`；`method` 只是標籤，`body` 非 `null` 時為 POST；
  回傳回應 body 字串。非同步（`open(..., true)`）在 runtime.js 就丟錯，本切片不處理（切片 6b）。
- 網路失敗、URL 解析失敗等其他錯誤的確切 JS 可觀察結果，實作前由 oracle probe 凍結（第 8 節）。

## 2. 現況（已對照程式碼）

- Cookie jar 在 `TaiNetwork` 內（`src/network.c` `Cookie *cookies`），`TaiNetwork` 是單一執行緒擁有。
  `tai_network_cookie_get/set` 已實作 JS 規則，但 `src/` 無呼叫者。
- `TaiBrowserApp`（`src/tabset.c`）的 loader 執行緒在 `loader_main` 建立、獨占使用並銷毀共用的
  `TaiNetwork`；SDL 執行緒只碰 mutex 保護的佇列。
- **載入期腳本在 loader 的 `tai_network_poll` 回呼內執行：** `page_load_request_done` →
  `page_load_finish_resources` → `page_apply_resources` → `tai_js_eval`。回呼在 `curl_multi_perform`
  回傳、請求已從清單摘除之後才呼叫，可再 submit／cancel（`network.h` 契約），但目前沒有程式在回呼內
  再次 poll。
- **事件期腳本在 SDL 執行緒：** 已提交的 `TaiPage` 由 `TaiSession` 擁有，點擊、按鍵在 SDL 執行緒派送。
- **同步載入路徑：** `tai_page_load_request`（`session.c` 單頁呈現、`main.c` headless CLI）在呼叫者
  執行緒用呼叫者自己的 `TaiNetwork`，之後的事件也在同一執行緒。
- `TaiPage` 不保存 CSP 允許清單（`page_prepare_document` 用完即丟）與 Referrer-Policy（native
  目前完全沒實作 Referrer-Policy，屬既有缺口）。
- JS 2 秒上限：`enter_js` 設 `deadline`，QuickJS interrupt handler 檢查；bridge 呼叫期間時間照算。

## 3. 設計總覽

```
             ┌──────────── TaiBrowserApp（SDL 執行緒建立／銷毀）────────────┐
             │ TaiCookieJar（自帶 mutex，app 擁有，loader 啟動前建立、join 後銷毀）│
             │ XHR 佇列 + xhr_condition（mutex = app->mutex）                   │
             └──────────────────────────────────────────────────────────────┘
 SDL 執行緒（事件期 JS）                        loader 執行緒（載入期 JS、所有網路 I/O）
  js.c ─host→ browser.c page_xhr ─TaiPageNet.request→ app_xhr_request
                                   │ 放入佇列、cond_wait(done)   ─→ 取出、tai_network_submit
                                   │                              ←─ xhr_done：填回應、done=true、signal
  js.c ─host→ browser.c page_cookie ─→ TaiCookieJar（mutex）   ←─ network.c 也經 jar（mutex）
  載入期：js.c ─host→ page_xhr ─TaiPageNet.request→ loader_xhr_request
                                   └→ tai_network_request_until(network, ..., service, abort)
```

三個層次各自只認識下一層：

1. **`js.c`**：`TaiJsHost` 新增回呼，不認識網路與執行緒。
2. **`browser.c`（page）**：實作 Python 的 cookie／XHR 規則（host、CSP、Origin、CORS、referrer），透過
   page 持有的 `TaiPageNet` 取得「送一個同步請求」與 cookie jar，不認識 loader 或佇列。
3. **`tabset.c`（app）／`session.c`／`main.c`**：注入 `TaiPageNet`，決定請求在哪個執行緒、怎麼等。

## 4. 介面

### 4.1 `TaiCookieJar`（`network.h`、`network.c`）

```c
typedef struct TaiCookieJar TaiCookieJar;
TaiCookieJar *tai_cookie_jar_create(void);
void tai_cookie_jar_destroy(TaiCookieJar *jar);
/* 任何執行緒皆可呼叫；內部 mutex 保護。回傳值為呼叫者擁有的副本。 */
char *tai_cookie_jar_js_get(TaiCookieJar *jar, const char *host);   /* 配置失敗 NULL */
bool  tai_cookie_jar_js_set(TaiCookieJar *jar, const char *host, const char *value);
/* network 借用 jar；jar 必須比 network 活得久。 */
TaiNetwork *tai_network_create_with_jar(TaiCookieJar *jar);
TaiCookieJar *tai_network_cookie_jar(TaiNetwork *network);        /* 借用 */
```

- `tai_network_create()` 建立並擁有私有 jar（同步路徑、既有測試行為不變）；`tai_network_destroy`
  只銷毀自己擁有的 jar。`tai_network_cookie_get/set` 改成 jar 的包裝。
- `network.c` 內 `cookie_valid`（`start()` 加 `Cookie` header、讀 `samesite`）與 `cookie_store`
  （`complete_transfer` 的 `Set-Cookie`）都在 jar mutex 內完成，**鎖內只做記憶體操作**，把要用的值複製
  出來後才解鎖；不在持鎖時呼叫 libcurl 或回呼。
- 過期檢查（`cookie_valid` 會刪除過期項目）是寫入，因此讀取也取同一把 mutex（不用 rwlock）。
- 理由：cookie 讀寫只是記憶體操作，走 loader 佇列會讓 `document.cookie` 最多延遲一個 loader 迴圈，
  甚至被載入期 XHR 卡住 30 秒。Python 的 `COOKIE_JAR` 也是多執行緒共用的全域 dict（靠 GIL）。

### 4.2 `TaiJsHost`（`js.h`、`js.c`）

```c
typedef enum { TAI_JS_HOST_OK, TAI_JS_HOST_ERROR, TAI_JS_HOST_NO_MEMORY } TaiJsHostStatus;
/* 新增成員（皆可為 NULL；NULL 時 cookie 讀 ""、寫無效，XHR 丟 Error）： */
TaiJsHostStatus (*cookie_get)(void *userdata, char **value);          /* *value 呼叫者擁有 */
TaiJsHostStatus (*cookie_set)(void *userdata, const char *value);
TaiJsHostStatus (*xhr_send)(void *userdata, const char *url, const char *body /* NULL=null */,
                            char **response, char **message);          /* 兩者呼叫者擁有 */
```

- `TAI_JS_HOST_ERROR` 時 `js.c` 丟 `Error(message)`（Python 的 bridge 例外；差異測試沿用切片 0 的
  bridge 錯誤正規化）；`NO_MEMORY` 丟 OOM。
- `method` 不傳給 host（Python 只當標籤）。`body` 由 `js.c` 以 `JS_ToCString` 轉字串，JS `null` → NULL。
- **Deadline：** `xhr_send` 呼叫前後量時間，若 `depth > 0` 把 `deadline` 往後延同樣秒數（XHR 阻塞
  不計入 2 秒上限），但每次最外層進入累計最多延長 30 秒（10.1）。cookie 回呼不延長。
- 回呼仍遵守既有契約：同步、在執行 JS 的執行緒、不得重入同一個 context。但 `xhr_send` 內的巢狀 poll
  可能執行**別的** page 的 JS（見 5.2），那是別的 `TaiJsContext`／`JSRuntime`，由 `enter_js` 的
  `JS_UpdateStackTop` 處理。

### 4.3 `TaiPageNet`（`browser.h`、`browser.c`）

```c
typedef struct {
    /* 同步送出一個請求，回傳擁有權轉移的 TaiResponse（transport 錯誤放在 response->error）；
     * 只有配置失敗或被取消時回 NULL，*cancelled 區分兩者。 */
    TaiResponse *(*request)(void *userdata, const TaiUrl *url, const TaiUrl *referrer,
                            const char *payload, const char *origin, const char *policy,
                            bool *cancelled);
    TaiCookieJar *cookies;   /* 借用 */
    void *userdata;          /* 借用 */
} TaiPageNet;

/* page 複製這個結構（不複製指標指向的物件）。只有 page 目前的擁有者執行緒可呼叫。 */
void tai_page_set_net(TaiPage *page, const TaiPageNet *net);
```

- `TaiPage` 新增：`TaiPageNet net`、`TaiMap *csp_origins`（`page_prepare_document` 保留 `parse_csp`
  結果，`tai_page_destroy` 釋放）、`char *referrer_policy`（Python `normalize_referrer_policy`：只接受
  `no-referrer`／`same-origin`，其餘 NULL）。本切片只讓 XHR 使用 referrer policy；文件導覽與
  subresource 仍未帶 policy，記為既有缺口，不在本切片修。
- `browser.c` 實作 `TaiJsHost` 的三個回呼（userdata = page）：
  - cookie：`host = tai_url_host(page->url)`，空 host 直接回 `""`／忽略；否則呼叫 jar。
  - XHR：`target = tai_url_resolve(page->url, url)`；CSP；`origin = 跨來源 ? tai_url_origin(page->url)
    : NULL`；`net.request(...)`；response 有 `error` → `HOST_ERROR`（訊息依 oracle）；跨來源檢查
    `access-control-allow-origin`；成功回 body 副本。
- 各載入路徑在建立 page 時就注入 net，因為載入期腳本在 `page_prepare_document` 之後、`page_finish_visual`
  之前執行：
  - `tai_page_load_request(network, ...)`（同步路徑）：`request` = 直接 `tai_network_request`，
    `cookies = tai_network_cookie_jar(network)`。之後事件也在同一執行緒，net 不變。
  - `tai_page_load_async(network, ..., const TaiPageNet *net)`：新增參數，由 tabset 傳入「loader 直連」
    net；`tai_page_load_async_markup`（`about:bookmarks`）同樣注入，但內部頁沒有 script。
  - tabset 在 `tai_tabset_pump` 提交 page 時（SDL 執行緒，已經由 completion 佇列的 mutex 取得
    happens-before）呼叫 `tai_page_set_net(page, &app->event_net)` 換成「事件期」net。

### 4.4 `tai_network_request_until`（`network.h`、`network.c`）

```c
/* 同 tai_network_request，但每次 poll 之間呼叫 service(userdata)；service 回 false 表示放棄：
 * 取消請求並回 NULL，*cancelled = true。service 可 submit／cancel 其他請求。 */
TaiResponse *tai_network_request_until(TaiNetwork *n, const TaiUrl *url, const TaiUrl *referrer,
    const char *payload, const char *origin, const char *policy,
    bool (*service)(void *userdata), void *userdata, bool *cancelled);
```

`tai_network_request` 改為 `service == NULL` 的特例；poll 間隔維持 100ms。

## 5. 執行緒與所有權

### 5.1 載入期（loader 執行緒，tabset 路徑）

- loader 直連 net 的 `userdata` = `LoadTask`（loader 在任務執行期間擁有它，task 不會在載入中被釋放：
  SDL 端只設 `cancelled`，釋放要等 completion 經佇列送回）。`LoadTask` 新增借用的 `TaiBrowserApp *app`
  （app 在 join loader 後才銷毀，比任務活得久；task 仍不持有 `TaiTabSet` 指標）。`request` 呼叫
  `tai_network_request_until(network, ..., loader_service, task)`。
- `loader_service(task)`：
  1. `task->cancelled`（atomic）或 `app->stopping`（取 `app->mutex` 讀）→ 回 false，XHR 放棄，JS 得到
     `Error("XMLHttpRequest cancelled")`（native 專屬訊息，Python 沒有對應情境），腳本結束後載入照
     常走到 `cancel_or_reap_loads` 被回收。取消延遲 ≤ 100ms，因此 `tai_browser_app_destroy` 的
     join 不會被 XHR 卡 30 秒。
  2. 把 XHR 佇列裡的**事件期**工作 submit 到 network（見 5.2），讓 SDL 的 XHR 不必等載入期 XHR 結束。
     不取新的 `LoadTask`（避免巢狀開始載入）。
- **巢狀 poll：** `tai_network_request_until` 在 poll 回呼內再 poll。已確認可行：libcurl 禁止的是在
  libcurl 自己的回呼內呼叫 multi API；`TaiNetwork` 的 done 回呼是在 `curl_multi_perform` 回傳、請求
  摘除後才呼叫。巢狀 poll 會完成其他請求並呼叫它們的回呼，包括：
  - 其他分頁的 resource → 可能讓其他 page 的載入期腳本巢狀執行（各自的 JS runtime），甚至再巢狀 XHR。
    深度上限 = 同時進行的載入數（≤ 視窗數 × 25 分頁），每層 C 堆疊有限；列為風險，測試兩層巢狀。
  - 其他載入完成 → `page_load_done` 只 `completion_prepare`，實際發布仍在 loader 主迴圈的
    `cancel_or_reap_loads`，不在巢狀中變更 `active_loads`。
  - 事件期 XHR 的請求 → `xhr_done`（5.2）。
  - **同一個 `TaiPageLoad` 的請求不會出現**：腳本只在該 load 的 resource 全部完成後執行。
- **D4 修正後的描述：** 巢狀 poll 仍推進所有已送出的傳輸，所以載入期 XHR 不會讓其他分頁「已開始」的
  下載停住；受影響的是新的導覽（佇列中的 `LoadTask` 要等 XHR 結束才開始）與取消回收。

### 5.2 事件期（SDL 執行緒，tabset 路徑）

```c
typedef struct XhrJob {               /* 放在 SDL 執行緒的堆疊上 */
    TaiUrl *url, *referrer;           /* job 擁有；SDL 建立，loader 只讀 */
    char *payload, *origin, *policy;
    TaiResponse *response;            /* loader 寫入、done 後由 SDL 取走 */
    bool submitted;                   /* 只有 loader 讀寫 */
    bool done;                        /* app->mutex 保護 */
    struct XhrJob *next;              /* app->mutex 保護 */
} XhrJob;
```

- SDL（`app_xhr_request`）：取 `app->mutex`；若 `stopping` 或 `!loader_ok` → 解鎖回 NULL（JS 得到
  `Error`）。否則放入 `xhr_queue`、`pthread_cond_signal(&app->condition)` 喚醒 loader，然後
  `while (!job.done) pthread_cond_wait(&app->xhr_condition, &app->mutex)`；解鎖後取走 `response`，
  釋放 job 的字串。**D9：** 這段期間 SDL 不處理事件、不重繪。
- loader：主迴圈與 `loader_service` 都會 `xhr_take_all`（持鎖把佇列整串摘下），逐一
  `tai_network_submit(..., xhr_done, job)`；submit 失敗直接以 NULL response 完成。
  `xhr_done(job, response)`：取鎖、`job->response = response`、`job->done = true`、
  `pthread_cond_broadcast(&app->xhr_condition)`、解鎖；**之後 loader 不再碰 job**。
- **堆疊上 job 的安全性：** SDL 在 `done` 之前絕不返回，所以 job 在 loader 使用期間一直有效。
  `done` 在 mutex 內設定、SDL 在 mutex 內觀察，response 的寫入對 SDL 可見。
- **loader 必須保證每個已排入的 job 都會 done：**
  - network 建立失敗：`loader_ok = false`，SDL 端不排入。
  - `network_failed`（poll 失敗）：取消所有 XHR 請求（`tai_network_cancel`），以 NULL response 完成。
  - 停止：只有 SDL 執行緒會設 `stopping`（`tai_browser_app_destroy`），而它此時卡在等待，不會發生；
    但為了防禦，loader 結束前把佇列與進行中的 job 全部以 NULL 完成。
  - 傳輸本身受 libcurl 30 秒總時限保證結束。
  - 因此 SDL 端用不帶時限的 `pthread_cond_wait`；不引入「SDL 放棄 job」的路徑，避免 job 擁有權在
    兩個執行緒間轉移。
- loader 等待條件改為 `!stopping && !queued_head && !xhr_queue && !active_loads && !xhr_active`；
  `xhr_active`（進行中的事件期 XHR 數）只有 loader 讀寫。
- 喚醒延遲：loader 可能正在 `curl_multi_poll(16ms)`，最多延遲 16ms。可接受；若量測有問題再加
  `curl_multi_wakeup`（需要新的執行緒安全 network API，本切片不做）。
- **環形等待檢查：** SDL 等 loader；loader 從不等 SDL（只取 `app->mutex` 做短暫的佇列操作，SDL 在
  `cond_wait` 期間會釋放 mutex）。載入期 XHR 期間 loader 仍透過 `loader_service` 服務事件期佇列。

### 5.3 同步路徑（`session.c`、`main.c`、presentation 單頁）

net 直接使用呼叫者的 `TaiNetwork`（`tai_network_request`，`service = NULL`，不可取消），jar 為
`tai_network_cookie_jar(network)`。載入與事件都在同一執行緒，無佇列。這些路徑本來就同步阻塞。

### 5.4 生命週期與銷毀順序

- `TaiCookieJar`：app 在啟動 loader **之前**建立，`tai_browser_app_destroy` 在 join loader **之後**
  銷毀。page 的 `net.cookies` 借用 jar；app 比所有 tab set 與 page 活得久（既有契約）。
- `event_net` 放在 `TaiBrowserApp` 內，`userdata = app`；page 只借用。
- 導覽取代舊 page、關分頁、關視窗都在 SDL 執行緒；事件期 XHR 進行中 SDL 被佔住，所以不會發生
  「XHR 進行中 page 被銷毀」。
- 載入中的 page 屬於 loader；取消由 5.1 的 `loader_service` 處理，腳本回傳後才走既有回收。
- 「XHR 期間關視窗／導覽」在本設計下的實際意義：事件期 → 關閉與導覽事件排在 SDL 佇列，XHR 結束後
  才處理（D9）；載入期 → SDL 發出取消或關閉後 ≤ 100ms 中止 XHR。

## 6. 錯誤與配置失敗

- 所有 host 回呼走單一清理出口；`TAI_JS_HOST_NO_MEMORY` 不丟 `Error` 而是 OOM。
- `js.c` 新 bridge 的每個 `JS_New*`／`JS_ToCString` 檢查失敗（計畫「風險」一節）。
- 配置故障掃描（切片 7）涵蓋：jar 讀寫、XHR 的 URL 解析、job 字串複製、response body 複製。

## 7. 刻意差異與紀錄

- **D4（修正描述）：** 載入期同步 XHR 期間，新的導覽等待它結束；已開始的傳輸照常推進。
- **D9：** 事件期同步 XHR 期間所有視窗無回應，最長約 30 秒（libcurl 總時限）。
- 載入期 XHR 被取消時的 `Error("XMLHttpRequest cancelled")`：Python 沒有取消（導覽後舊 Tab 的 XHR
  照跑完），只有在導覽或關閉時可觀察，腳本結果本來就被丟棄；記入差異表。
- Referrer-Policy：本切片只讓 XHR 帶 policy；導覽與 subresource 未帶是既有缺口，另列。

## 8. 測試計畫

- **Oracle 凍結：** `js_dom_oracle_probe` 已有 cookie 情境（2 個 pending 由本切片接上）。XHR 在
  `tests/network_fixture.py` 加路由：同源 GET/POST、跨來源 ACAO 相符／`*`／不符／缺少、CSP 擋、
  重新導向、404、連線失敗、`about:`／`data:` 頁面的 cookie，擴充 `js_page_fixture.py` 整頁情境，由
  `js_page_oracle_probe` 凍結（含錯誤種類與訊息）。
- **單元：** `test_network.c`：jar 多執行緒壓力（兩條執行緒交錯讀寫＋network 請求，TSan 不在工具鏈內，
  以 ASan＋大量迭代代替）、`tai_network_request_until` 取消。`test_js.c`：host 回呼錯誤碼、deadline
  延長、NULL host。
- **整合：** `js_dom_integration` 比對 oracle；tabset 測試：載入期 XHR 不死結、載入期 XHR 期間導覽
  （≤ 100ms 取消）、載入期 XHR 期間關閉 app（join 不等 30 秒）、事件期 XHR 在載入期 XHR 進行中仍完成
  （5.1 的 service）、兩層巢狀（兩個分頁的載入期腳本都做 XHR）、慢伺服器 > 2 秒仍成功（deadline）。
- **ASan＋LSan** 全套；切片 7 再加 Xvfb。

## 9. 請審查者特別檢查

1. 5.2 堆疊上 `XhrJob` 的擁有權與可見性論證；是否有 loader 在 `done` 後仍碰 job 的路徑。
2. 5.1 巢狀 poll 的重入：`page_load_request_done`、`cancel_or_reap_loads`、`TaiPageLoad` 狀態在巢狀
   回呼中是否有 use-after-free 或重複發布。
3. `TaiCookieJar` 鎖的範圍：`start()` 讀 cookie 與 `complete_transfer` 寫 cookie 是否有鎖外使用。
4. `tai_page_set_net` 在提交時切換的 happens-before 是否充分（completion 佇列 mutex）。
5. 是否遺漏「SDL 等待而 loader 永不完成」的路徑。
6. 載入期 XHR 被取消時，`TaiPageLoad` 繼續跑完剩餘腳本與 `page_finish_visual` 的成本與正確性。

## 10. 審查結果與修正（2026-09-29，`ownership-reviewer`）

審查只讀程式碼、未執行。整體架構（jar 獨立加鎖、事件期堆疊 `XhrJob`、loader 永不等 SDL、提交時經
completion mutex 交接 page）判定成立；以下修正取代前文對應段落。

### 10.1 [HIGH] Deadline 延長讓 loader 可被卡死

`for(;;){try{x.send()}catch(e){}}` 在載入期永遠不觸發 2 秒上限，而 loader 是所有分頁唯一的載入執行緒，
`tai_browser_app_destroy` 也要 join 它；取消只讓 XHR 丟錯，try/catch 會吃掉。修正：

- **延長上限：** 每次最外層 `enter_js` 起算，XHR 延長累計最多 30 秒；超過後不再延長，原本的 interrupt
  照常中止腳本。
- **取消即中斷：** `TaiJsHost` 新增 `bool (*cancelled)(void *userdata)`，interrupt handler 在它回 true 時
  立即中止（不可攔截，同 deadline）。實作只做 atomic 讀取：`LoadTask.cancelled` 已是 atomic；
  `app->stopping` 另外鏡像成 `atomic_bool stopping_flag`，不在 interrupt handler 取 mutex。
- **取消後的 XHR 立即失敗：** `loader_xhr_request` 送出前先檢查 cancelled，不再每次付 100ms poll。
- **取消後跳過剩餘工作：** `TaiPageNet` 新增 `cancelled` 回呼；`page_apply_resources` 在每個 script 之間、
  `page_load_finish_resources` 在 `page_finish_visual` 前檢查，取消時直接以「navigation cancelled」完成
  （也回答第 9 節問題 6）。

### 10.2 [HIGH] 跨 page 巢狀沒有堆疊深度上限

巢狀 poll 會派送所有 ready 的請求（`network.c` 858–876），其他載入的 `page_load_request_done` 會跑
`tai_js_eval`，可能再巢狀；每個 JS runtime 最多 512KB 堆疊，loader 預設約 8MB，深度只受同時載入數限制。
修正（採審查者建議的根本解）：

- `tai_network_submit` 的請求新增「巢狀可派送」旗標（內部欄位，公開 API 以
  `tai_network_submit_nested` 或旗標參數提供）。`tai_network_request_until` 用受限模式 poll：
  `complete_transfer` 照常處理所有完成的傳輸（只標 `ready`），但**只派送**自己的請求與標記為巢狀可派送
  的事件期 XHR；其他 ready 請求留在清單，由外層 poll 派送。
- 受限模式判斷「是否跳過 curl 等待」時只計入可派送的 ready 請求，否則被延後的 ready 請求會造成忙迴圈。
- 結果：載入期 XHR 期間不會執行其他 page 的腳本，第 9 節問題 2、3 從根本消失；巢狀深度固定為 1。
- 代價：巢狀期間其他分頁已完成的下載要等外層才處理（傳輸本身仍推進）。D4 描述隨之修正。

### 10.3 [MEDIUM] `request_until` 需要 network

`LoadTask` 沒有 network 指標。改為 loader 在 `loader_main` 堆疊上的 `LoaderContext { TaiBrowserApp *app;
TaiNetwork *network; }`，載入期 net 的 userdata 是 `{ LoaderContext *, LoadTask * }`（放在 `LoadTask`
內的小結構，生命週期同 task）。

### 10.4 [MEDIUM] loader 迴圈與事件期工作清單

- 等待條件與 **poll 條件**（`tabset.c` 286 目前只在 `active_loads` 時 poll）都加入 `xhr_queue`／
  `xhr_active`；否則只有事件期 XHR 時 loader 不 poll、SDL 永遠等不到。
- loader 端進行中清單：`XhrJob` 新增只有 loader 讀寫的 `TaiRequest *request` 與 `loader_next`（與受
  mutex 保護的 `next` 分開）。`xhr_done` 先從進行中清單摘除、`xhr_active--`，最後才在 mutex 內設
  `done`。
- `network_failed`：對每個進行中 job `tai_network_cancel(request)`（會抑制回呼），再以 NULL response
  完成。

### 10.5 [MEDIUM] SDL 等待的上限不是 30 秒

每次重新導向都重新 `start()`，`CURLOPT_TIMEOUT_MS` 重算（`network.c` 560、713），最多 10 跳約 300 秒；
loader 也可能正在跑別的載入期腳本。修正：

- 同步 XHR（載入期與事件期）都有**總時限 30 秒**，從送出起算、跨重新導向：載入期由
  `tai_network_request_until` 檢查；事件期 job 記錄送出時間，loader 每輪檢查逾時就 cancel 並以
  `Error("XMLHttpRequest timed out")` 完成。
- loader 在 script 之間（10.1 的 cancelled 檢查點）也服務事件期佇列：submit 新 job 並做一次 0ms 的受限
  poll（10.2），讓事件期 XHR 的延遲上限是「一個 script（≤ 2 秒＋延長上限）」而非整頁載入。
- D9 的文字改為：最長約 30 秒傳輸＋ loader 正在執行的單一載入期 script 的時間。

### 10.6 [MEDIUM] 提交時切換 net 的時機

`tai_tabset_pump` 在提交後 `task_destroy(task)`（`tabset.c` 872–915），載入期 net 的 userdata 指向 task，
若 page 保留它，第一次事件期 XHR 就是 use-after-free。修正：取得 `candidate` 後、resize 與 commit
（navigation 與 history 兩條路徑）**之前**立即 `tai_page_set_net(candidate, &app->event_net)`；提交失敗
被銷毀的 candidate 不受影響。同步路徑（`main.c`、`session.c`、presentation 單頁）確認 page 在 network
之前銷毀。

### 10.7 補充的測試

try/catch XHR 迴圈遇取消與停止；多分頁同時載入期 XHR（驗證巢狀深度 1）；事件期 XHR 進行中
`network_failed`；loader 閒置（無 `active_loads`）時的事件期 XHR；提交後 `LoadTask` 已釋放再做事件期
XHR（ASan）；慢伺服器的重新導向鏈（總時限）；關視窗後的 XHR。

## 11. 實作與設計的差異（2026-09-29）

- **`XhrJob` 借用而非複製輸入：** url／referrer／payload／origin／policy 借用自等待中的 SDL 執行緒
  （它在 `done` 前不會返回，且這些物件在等待期間不變），`tai_network_submit` 本身會複製；省去一次
  配置與其失敗路徑。
- **`TaiPageNet`：** `request` 失敗時回 NULL 並給擁有權轉移的 `*message`（NULL 表示配置失敗），取代
  `bool *cancelled`；另有 `checkpoint`（script 之間，載入期服務事件期佇列並做一次 0ms 受限 poll，回
  false 表示已取消）與 `cancelled`（無鎖，供 interrupt handler）。
- **`tai_network_request_until`：** 以 `TaiWaitStatus`（DONE／CANCELLED／TIMED_OUT／FAILED）回報並帶
  總時限；`tai_network_request` 是它的受限特例（總時限 0）。事件期 XHR 由 `tai_network_allow_nested`
  標記為巢狀可派送；`tai_network_poll_nested` 供 checkpoint 使用。
- **中斷檢查：** QuickJS 每數千個操作才呼叫 interrupt handler，只做少量 JS 的慢速 XHR 迴圈要數分鐘
  才會輪到。`op_xhr_send` 在 host 回傳後立即檢查 deadline 與 `cancelled`，成立時丟不可攔截的
  `InternalError('interrupted')`（由 `test_js.c` 的 30 秒上限與取消情境發現）。
- **Cookie jar 種子：** `tai_cookie_jar_http_set` 以 Set-Cookie 規則寫入，供 `js_probe` 重現 oracle
  預先放入的 HttpOnly cookie。
- **測試：** 載入期／事件期 XHR 的整頁 oracle 比對同時跑 headless 與 `--tabset` 兩種路徑
  （`js_page_probe`）；tabset 的取消、關閉、事件期 XHR 在載入期 XHR 期間完成、兩分頁同時阻塞由
  `tests/test_tabset_xhr.c` 驗證。未做：`network_failed` 情境（無法在測試中穩定觸發 poll 失敗）、
  兩層巢狀的 C 堆疊量測（受限 poll 已讓深度固定為 1）、QuickJS 配置故障掃描涵蓋新 bridge（切片 7）。

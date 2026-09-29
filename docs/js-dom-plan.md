# JS DOM 補齊：整體計畫與交接

**狀態：實作中（2026-09-29）。** 六項決定皆已確認；切片 0–5 完成，切片 6 起尚未開始。
計畫已經四路獨立驗證（oracle 實跑、native 程式碼、所有權設計、文件一致性），結果已併入本文。
本工作處理 [ACCEPTANCE.md](../ACCEPTANCE.md) 的「JS-visible DOM mutation、query、event
propagation/default prevention、XHR 與實際可用 scheduling APIs」，並滿足同檔「ASan 重複
load/mutate/render/close」與「獨立 verifier」兩項；也處理 [PORTING_PLAN.md](../PORTING_PLAN.md)
子系統表中「JavaScript / events」「DOM / HTML」「URL / HTTP」三列的 JS 相關缺口，以及
[視窗標題計畫](window-title-plan.md) 問題 12（JS 改寫標題）在 native 端未比對的部分。

## 一句話摘要

把 native `src/js.c` 的 JS 介面補到與凍結的 `tests/reference/runtime.js` 相同：DOM 查詢與修改、
`innerHTML`/`outerHTML`、ID 全域變數同步、`document.cookie`、同步 `XMLHttpRequest`、
`requestAnimationFrame` 與 `log`；並先修正 listener 丟錯會讓視窗結束的既有缺陷；native 執行 inline script 保留並記為刻意差異 D6。
每一項先由 Python oracle 凍結答案，再由 native 逐點比對。

## 目標與完成條件

1. `python3 tests/js_dom_oracle_probe.py --check` 連跑三次輸出相同（CTest `js_dom_oracle_probe`）。
2. `js_dom_differential`（純 JS／DOM 情境）與 `js_dom_integration`（完整頁面、網路、事件、RAF）
   與 oracle 逐點相符；只有「刻意差異」表列出的欄位可套用對應規則。
3. 完整 CTest、`build-asan/` 全套 ASan/UBSan＋LSan 通過，其中包含反覆 load → mutate → render →
   close 的情境；回報時註明有執行 LSan。
4. QuickJS／DOM 配置故障掃描涵蓋所有新 bridge operation。
5. Xvfb 真實視窗：點擊觸發 DOM 修改、標題改變、listener 丟錯不讓視窗結束，截圖與事件紀錄為證。
6. `ownership-reviewer` 獨立審查的 findings 全部解決或記錄。
7. 紀錄同步（見切片 7）。

## Python 依據（`tests/reference/browser.py`）

- JS 端：`tests/reference/runtime.js`（344 行，完整介面）。
- Python 端：`JSContext`（4437–5160）。呼叫點：
  - `Tab._finish_document_load`（6005 起；6058 `self.js = JSContext(self)`）。
  - `Tab._process_page_resources`（5879 起）：只收有 `src` 的 script，依 source 順序執行。
  - `Tab.run_animation_frame`（6219 起；6230 `evaljs(RAF_JS)`，`RAF_JS = "runRAFHandlers()"` 定義於
    224 行；callback 例外被捕捉並印出）。
  - `Tab.submit_form`（6618）、`Tab.click`（6681）、`Tab.keypress`（6759）派送 submit／click／keydown。
- 歷史摘要：[docs/python-runtime.md](python-runtime.md)「JavaScript 可觀察 API」（權威仍是凍結 oracle，
  見 [python-reference.md](architecture/python-reference.md)）。

### Oracle 實跑確認的行為（2026-09-29，dukpy＋凍結 runtime.js）

- **Timer：** `typeof setTimeout`、`setInterval`、`clearInterval` 都是 `undefined`；呼叫丟
  `ReferenceError`。browser.py 雖有 export，頁面實際不可用。原因是原始專案 commit `7d536e0`
  （2026-09-04）刪除 `SCHEDULING_RUNTIME_JS` 時只把 RAF 搬進 runtime.js，timer 與非同步 XHR 的 JS
  包裝被意外移除；較舊的完整副本 `/home/paulboul/tai_gar/server.py`（3233 行起）仍保留該段。
  `setInterval` 則在任何版本都沒有 JS 包裝。
- **XHR：** `open(m, u, true)` 丟 `Asynchronous XHR is not supported`。同步路徑的網路部分（CSP／CORS）
  在切片 5 以 fixture server 另行凍結。
- **Inline script：** 不執行（`_collect_page_resources` 要求 `src`）。
- **Listener 丟錯：** `dispatch_event` 捕捉並印 `Event <type> crashed ...`（stdout），回「未阻止」，
  default action 照跑，連先前呼叫過的 `preventDefault` 也失效；同節點之後的 listener 與冒泡都不再執行。
- **ID 全域：** 重複 id 第一個勝；不覆寫 `document`、`Node` 或腳本自己設的全域；舊自動全域仍指向原
  wrapper 才刪；刪後再加回是新 wrapper（`!==`）；`setAttribute('id')` 會重算；detached 節點的 id 不進
  全域，接上後才進。`getAttribute` 名稱不 casefold，`setAttribute` 才 casefold。
- **序列化：** 屬性依解析順序（同名後者覆蓋、位置留在第一次出現處）；文字只 escape `& < >`，屬性
  escape `" ' & < >`（`'` 為 `&#x27;`）；`<script>` 內容也被 escape；void 元素無結尾標籤；空屬性輸出 `disabled=""`；
  註解被丟棄；**屬性值的 entity 不解碼**（`class="a&quot;b"` → `class="a&amp;quot;b"`）；
  `<br/>` 解析成標籤 `br/`，序列化成 `<br/>…</br/>`。
- **innerHTML 設定：** 以 `<html><body>` 包裝，取**最後一個** `body`（`find_body` 不中斷）；
  `innerHTML = 5` 得 `"5"`，`= null` 丟 `TypeError`。
- **Mutation：** 移動既有節點會先從舊 parent 移除；`insertBefore(x, null)` 等於 append；
  `insertBefore(x, x)` 不動；錯誤訊息 `Reference child is not a child of parent`、
  `Cannot insert a node into itself or its descendant`、`Node is not a child of this parent`；
  `insertBefore(y)`（無第二參數）是 JS `TypeError`。
- **其他：** `createElement` 名稱用 Python `casefold()`（`'İ'` → `i̇`）；`children` 略過 Text；
  `Node` 沒有 `parentNode`；handle 依首次取用順序編號；`log` 印到 stdout。
- **RAF：** 整批取出後清空，callback 內新註冊的留到下一 frame；每次呼叫都通知需要 frame；
  callback 丟錯時同批剩下的被丟棄。Headless 是否跑 RAF 延到切片 6 以完整 `Tab` 凍結。

## 現況與缺口（native，已對照程式碼）

| 介面 | native 現況 | 缺口 |
|---|---|---|
| `querySelectorAll`、`getAttribute`/`setAttribute` | 有（`src/js.c`） | `setAttribute('id')` 不重算 ID 全域；未知 handle／Text 的錯誤需比對 |
| `id`/`style` 存取器、`children`、`window`、`log` | 無 | 全補 |
| `createElement`、`appendChild`、`insertBefore`、`removeChild` | 無（`tai_node_append` 有循環檢查但沒接 JS） | 全補 |
| `innerHTML`/`outerHTML` | 切片 3 前：`tai_node_set_inner_html` 無呼叫者且取**第一個** `body`；無 HTML 序列化器 | 切片 3 已完成 |
| ID 全域 | 建立時設一次，從不刪除（`src/js.c:33`、`189`） | 改成 Python `sync_id_globals` 語意 |
| Inline script | **會執行**（`src/browser.c:115-124`、`296-299`），與外部 script 依 source 順序執行，不受 CSP 限制 | 保留執行，但頁面有 CSP `default-src` 時不執行（比照真實瀏覽器）；記為刻意差異 D6（決定 5） |
| Listener 丟錯 | `tai_js_dispatch_event` 回錯誤，`browser.c:1205/1348/1450` 直接 `return false`，一路讓**視窗事件迴圈結束** | **既有缺陷**，改成真實瀏覽器語意（決定 6，D7） |
| `document.cookie` | `tai_network_cookie_get/set` 存在，但 src/ 無呼叫者；network 屬於 loader 執行緒 | 接 JS，先定執行緒設計 |
| 同步 XHR | 無 | 全補 |
| RAF | 無 | 全補 |
| Mutation 後重建 | 切片 4 前：`invalidated` 只設 `page->dirty`，重建在事件處理尾端；caret 與 fragment 捲動用舊 layout | 切片 4 已完成 |
| 焦點節點被移除 | 切片 4 前：`page->focused` 與 `node->focused` 會留在 detached 節點 | 切片 4 已完成（D8） |
| QuickJS 跨執行緒 | runtime 在 loader 建立、在 SDL 執行緒執行，從未呼叫 `JS_UpdateStackTop` | 每次進入 JS 前更新 |

## 整體流程

```
 切片 0  Oracle：js_dom_oracle_probe.py 凍結 JS/DOM 情境；integration 情境延到切片 4–6 補凍結
                          ▼
 切片 1  既有缺陷：listener 丟錯不再中止視窗、JS_UpdateStackTop、deadline 重入；記錄 D6
                          ▼
 切片 2  runtime 對齊凍結 runtime.js ＋ DOM mutation ＋ ID 全域同步
                          ▼
 切片 3  HTML 序列化與 innerHTML/outerHTML
                          ▼
 切片 4  頁面整合：mutation 後重建、焦點清除、JS 改寫標題
                          ▼
 切片 5  document.cookie 與同步 XHR（先定執行緒設計並審查）
                          ▼
 切片 6  requestAnimationFrame 接到視窗 frame
                          ▼
 切片 6b 補回 setTimeout／setInterval／非同步 XHR（決定 1，刻意差異 D5）
                          ▼
 切片 7  收尾：CTest、ASan/UBSan＋LSan、配置故障、Xvfb、獨立審查、紀錄
```

## 切片細節

### 切片 0：Oracle 凍結

**狀態：完成（2026-09-29）。** `tests/js_dom_cases.py`（情境，native 差異測試共用）、
`tests/js_dom_oracle_probe.py`、`tests/fixtures/js_dom_oracle.json`，CTest `js_dom_oracle_probe`
連跑三次通過，竄改 fixture 時會失敗。Probe 用真的 `JSContext`＋最小 Tab 替身（文件、URL、
render／RAF 計數）；`log` 與 crash 經模組層 `print` 結構化記錄。每個 JS step 以
`js_dom_cases.step_source()` 包裝（間接 `eval`、值編碼成 JSON），native 須逐字使用同一段包裝。
錯誤正規化：dukpy 把 bridge 的 Python 例外包成 `EvalError("Error while calling Python Function
(fn): Exception('msg')")`，fixture 記為 `{"type": "bridge", "function", "message"}`（`KeyError` 等
只記 `python` 型別）；native 對應為丟出同訊息的 `Error`，差異測試依此比對。JS 錯誤只記 `name`，
純 `Error` 才記訊息。

凍結時新確認的行為（上方清單以外）：
- 數字 id 也會成為全域（`id = 42` 後 `window['42']` 是 wrapper）。
- `setAttribute(n, null/undefined)`、`id = null`、`style = null` 在 JS 端 `toString` 丟 `TypeError`，
  不觸發 invalidation；`getAttribute(5)` 不轉字串，回 `""`。
- `children`、`querySelectorAll` 每次產生新 wrapper（`list.children[0] !== list.children[0]`）。
- `outerHTML` 只有 getter，賦值靜默忽略；`innerHTML = { toString }` 走 `toString`。
- `innerHTML = '<head><title>t</title></head>text'` 在 Python parser 下原樣保留在元素內。
- `document.cookie` 讀取回傳含參數的序列化（`theme=dark; samesite=lax; path=/`），每個 host 一個
  cookie；過期 `Expires` 會刪除；既有 HttpOnly cookie 讓 JS 讀寫都無效。
- `createElement` 已建但未接上的節點可掛 listener，接上後派送照常冒泡。

- 新增 `tests/js_dom_oracle_probe.py`，以 SDL dummy 載入凍結 browser.py，用最小 Tab 建立真的
  `JSContext`，對每個情境記錄 DOM JSON、JS 回傳值、例外（只比對「有丟錯」與訊息中 Python 自訂的部分，
  dukpy 引擎訊息不比）、`log`／crash 輸出、invalidation 次數。輸出 `tests/fixtures/js_dom_oracle.json`，
  註冊 CTest `js_dom_oracle_probe`（`--check`）。
- 情境以上方「Oracle 實跑確認的行為」逐條成為 checkpoint，另加：未知 handle、非字串引數、
  listener 內移除節點後冒泡是否仍到已脫離的祖先（Python 先算好祖先路徑）、事件中 `setAttribute('id')`。
- 完整頁面情境（RAF 首幀、headless）在切片 4／6 以
  fixture server 驅動完整 `Tab` 凍結。

### 切片 1：既有缺陷

**狀態：完成（2026-09-29）。** 前言覆寫 `dispatchEvent` 逐一 `try/catch`，錯誤經 C 函式
`__tai_listener_error` 印到 stderr；`tai_js_dispatch_event` 只在派送無法開始（輸入或配置失敗）時回錯，
逃出的例外（deadline 中斷、OOM）由 js.c 印出並回「未阻止」；`browser.c` 以 `dispatch_page_event`
統一三個呼叫點，錯誤一律照做 default action、不再提前 return（dirty 頁面走原本的重建路徑）。
`enter_js`／`leave_js` 以巢狀深度管理 deadline，最外層進入時呼叫 `JS_UpdateStackTop`；建立時的 ID
同步也在 deadline 內。證據：`javascript`（丟錯 listener、ReferenceError、2 秒中斷後可繼續使用、
另一執行緒淺層成功＋深遞迴丟 stack overflow；拿掉 `JS_UpdateStackTop` 時此測試失敗）、
`browser_headless`（丟錯 click listener 仍切換 checkbox 並重建）、`inline_script_integration`
（source 順序、CSP 允許清單、空清單、非 `default-src`）。完整 CTest 47/47；`build-asan/`
的 `javascript`、`browser_headless`、`inline_script_integration`、`js_dom_oracle_probe` 在 LSan 開啟下通過。
D6、D7、D1（crash 部分）已寫入 `PORTING_PLAN.md`。巢狀 deadline 目前沒有重入路徑可測，切片 2 的
bridge 內 ID 同步出現後補測。

- **Listener 丟錯（決定 6，D7）：** 比照真實瀏覽器逐一隔離 listener：native 前言覆寫
  `Node.prototype.dispatchEvent`，對每個 listener 個別 `try/catch`，錯誤交給 C 印到 stderr（格式
  `Event <type> crashed ...`）後繼續下一個 listener 與冒泡；已呼叫的 `preventDefault`／`stopPropagation`
  保持有效。凍結 runtime.js 不改。`tai_js_dispatch_event` 只在 bridge 本身失敗（配置失敗等）時回錯；
  `browser.c` 三個呼叫點依 `default_prevented` 執行 default action，錯誤一律不讓視窗結束；錯誤路徑也要
  重建 dirty 頁面（listener 可能已在丟錯前改了 DOM）。
- **Inline script：** 保留執行（決定 5）；`collect_resources` 在頁面有有效 CSP `default-src`（`parse_csp` 回傳非 NULL）時
  不收 inline script。在 `PORTING_PLAN.md`「已知差異與範圍」記錄 D6。
- **`JS_UpdateStackTop`：** 每個進入 JS 的入口（eval、dispatch、RAF、XHR 回呼）先呼叫。
- **Deadline：** 改成巢狀計數，內層不歸零外層；`tai_js_create` 內的 ID 同步也受 deadline 保護。
- 測試：`tests/test_js.c`（CTest `javascript`）加丟錯 listener、跨執行緒深遞迴；headless 整合確認
  inline script 與外部 script 依 source 順序執行；有 CSP `default-src` 時 inline script 不執行、外部 script
  照允許清單執行。

### 切片 2：runtime 對齊、DOM mutation、ID 全域

**狀態：完成（2026-09-29）。**
- **Runtime：** `cmake/embed_text.cmake` 在建置時把凍結 `runtime.js` 與 `src/js_prelude.js`（D7 的
  `dispatchEvent` 覆寫）原文嵌入 `tai_js_sources.h`；凍結檔一字未改。
- **介面：** `tai_js_create(root, const TaiJsHost *, error)`；`TaiJsHost` 有 `invalidated`、
  `node_removed`、`report`（NULL 時寫 stderr，D1），切片 5 再加 cookie／XHR。新增
  `tai_js_eval_value` 供 probe 取回完成值。
- **Handle：** 與 Python `get_handle` 相同，依首次使用編號、永不重用（不需要 D3）；只在 js.c 的
  `handle_of`／`node_arg` 轉換。未知 handle 丟 `Error('Unknown node handle')`。
- **DOM：** `tai_document_create_element`、`tai_node_insert_before`、`tai_node_remove_child` 回傳
  `TaiDomStatus`，檢查順序同 Python，失敗時樹不變；D2 上限 1,000,000（只限腳本建立）。
- **ID 全域：** 每次 mutation 後走訪整棵樹、呼叫 runtime.js 的 `sync_id_globals`。同步失敗只回報；
  但無法攔截的錯誤（deadline 中斷）會往外傳，否則外層腳本永遠不會被中斷——這個缺陷由 ASan 下的
  時間測試發現並修正。
- **測試：** `tests/js_probe.c`＋`tests/js_dom_differential.py`（CTest `js_dom_differential`）：
  12 個情境與 oracle 相符，14 個標為 pending（需要切片 3／5／6 的 bridge）；`listener_throws` 套用 D7
  的預期答案。`test_js.c` 新增 mutation 回呼、巢狀 deadline（中斷不得被 resync 吞掉）、1 萬次 append
  （Debug 0.51 s；ASan 建置改跑 2,000 次）、D2 上限；`test_dom.c` 新增狀態碼與上限。
- **Oracle probe 修正：** `oracle.dom_value` 共用 Element 的屬性 dict，先前的快照會被後續修改改寫，
  已改為深拷貝，並重新產生 fixture。
- **QuickJS 修補 0002–0005：** 嵌入完整 runtime.js 後，`tabset_loader_oom` 在 parser 的 OOM 路徑
  崩潰。`test_quickjs_oom` 新增 runtime.js 編譯掃描（失敗後全部失敗／只失敗一次兩種模式），另外找到
  兩個缺陷。每個修補都已驗證：拿掉就會讓掃描失敗；五個修補依序套用可重現 `deps/`。見
  `patches/README.md`。
- **證據：** Debug CTest 48/48；`build-asan/` 全套 48/48（LSan 開啟，只排除文件記載的 fontconfig／
  cairo 洩漏）。
- **後續風險：** `collect_ids`、`collect_matches`、`tai_dom_json` 以遞迴走訪 DOM，而腳本現在能用
  `appendChild` 建出任意深的樹。200,000 層巢狀 `<div>` 的既有 headless 載入跑超過 2 分鐘仍未結束
  （已中止），所以深樹本來就是 native 的既有問題，不是切片 2 造成的。切片 7 決定要加深度上限還是
  改成迭代走訪。

- `src/js.c` 的內嵌 runtime 改為建置時嵌入凍結 `runtime.js` 原文（CMake 產生標頭；原檔只讀），加上
  一小段 native 專用前言（例如 `log` 導向）。若最後不能原文嵌入，逐項等價並在 `ARCHITECTURE.md`
  `js.c` 列記錄。
- `dom.h` 新增 `tai_document_create_element`、`tai_node_insert_before`、`tai_node_remove_child`，
  回傳錯誤碼；循環與「不是子節點」檢查在 DOM 層，JS 層轉成 Python 同訊息的 `Error`。
- 所有節點仍由 `TaiDocument` 擁有，id 不重用，detached 節點活到文件銷毀。新增文件節點數上限
  （超過丟 JS 錯誤），避免迴圈建立節點讓 C 堆無界成長；記為刻意差異。
- ID 全域同步在 JS 端執行（runtime.js 的 `sync_id_globals`），C 端一次傳入 entries；同步失敗只記錄、
  不讓已完成的 mutation 變成錯誤。wrapper 由 JS 端保存，C 端不持有 `JSValue`。
- ID 全域同步先照 Python 每次走訪整棵樹（O(n²)），另加時間測試：以迴圈 `appendChild` 加入 1 萬個
  無 id 元素，Debug 建置須在 1 秒內完成（2 秒 JS 上限的一半）。超標才改用 C 端「id → 元素」索引表
  （保留文件順序），省去整棵樹走訪；每次同步仍重建所有 id 全域的 wrapper、重新檢查 `name in window`，
  因此與 Python 行為完全相同，不算刻意差異。
- 每次 mutation 呼叫 `invalidated`；新增 `node_removed` 回呼供頁面清焦點。
- 測試：`tests/js_dom_differential.py`（CTest `js_dom_differential`，傳入新的 `js_probe` 執行檔，
  它讀 HTML＋腳本、輸出與 oracle 相同格式的 JSON）。
- **綁定邊界（為日後升級保留，見「綁定升級路線」）：**
  1. `dom.h` 不知道 handle 或 JS：新增的 DOM API 只收發 `TaiNode *`，不含 `JSValue`、handle 整數或
     JS 例外語意。
  2. handle 與節點的轉換只在 `js.c` 單一位置（現有 `node_from_value` 與對應的「節點 → JS 值」函式）；
     各 bridge operation 不直接讀寫整數 handle。
  3. 凍結 `runtime.js` 原文與 native 自己加的 JS 分成兩個來源檔分別嵌入（例如
     `src/js_prelude.js`），凍結檔一字不改；Python 原版語意與 native 擴充（`log` 導向、D5 的 timer／
     非同步 XHR）由此區分。
  審查時（切片 7 的 `ownership-reviewer`）逐條確認這三條沒有被破壞。

### 切片 3：序列化與 innerHTML

**狀態：完成（2026-09-29）。**
- **序列化：** `dom.c` 新增 `tai_node_serialize(node, outer)`（Python `serialize_node`；outer＝
  `outerHTML`，否則只序列化子節點＝`innerHTML`）。以明確堆疊迭代走訪，深樹不會耗盡 C 堆疊。屬性依
  `TaiMap` 儲存順序（同名後者覆蓋、位置留在第一次出現處）；文字（含 `<script>` 內容）只 escape
  `& < >`，屬性值另 escape `"` 與 `'`（實跑 `html.escape(quote=True)` 確認為 `&#x27;`）；void 元素
  不輸出子節點與結尾標籤，但 void 元素自己的 `innerHTML` 仍列出腳本加入的子節點（同 Python）。
- **innerHTML 設定：** `tai_node_set_inner_html(node, html, &removed, &removed_count)` 改回傳
  `TaiDomStatus`：取**最後一個** `body`（修正原本取第一個與錯誤註解）；fragment 的所有節點（含包裝用
  `html`／`head`／`body`）加入文件前先檢查 D2 上限（超過回 `TAI_DOM_NODE_LIMIT`）；Python
  `HTMLParser` 會丟 `IndexError` 的標記（開放元素堆疊為空）回新的 `TAI_DOM_PARSE_ERROR`，與配置
  失敗（`TAI_DOM_NO_MEMORY`）分開；三種失敗都整批回收 fragment、樹與節點數不變。成功時舊子節點陣列
  移交呼叫端。
- **Bridge：** `innerHTML_get`、`innerHTML_set`、`outerHTML_get`（只經 `node_arg` 取節點）。
  `innerHTML_set` 對每個舊子節點呼叫 `node_removed`，再 `after_mutation(js, true)`（ID 全域重算＋
  `invalidated`，中斷往外傳）。錯誤：上限 → `Error('Document node limit reached')`；解析錯誤 →
  可攔截的 `Error('HTML parsing failed: invalid parser stack')`（Python 為 bridge `IndexError`，
  差異測試規則視為 native `Error`）。
- **Parser 一致性：** 屬性 entity 不解碼、`<br/>` 成為標籤 `br/`、空屬性與無值屬性皆為 `""`、屬性名
  casefold、重複屬性位置，native parser 原本就與 Python 一致，未修改 parser。`tests/dom_differential.py`
  新增 6 個情境，並加強為（1）屬性依順序比對（原本 dict 比較忽略順序）、（2）每個情境以 AST 取出
  凍結 `JSContext.serialize_node`／`serialize_attributes` 與 `VOID_ELEMENTS`，比對 native
  `outerHTML`／`innerHTML`（`test_dom --serialize`／`--serialize-inner`）；278 個情境全部相符。
- **差異測試：** `js_dom_differential` 移除 10 個切片 3 pending，22 個情境與 oracle 相符、剩 4 個
  pending（切片 5 cookie×2、切片 6 RAF×2）；未改 `js_dom_cases.py` 與 fixture，
  `js_dom_oracle_probe.py --check` 仍相符。另以臨時 oracle probe 實跑（不入固定情境）：setAttribute 值含
  `' " & < >`、`<br/>`、entity 屬性、emoji、`<body id=…>` 包裝、`<script>` 不執行、`</div></body></html>`
  前綴、改寫 `html` 元素的 `innerHTML`、解析錯誤後樹不變——全部相符；唯一不同是字串含 U+0000（native
  在 NUL 處截斷），記入 PORTING_PLAN.md「已知差異與範圍」。
- **單元測試：** `test_dom.c`（以 `--wrap` 注入配置失敗）：序列化規則、最後一個 body、舊子節點移交、
  解析錯誤不變、每個配置點失敗時樹與節點數不變、序列化配置失敗回 NULL、D2 上限（剛好達上限成功、
  多一個失敗）。`test_js.c`：getter 不觸發回呼、`innerHTML` 的 `node_removed`／`invalidated` 次數、
  ID 全域刪加、`null` 丟 `TypeError`、解析錯誤可攔截、節點上限錯誤且回呼與樹不變。
- **證據：** Debug CTest 48/48；`build-asan/` 全套 48/48（ASan/UBSan＋LSan 開啟，只排除文件記載的
  fontconfig／cairo 洩漏）。指令：`cmake --build build -j 4`、`ctest --test-dir build
  --output-on-failure -j 3`、`ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=<檔>
  ctest --test-dir build-asan --output-on-failure -j 3`、`python3 tests/js_dom_oracle_probe.py --check`。
- **未做：** 獨立 `ownership-reviewer` 審查留待切片 7；標題的 JS 改寫整合屬切片 4。

- `dom.c` 新增 `tai_node_serialize(node, bool outer)`，規則照 oracle（見上）；確認 native parser
  對屬性 entity、`<br/>`、空屬性與 Python 一致，不一致就先修 parser（`dom_differential.py` 加情境）。
- `tai_node_set_inner_html` 改取最後一個 `body`，修正錯誤註解；確認配置失敗時 fragment 整批回收。

### 切片 4：頁面整合

**狀態：完成（2026-09-29）。**
- **Dirty 期間不查 layout：** `browser.c` 新增 `settle_layout()`（dirty 時先 `rebuild_dirty_page`）。
  `tai_page_activate_viewport` 進入時、input caret 計算前、`apply_fragment_url` 捲動前都先 settle；
  其餘入口（text input、key、blur）原本就在回傳前重建，錯誤提前 return 時由下一次入口的 settle
  補上。`page_finish_visual` 在第一次 layout 前清掉 dirty：載入期腳本已包含在第一個 frame，不再讓
  載入後第一個事件多做一次重建。Fragment 捲動因此與 Python 相同（Python 在 `render` 重新 layout
  後才 `scroll_to_fragment`）。Caret 以重建後的 layout 計算；Python 以派送前命中的 input box 幾何量
  新值，只有 listener 改變該 input 位置或字型時不同，記入 D8 列。
- **焦點清除（D8）：** Python **不**清焦點（`removeChild`／`innerHTML_set` 只 `detach`，`self.focus`
  留在脫離的 input，之後按鍵照樣改它的 `value`）。使用者指定本切片清焦點，比照真實瀏覽器，記為
  刻意差異 D8。實作：`TaiJsHost.node_removed` 回呼（`browser.c` `node_removed`）檢查被移除子樹是否含
  `page->focused`，是就清 `page->focused`／`node->focused` 並設 dirty；同一操作再接回別處（移動）也
  清。`tai_page_text_input` 在 `keydown` 派送後重新檢查焦點；點擊時只聚焦仍在文件內的 input。不變式：
  `page->focused` 一律在文件內。
- **標題：** `tests/test_title_window.c` 新增 `dom_change` 情境，`tests/title_integration.py` 移除
  略過，比對 `title_oracle.json` 已凍結的問題 12（未重新凍結）；33 個視窗檢查點相符。
- **整頁 oracle：** `tests/js_page_fixture.py`（fixture server 與情境）、`tests/js_page_oracle_probe.py`
  （真的 `BrowserApp`／`Tab`，SDL dummy，CTest `js_page_oracle_probe`，連跑三次輸出相同）凍結
  `tests/fixtures/js_page_oracle.json`；`tests/js_page_probe.c` 經公開 `TaiPage` 介面重放相同動作，
  `tests/js_dom_integration.py`（CTest `js_dom_integration`）逐點比對 5 個情境、9 個檢查點：載入期
  mutation（`createElement`／`appendChild`／`insertBefore`／`removeChild`／`innerHTML`／`setAttribute`／
  改標題）、點擊 listener 兩次改 DOM 與標題、丟錯 listener 後另一 listener 照常、fragment 連結的
  listener 把目標往下推（捲動 464.34px）、`keydown` listener 移除焦點 input。只有
  `focus.after_key.focus` 套用 D8。以切片 4 前的 `browser.c` 重跑時 fragment 捲動為 0，測試失敗。
  整頁情境走 `TaiPage` 輸入介面而非 dummy SDL 視窗；SDL 路由已由既有視窗測試覆蓋，Xvfb 真實視窗
  留在切片 7。
- **單元測試：** `tests/test_browser.c` `check_script_mutation`：載入期 mutation 後閒置點擊不換 frame、
  fragment 用重建 layout、`removeChild`／祖先 `innerHTML`／移動三種移除都清焦點且不改值、移除無關
  節點保留焦點、點擊 listener 移出 input 不聚焦、點擊 listener 改標題。以切片 4 前的 `browser.c`
  重跑會失敗。
- **證據：** Debug CTest 50/50；`build-asan/` 全套 50/50（ASan/UBSan＋LSan 開啟，只排除文件記載的
  fontconfig／cairo 洩漏）；`python3 tests/js_page_oracle_probe.py` 連跑三次輸出相同。
- **未做：** `ownership-reviewer` 審查留待切片 7。

原計畫：
- 規定 `page->dirty` 期間不得查 layout：`browser.c:1378` caret、`1277` fragment 捲動等位置在查詢前
  先重建，或在 JS 回傳後立即重建；JS 錯誤路徑也要重建。
- `node_removed` 回呼：被移除的子樹含焦點節點時清除 `page->focused` 與 `node->focused`。
- 標題：擴充 `tests/title_integration.py`，比對 `title_oracle.json` 已凍結的問題 12，不重新凍結。
- 新增 `tests/js_dom_integration.py`（CTest `js_dom_integration`）：fixture server 提供頁面，外部 script
  修改 DOM、點擊觸發 listener、listener 丟錯；headless JSON 與 dummy SDL 比對 oracle。

### 切片 5：cookie 與同步 XHR

先寫設計、交 `ownership-reviewer` 審查，再實作。設計原則：

- **注入介面：** `tai_js_create` 接收 `TaiJsHost` 回呼表（`cookie_get`、`cookie_set`、`xhr_send`），
  由建立 context 的一方注入；js.c 不直接認識 network 與執行緒。
- **載入期**（loader 執行緒上執行的外部 script）：直接使用 loader 自己的 `TaiNetwork`，不經佇列。
  但 loader 是單一 poll 迴圈，同步 XHR 會暫停所有分頁的載入，需量測並記錄。
- **事件期**（SDL 執行緒）：透過 request/response 佇列交給 loader，loader 以獨立 job 處理；
  SDL 端用 `pthread_cond_timedwait` 等待，檢查 `stopping`、task 取消與總時限。**loader 永不等待 SDL
  執行緒**，避免環形等待。等待期間不處理 SDL 事件，所有視窗無回應，最長到 libcurl 30 秒總時限
  （D9，決定 2）。
- **同步載入路徑**（`session.c`、`main.c` 的 `tai_page_load_request`，headless CLI）在呼叫者執行緒直接使用
  network，host 由該路徑注入。
- **Deadline：** XHR 阻塞時間不計入 2 秒 JS 上限。
- **銷毀順序：** 先取消 XHR job，再 join loader，最後銷毀 page。
- 行為：CSP 阻擋、跨來源帶 `Origin`、ACAO 不符丟錯、`method` 只當標籤、`send()` 無引數為 `null` body；
  cookie 規則照 `JSContext.document_cookie_get/set`。
- 測試：`tests/network_fixture.py` 增加 XHR／CORS 路由；載入期 XHR 不死結、XHR 期間關視窗、
  XHR 期間導覽、慢伺服器超過 2 秒仍成功。

### 切片 6：requestAnimationFrame

- 「需要 frame」旗標放在 `TaiPage` 內（不放 tabset／window 全域佇列），導覽換頁後自然失效；視窗
  frame 迴圈以目前已提交的 page 取 context 執行 `runRAFHandlers()`；RAF 執行中觸發導覽時延後替換。
- Headless 行為依切片 0／6 凍結結果。
- 測試：dummy SDL 下 callback 次數、順序與 DOM 結果比對 oracle；RAF 中導覽。

### 切片 6b：補回 timer 與非同步 XHR（決定 1）

- JS 端規格：`server.py` 的 `SCHEDULING_RUNTIME_JS`（`setTimeout`、`runSetTimeout`、可非同步的
  `XMLHttpRequest`、`runXHROnload`）；另依同樣模式補 `setInterval`／`clearInterval`／`runSetInterval`。
  以 native 專用前言加在凍結 runtime.js 之後，凍結檔不改。
- Python 端規格：凍結 browser.py 的 `JSContext.setTimeout`、`setInterval`、`clearInterval`、`discard`、
  `XMLHttpRequest_send(isasync=True)`、`dispatch_xhr_onload`：延遲換算（非數字、負值、非有限值為 0）、
  callback 只在 Tab 主執行緒執行、導覽 discard 後已排入的 callback 變 no-op、非同步 XHR 失敗不觸發
  onload、CSP／CORS 與同步相同。
- native 設計：timer 與非同步 XHR 完成都只「排入」頁面擁有者執行緒的工作，不在其他執行緒執行 JS；
  timer 需接到 SDL 迴圈的等待時間（沿用 `src/scheduler.c` 或視窗 frame 迴圈），headless 在輸出前
  是否等待 timer 由輔助 oracle 決定。頁面銷毀時取消所有 timer 與進行中的 XHR。
- Oracle：Python 端行為以凍結 `JSContext` 為準；JS 包裝以 `server.py`（或 `7d536e0^` 的 browser.py）
  作輔助 oracle，凍結到 `js_dom_oracle.json` 的獨立區段並註明來源。
- 測試：延遲順序、`clearInterval`、callback 內再排 timer、導覽後舊 timer 不執行、非同步 XHR onload
  與失敗、關視窗時 timer／XHR 進行中（ASan＋LSan）。

### 切片 7：收尾

- 完整 CTest；`build-asan/` 全套 ASan/UBSan＋LSan，含反覆 load → mutate → render → close；
  配置故障掃描涵蓋 createElement、insertBefore、innerHTML、ID 同步刪除路徑。
- Xvfb 真實視窗（`native-window-verification` skill）：點擊觸發 mutation、標題改變、丟錯 listener、截圖。
- `ownership-reviewer` 獨立審查。
- 紀錄：
  - 新增 `docs/acceptance/<日期>-js-dom.md`，在 `ACCEPTANCE.md` Slice evidence 表加一列（`VALIDATING`）。
  - `PORTING_PLAN.md`「目前工作」加第 7 項；子系統表更新「JavaScript / events」（移除已具備的
    bubbling，timers/fetch 依決定改寫）、「DOM / HTML」、「URL / HTTP」（XHR／CORS）。
  - 刻意差異寫入 `PORTING_PLAN.md`「已知差異與範圍」，每項含原因、可觀察影響、移植後果。
  - cookie／XHR 執行緒所有權寫入 `docs/architecture/native-runtime.md`。
  - 修正 `docs/architecture/python-reference.md` 144–147 行「inline scripts … processed」的錯誤敘述與
    `run_animation_frame` 行號。

## 綁定升級路線（本工作不做，記錄供日後參考）

本工作採 runtime.js＋整數 handle，以最低成本對齊 oracle。這個設計日後會卡住的地方：C 端無法得知 JS
是否仍持有某個 handle，所以 detached 節點永不回收（D2 上限的由來）；每次存取產生新 wrapper，網頁掛在
元素上的自訂屬性、以節點為 key 的 `WeakMap`／`Set`、`===` 比較都不可靠；`LISTENERS` 全域表的 callback
不隨節點回收；只有單一 `Node` 型別，沒有 `HTMLElement` 等階層與 `instanceof`。

守住切片 2 的三條綁定邊界後，可分兩步升級，改動限於 `js.c` 與 native 前言 JS：

1. **Wrapper 唯一：** JS 端依 handle 快取 `Node` 物件，改動僅數行。會改變 oracle 可觀察行為（例如
   刪除後再加回的 ID 全域在 Python 是新物件），須記為刻意差異。
2. **真實瀏覽器式綁定：** 改用 QuickJS class（`JS_NewClass`＋opaque `TaiNode *`＋finalizer），
   JS 物件直接持有節點，listener 改掛在節點上，GC 回收 wrapper 時通知 C 端，detached 節點即可釋放並
   移除 D2 上限。`dom.h`、頁面整合與 oracle 差異測試沿用；差異測試作為回歸保護，行為變化逐項記為刻意差異。

## 預期的刻意差異

| 編號 | 項目 | 理由 |
|---|---|---|
| D1 | `log` 與 crash 訊息輸出到 stderr（Python stdout） | native JSON CLI 以 stdout 輸出結果 |
| D2 | 文件節點數上限 | Python 靠 GC 回收 detached 節點，native 保留到文件銷毀 |
| D3 | `node.handle` 數值 | 只有確認頁面不可觀察時才列 |
| D4 | 載入期同步 XHR 期間新的導覽要等它結束才開始；已送出的傳輸照常推進，其他分頁已完成的下載延到 XHR 結束才處理（巢狀 poll 只派送自己的請求與事件期 XHR） | native 單一 loader 迴圈；設計見 [cookie／XHR 設計](js-cookie-xhr-design.md) 10.2 |
| D5 | 提供 `setTimeout`、`setInterval`／`clearInterval`、非同步 XHR（凍結 oracle 不可用） | 原版 `7d536e0` 意外移除 JS 包裝；使用者 2026-09-29 決定補回 |
| D6 | 執行 inline `<script>`（Python 只執行有 `src` 的 script），與外部 script 依 source 順序執行；頁面有 CSP `default-src` 時不執行 inline script | 真實網頁大量依賴 inline script；使用者 2026-09-29 決定保留。oracle 比對的頁面含 inline script 時，該腳本造成的 DOM 變化不在 Python 答案內，測試頁面以外部 script 為主 |
| D7 | Listener 丟錯只影響該 listener：同節點其他 listener 與冒泡照常執行，先前的 `preventDefault` 有效（Python 會中斷整個派送、default action 一律照做） | 真實瀏覽器語意；使用者 2026-09-29 決定。oracle 比對只對「丟錯 listener」情境套用此規則 |
| D8 | 移除含焦點 input 的子樹時清除焦點（Python 焦點留在脫離的節點，按鍵仍改它的值） | 真實瀏覽器語意；使用者 2026-09-29 指定為切片 4 範圍 |
| D9 | 事件期同步 XHR 等待期間所有視窗無回應（不重繪、不處理輸入、無法關閉），最長約 30 秒傳輸總時限（跨重新導向）加上 loader 正在執行的單一載入期 script 時間（Python 只卡該 Tab，視窗照常）；設計見 [cookie／XHR 設計](js-cookie-xhr-design.md) 10.5 | native JS 在 SDL 執行緒執行；使用者 2026-09-29 選擇接受（決定 2） |

## 不在本工作範圍

`fetch`、capture phase、`parentNode` 等 runtime.js 沒有的 DOM API。

## 決定（2026-09-29，使用者確認）

1. **Timer 與非同步 XHR：補回。** `setTimeout`、`setInterval`／`clearInterval` 與非同步 XHR 都提供
   （切片 6b，刻意差異 D5）。理由：原版 `7d536e0` 意外移除，且 ACCEPTANCE 要求「實際可用
   scheduling APIs」。`setInterval`／`clearInterval` 原版從未有 JS 包裝（非誤刪），使用者另行確認一併補上。
2. **cookie／同步 XHR 的執行緒：** 採切片 5 的注入介面＋佇列設計。原理由寫「事件期 XHR 期間該視窗
   不重繪，與 Python 同步 XHR 阻塞 Tab 相同」，經對照 oracle 修正：Python 的同步 XHR
   （`JSContext.XMLHttpRequest_send` → `network.run_sync`）只卡該 Tab 的主執行緒，視窗執行緒仍重繪、
   可切分頁與關閉；native 的 JS 在 SDL 執行緒執行，等待期間**所有視窗**都不重繪、不處理輸入、無法
   關閉，最長到 libcurl 總時限 30 秒（連線 10 秒）。使用者 2026-09-29 在三個選項（接受卡住／等待中只
   處理關閉／每分頁獨立 JS 執行緒）中選擇**接受卡住**，記為刻意差異 D9。
3. **`log` 與 crash 訊息：** 輸出到 stderr（D1）。
4. **文件節點數上限：** 1,000,000，超過丟 JS 錯誤（D2）。
5. **Inline script：保留執行，CSP 下阻擋**，記為刻意差異 D6。比照真實瀏覽器：頁面有有效的 CSP
   `default-src` 時一律不執行 inline script。`'unsafe-inline'`、nonce、hash 不支援，因為 Python 與 native
   的 CSP 解析都只接受 URL 來源。
6. **Listener 丟錯：比照真實瀏覽器**，逐一隔離 listener（切片 1，刻意差異 D7）。

## 風險

- **Mutation 與 layout 借用：** layout 以 `const TaiNode *` 借用 DOM；dirty 期間不得查詢。
- **QuickJS OOM：** 新 bridge 每個 `JS_New*`／`JS_SetProperty*` 的失敗都要檢查（現有
  `collect_matches` 與 dispatch 忽略回傳值），每個 operation 走單一清理出口。
- **死結與取消：** 見切片 5；以測試覆蓋而非只靠審查。

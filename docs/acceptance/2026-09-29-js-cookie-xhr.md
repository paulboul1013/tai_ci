# JS DOM 切片 5：document.cookie 與同步 XHR（2026-09-29）

狀態：VALIDATING。設計見 [cookie／XHR 設計](../js-cookie-xhr-design.md)（第 11 節為實作差異），
刻意差異 D4、D9 與 XHR 取消／逾時訊息見 `PORTING_PLAN.md`「已知差異與範圍」。

## 證明了什麼

- **Oracle 凍結：** `tests/js_page_fixture.py` 新增 5 個情境（載入期 XHR 23 項：同源 GET／POST／空
  POST／相對 URL／fragment、JS cookie 送出、Set-Cookie 後讀取、HttpOnly 隱藏且不可覆寫、跨來源
  ACAO 相符／`*`／不符／缺少、相對與絕對重新導向、404、連線拒絕、`data:`、未 `open`、非同步、
  method 只是標籤；點擊 listener 內的 XHR 兩次；CSP 擋跨來源；`Referrer-Policy: no-referrer`；
  2.3 秒慢伺服器）。`tests/js_page_oracle_probe.py --check` 重跑相符，原 5 個情境不變。Python 的相對
  `Location` 會丟掉 port（重新導向失敗），兩邊一致。
- **Native 比對：** `tests/js_dom_integration.py` 每個情境跑 headless 與 `--tabset`（載入期腳本在
  loader、點擊在呼叫執行緒經佇列）兩種路徑，10 情境 × 2、32 個檢查點全部相符。
  `js_dom_differential` 接上 2 個 cookie 情境（24 相符、剩 2 個 RAF pending）。
- **執行緒與取消（`tests/test_tabset_xhr.c`，CTest `tabset_xhr`）：** 吃掉所有錯誤的載入期 XHR 迴圈在
  導覽取代後 0.02 秒讓新頁面提交；XHR 迴圈中銷毀 app 0.01 秒 join；另一視窗載入期 XHR（4 秒）
  進行中，點擊 listener 的事件期 XHR 0.12 秒完成；關閉等待中的視窗後 app 立即停止；兩分頁同時
  阻塞都完成。拿掉取消檢查時此測試 SIGABRT（mutation 驗證）。
- **單元：** `test_js`：host 錯誤碼、NULL host、非字串 body／URL、2.5 秒阻塞仍完成、延長上限 30 秒
  （XHR 迴圈 33.0 秒被中斷）、`cancelled` 不可攔截；`test_network`：jar 三執行緒交錯 2 萬次、受限
  poll 不派送他人完成、取消與 0.3 秒總時限。
- **Sanitizer：** Debug 乾淨重建 CTest 51/51；`build-asan/` ASan/UBSan＋LSan（`detect_leaks=1`，只排除
  `leak:libfontconfig.so`、`leak:libcairo.so`）51/51。
- **獨立審查：** `ownership-reviewer` 未發現確認缺陷；PLAUSIBLE 低風險項（受限 poll 以指標比對自己的
  請求，釋放後位址重用可能誤認）已改為請求旗標並重跑兩套 CTest。

## 主要缺口

- 未測：事件期 XHR 的 30 秒逾時（無可注入的短時限）、`xhr_closed`／`network_failed` 路徑、
  `checkpoint` 回 false 後跳過剩餘腳本的單獨斷言、新 bridge 的配置故障掃描（切片 7）、Xvfb 真實
  視窗（切片 7）。
- 載入期腳本本身長時間運算時（無 XHR、無 checkpoint），事件期 XHR 等到該腳本結束（D9 範圍內）。
- 導覽與 subresource 仍未帶 Referrer-Policy（只有 XHR 帶）。

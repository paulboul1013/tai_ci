# C17 移植狀態

Python source 已定位於 tai_gar 並固定工作樹快照；目前尚未達成整體驗收。
tai_ci 原有 main.c/Makefile 刪除狀態保持不變。

| Subsystem | Python source | C destination | Dependencies | State | Behavioral tests | Known discrepancies |
|---|---|---|---|---|---|---|
| Core / ownership | Python builtins | src/core.c | C17 | INTERFACE_DEFINED | allocation/map/string tests | C 明確 error return |
| DOM / HTML | browser.py Text/Element/HTMLParser/ViewSourceParser | src/dom.c | core / Unicode | ANALYZED | normalized DOM differential | 待實作 |
| CSS / style | CSSParser/selectors/style | src/css.c | DOM/core | ANALYZED | rule/selector/style differential | 待實作 |
| URL / HTTP | URL/cookie/referrer helpers | src/url.c / src/network.c | core/libcurl multi/OpenSSL/zlib | ANALYZED | local HTTP corpus / URL differential | 待實作；不可採 curl 自動 cookie/redirect 語意 |
| Fonts / layout | Document/Block/Line/Text/controls | src/layout.c | DOM/CSS/HarfBuzz/FreeType/FriBidi | ANALYZED | geometry/text/display differential | Skia→native stack 須量測差異 |
| Paint / raster | Draw*/Blend/Blur/Scroll/Raster* | src/render.c | Cairo/layout | ANALYZED | structural commands / PNG / clipping | GPU 策略待驗證 |
| JS / events | JSContext/runtime.js | src/js.c | QuickJS-NG/DOM/CSS/network | ANALYZED | bridge/event/XHR differential | runtime.js 缺少部分 Python hooks |
| Scheduling | TaskRunner/NetworkTaskRunner/frame clocks | src/scheduler.c | threads/network | ANALYZED | priority/cancellation/stale generation | 待實作 |
| Browser / window | BrowserApp/BrowserWindow/Tab/Chrome | src/browser.c / src/main.c | 上列全部 / SDL3 | ANALYZED | navigation/forms/history/tabs/windows E2E | 待實作 |

## 執行順序與驗證關卡

1. 固定 oracle、完成 dependency 分析及 C ownership contracts。
2. 先跑 Python cases，建立 DOM/CSS probe 的失敗測試；實作 core、DOM、CSS，build、differential、sanitizer。
3. URL/HTTP 與 layout/raster 可在共用 contracts 確定後平行實作；各自使用本機 deterministic fixtures。
4. 整合 native headless navigation→DOM→style→layout→PNG，驗證 rendering。
5. 接通 QuickJS-NG、事件與 network policy，再接 scheduler/SDL3/window orchestration。
6. 獨立 verification agent 對行為、ownership、stale work、resource destruction 與 E2E 主動找錯；修正後更新此表。

每項只有 functionality、build、tests、oracle、ownership review、差異文件、上下游 integration 全通過才標 COMPLETE。
Dependencies 固定 revision；不以可編譯或少數 smoke cases 代替完整驗收。

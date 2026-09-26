# Keyboard delivery, loader-thread allocation failure and QuickJS OOM — 2026-09-25

以 `SDL_EVENT_LOGGING=1` 啟動 `tai-browser --window` 記錄 SDL 實收事件。WSLg `:0`
上點擊地址列後，`xdotool key --window` 產生 `windowid=0` 的 KEY_DOWN 且沒有
TEXT_INPUT；`xdotool windowfocus` 使 SDL 在約 0.8ms 內先 FOCUS_GAINED 再
FOCUS_LOST，其後 XTEST 按鍵沒有任何 SDL 事件。WSLg compositor 會撤回 X11 焦點，
此路徑仍無法作為鍵盤證據，瀏覽器維持忽略 `windowID=0`。

改在無 window manager 的隔離 `Xvfb :99` 執行同一 binary 與 fixture，XTEST 按鍵經
X server 焦點分派。點擊地址欄右端、80 次 BackSpace、輸入
`http://127.0.0.1:8765/second.html` 後按 Return：267 個鍵盤／文字事件全為
`windowid=2`，`windowid=0` 為 0，TEXT_INPUT 33 筆與 URL 長度一致，順序為每鍵
KEY_DOWN→TEXT_INPUT→KEY_UP；server 記錄 `GET /second.html 200`，截圖
[`xvfb-keyboard-address.png`](/tmp/tai-tabs-acceptance/xvfb-keyboard-address.png)
依序顯示清空、輸入與 Second Page，事件記錄在
[`xvfb-keyboard-events.txt`](/tmp/tai-tabs-acceptance/xvfb-keyboard-events.txt)。先前一次
點在 URL 中段再按 Ctrl+A 的嘗試送出 `/indhttp://…second.htmlx.html`；frozen Python
同樣以點擊 x 決定游標且不處理 Ctrl+A，因此這是等價行為，不是缺陷。這驗證真實 X11
server→SDL→地址列的鍵盤順序；WSLg／Wayland compositor 的焦點交付仍未驗證。

loader thread 配置故障以 linker `--wrap=malloc,calloc,realloc,pthread_create` harness
注入：只標記 TabSet loader thread，從第 i 次配置起持續失敗，涵蓋專案與 QuickJS 靜態碼，
不含 curl/Cairo/Fontconfig 共享函式庫。每個 i 檢查 pending 會結束、失敗時保留原頁與
history、成功時 history 前進，再以同 tab 無故障導覽恢復。第 190 點在
`JS_NewContextRaw()` 的 `class_proto` 配置失敗時，QuickJS-NG `df836d1`（上游 master
`19dbe85` 仍相同）已 `add_gc_object()` 卻直接 `js_free_rt(ctx)`，其後
`JS_FreeRuntime()` 走訪已釋放 GC 節點，ASan 回報 SEGV。這是可重現的依賴端
use-after-free。以 scratch 複本在釋放前加 `remove_gc_object(&ctx->header)` 後，
`data:` 導覽／初始載入分別掃過 444／396 點、本機 HTTP 分別掃過 791／835 點至無故障
完成，ASan/UBSan/LSan（僅 Fontconfig suppression）全部 exit 0。

此修補已成為 tracked
[`patches/quickjs/0001`](../../patches/quickjs/0001-unlink-context-on-class-proto-oom.patch)，
CMake configure 時套用至 `deps/quickjs`，已存在則略過，無法套用則停止；無自身
`.git` 的 checkout 也以 `GIT_CEILING_DIRECTORIES` 驗證可套用與偵測。新增兩個 CTest：
`quickjs_oom` 以毒化並延後歸還的自訂 QuickJS allocator 逐點失敗 `JS_NewContext()`，
未修補的 libqjs 在 `gc_decref` 斷言 abort，修補後 57 點通過；
`tabset_loader_oom` 為上述 `data:` loader-thread 掃描（本機 HTTP 掃描未納入 CTest）。
`build` 完整 CTest 32/32 通過；`build-asan` 兩個新測試在 ASan/UBSan/LSan
（Fontconfig suppression）下通過。上游 quickjs-ng 尚未回報。

code review 指出 button 座標以即時視窗尺寸換算、命中以已處理尺寸判斷。換算比例是
pixel density，resize 不改變它；命中使用使用者點擊當下已呈現的版面。剩餘風險只在
display scale 改變與 click 同時排隊，未另建自動測試。

# WSLg manual keyboard delivery — 2026-09-25

WSLg `:0` 上以 `SDL_EVENT_LOGGING=1 SDL_VIDEO_X11_XINPUT2=0` 啟動 `tai-browser --window`
本機 fixture，由使用者以實體鍵盤操作：點擊地址欄右端、BackSpace 編輯、輸入
`.htmX`、修正，最後清空並輸入 `http://127.0.0.1:8765/second.html` 後按 Enter。SDL
記錄在點擊前已 FOCUS_GAINED；266 個 KEY_DOWN／KEY_UP／TEXT_INPUT 全為 SDL
`windowid=2`，無 `windowid=0`；每筆 TEXT_INPUT 都緊接在其 KEY_DOWN 後，含 61 次
BackSpace 與 1 次 Enter，文字依序為 `.htmX`、`l`、`second` 與完整 URL。Enter 後
server 記錄 `GET /second.html 200`，截圖
[`wslg-manual-keyboard-after.png`](/tmp/tai-tabs-acceptance/wslg-manual-keyboard-after.png)
顯示地址欄與 Second Page，事件見
[`wslg-manual-keyboard-events.txt`](/tmp/tai-tabs-acceptance/wslg-manual-keyboard-events.txt)。
FOCUS_LOST 出現在 Enter 之後，對應使用者切回其他視窗。這驗證 WSLg compositor→
X11→SDL→地址列的實體鍵盤交付；自動注入仍受 WSLg 焦點撤回限制，Wayland 原生
backend 未驗證。

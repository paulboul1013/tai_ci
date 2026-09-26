# Native Wayland address keyboard delivery — 2026-09-26

在隔離的 `Xvfb :98` 上執行 Weston 13 的 X11 backend、Pixman renderer 與 kiosk shell；
Weston 的 X11 output ID 為 `2097157`。本機未安裝 Weston，因此只把 Ubuntu
`weston`/`libweston-13-0` 套件解到 `/tmp`，並在暫存副本改寫其固定模組搜尋路徑；
未改動系統安裝或專案程式碼。fixture server 綁定 `127.0.0.1:8765`。browser 以
`SDL_VIDEODRIVER=wayland WAYLAND_DISPLAY=tai-wayland-test SDL_EVENT_LOGGING=1`
啟動，SDL 記錄 Wayland window `4` 的 `WINDOW_SHOWN` 與 `FOCUS_GAINED`。

重跑時以 `Xvfb :98 -screen 0 1280x800x24 -nolisten tcp` 建立隔離 display，在
`DISPLAY=:98` 上啟動 Weston `--backend=x11 --shell=kiosk --renderer=pixman`
`--socket=tai-wayland-test --width=1024 --height=768 --no-config`，啟動上述 fixture
與 browser 並將 stderr 分別保存為 server/event log。先對隔離 X server 執行
`DISPLAY=:98 xset r off`，從本次 Weston log 取得 X11 output ID，再用下列命令
透過 Weston X11 window 把輸入送進 Wayland seat
（將 `2097157` 換成本次 ID）：

```bash
DISPLAY=:98 xdotool mousemove --window 2097157 760 63 click 1 windowfocus 2097157
DISPLAY=:98 xdotool key --repeat 80 --delay 5 BackSpace
DISPLAY=:98 xdotool type --delay 80 'http://127.0.0.1:8765/second.html'
DISPLAY=:98 xwd -silent -id 2097157 -out /tmp/tai-wayland-keyboard-2026-09-26/typed.xwd
DISPLAY=:98 xdotool key Return
DISPLAY=:98 xwd -silent -id 2097157 -out /tmp/tai-wayland-keyboard-2026-09-26/after.xwd
```

實際記錄中，click 為 `windowid=4 x=760 y=63`；focus 曾在 click 後短暫 LOST，
`windowfocus` 後 GAINED，所有後續 Backspace、33 筆 `TEXT_INPUT` 與 Return
`KEY_DOWN/KEY_UP` 均為 `windowid=4`，直到 Return 前沒有再次失焦。送出前截圖顯示
地址欄為完整 `http://127.0.0.1:8765/second.html` 且仍是 Tab Zero；fixture
收到 `GET /second.html HTTP/1.1` 200；送出後非空白截圖顯示 Second Page。
這證明輸入經 XTEST→Weston X11 backend→Wayland seat→SDL3→地址列與導覽。
第一次使用 Xvfb 預設 auto-repeat、`xdotool type --delay 30` 時出現重複 `t`；
停用 auto-repeat 並改為 80ms 間隔後，文字事件數與 URL 長度一致。
截圖與 event log 原存於本次 `/tmp/tai-wayland-keyboard-2026-09-26/`，已目視檢查，
但執行環境在後續對話回合重置 `/tmp`，檔案未保留；上述命令與事件條件是重跑檢查。
這是隔離 Weston 的原生 Wayland backend 證據，未涵蓋 WSLg 原生 Wayland seat。
同一 binary 的 focused `presentation_dummy`、`presentation_chrome`、`browser_tabs`
3/3 通過；完整 CTest 32/32、`python3 tests/tabs_oracle_probe.py --check` 通過。
本次只有驗證文件變更，沒有程式碼修補或新增 sanitizer 案例。

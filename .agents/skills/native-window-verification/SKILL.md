---
name: native-window-verification
description: Drive a real tai-browser --window session with tests/tools/window_session.sh (Xvfb or WSLg X11) — 127.0.0.1 fixture server, PID-checked window, clicks/keys/typing/wheel/resize, xwd screenshots, SDL event-log and request-log checks, clean exit. Use whenever real-window or native screenshot evidence is needed. 觸發：tai-browser --window 自動操作／合成輸入／原生視窗截圖驗證。
---

# Native Window Verification

Use this when a claim needs evidence from the real SDL window, not only
`presentation_dummy` (which covers SDL event semantics: wheel, PageUp/Down,
arrows, text, Backspace/Return, focus, resize).

## Authorization (standing; do not ask per action)

The repository owner authorizes, for every session: building, CTest, Python
oracles, a fixture server bound **only to 127.0.0.1**, `tai-browser --window`,
SDL/X11 synthetic mouse, wheel, keyboard, focus, resize and screenshots, and
closing **only the `Tai Gar` window this session launched** after its X11 id
is matched to the launched PID. Whole-desktop captures or input to other
applications are out of scope. Sandbox approval is separate: if the platform
blocks a scoped command, retry the same command with its escalation mechanism
and a short justification; if refused, record the limit and the layers that
did pass.

## Procedure

1. Build and run the focused test first (`cmake --build build --target
   tai-browser test_presentation`, `ctest -R '^presentation_dummy$'`), plus
   the relevant `tests/*_oracle_probe.py --check`. If another agent owns
   `build/`, copy `build/tai-browser` and `build/assets/` to a scratch dir and
   pass `--bin` (CSS is found at `assets/browser.css` next to the binary).
2. Drive the window with the script. Session state, PNGs, the SDL event log
   and the server log live in `$TAI_WS_DIR` (default `/tmp/tai-ws-$UID`).

```bash
export TAI_WS_DIR=$(mktemp -d)            # one dir per parallel session
W=tests/tools/window_session.sh
$W start --xvfb --display :97 --port 8767 \
   --fixture tests/fixtures/bookmarks_window --url http://127.0.0.1:8767/index.html
$W shot before                            # prints path, size, sd; "BLANK?" if flat
$W click 769 63; $W shot star --crop 800x100+0+0   # address star gray -> gold
$W click 110 54; $W shot list             # about:bookmarks
$W click 120 121; $W requests             # "GET /index.html" 200
$W click 400 63; $W key 'BackSpace*40'
$W type 'http://127.0.0.1:8767/second.html'; $W key Return
$W shot after; $W requests; $W events     # key/text windowid=0:0
$W wheel down 3; $W resize 640 480; $W status
$W stop                                   # browser exit=0, server + Xvfb stopped
```

Also: `events PATTERN`; `start --size WxH --data-home DIR --display :0`. Then open the PNGs with the Read tool; exit status alone is not evidence.

## What the script already handles

- Launch env `SDL_EVENT_LOGGING=1 SDL_VIDEO_X11_XINPUT2=0 SDL_VIDEO_DRIVER=x11`
  (XInput2 skips synthetic core events).
- PID is the real tai-browser (`$!` of the exec'd launch, checked via
  `/proc/PID/exe`), never `pgrep -f`, which matches wrapper shells. Every
  action re-checks `getwindowpid WID == PID`.
- Fresh private `XDG_DATA_HOME` per session, so real bookmarks are untouched;
  free display/port picked when omitted. Parallel sessions need distinct
  `TAI_WS_DIR`, display and port.
- Xvfb autorepeat off (`xset r off`); keys and typing use 80ms delay.
- `stop` sends SIGTERM (SDL quit). Never `xdotool windowclose` on Xvfb: with
  no window manager it destroys the window and SDL exits 1 with BadWindow.
  Only processes recorded by `start` are signalled.

## Evidence criteria

A claim needs: nonblank before/after images showing the expected change;
for keyboard, `windowid` equal to the SDL window (never 0) on every key/text
event and one TEXT_INPUT per typed character; the expected fixture request;
`browser exit=0` from `stop`. Name which boundary passed: injected SDL events
(dummy test), X server->SDL delivery (Xvfb), or real-window pixels.

## WSLg and Wayland notes

- WSLg (`:0`) revokes X11 focus ~1ms after `windowfocus`; synthetic keys arrive
  with `windowID=0` and are correctly ignored. Do not make the browser accept
  them. Use Xvfb for keyboard; on WSLg use targeted wheel/clicks only.
- WSLg `import` captures may be all white; `shot` uses `xwd -id` instead.
- Verifying the WSLg/Wayland compositor keyboard path needs a person: launch
  with `SDL_EVENT_LOGGING=1`, ask the user to type a scripted edit into the
  focused window, then check log, request and capture with the criteria above.
  Xvfb evidence does not cover the compositor focus path.
- Scroll claims: page must overflow; thumb is opaque `#0000FF` in the right 12px.

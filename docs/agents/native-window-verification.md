# Native Window Interaction Verification

Use this procedure when an agent must operate `tai-browser --window URL` and
verify the visible result from its own screenshots. It covers the Linux/WSLg
X11 path observed on 2026-09-22. The screenshot is evidence of a real window;
`presentation_dummy` is the separate evidence for SDL event semantics.

## 1. Establish the event contract

Build the current tree and run the focused test before desktop automation:

```bash
cmake --build build --target tai-browser test_presentation -j 4
ctest --test-dir build -R '^presentation_dummy$' --output-on-failure
```

`tests/test_presentation.c` injects SDL wheel, pointer, text, and key events.
It covers flipped wheel direction, fractional/nonfinite input, unrelated
windows, scroll clamps, resize, active-control text input, focus loss/gain,
Unicode text, Backspace/left/right/Return, and a navigation callback replacing
the page while the same SDL window remains open. Record the result; this step
is complete only when the focused test passes on the binary being checked.

| Movement | SDL dummy evidence | Real-window automation |
|---|---|---|
| Wheel down/up | Direction, clamp, flipped and invalid input | Targeted X11 buttons `5`/`4` |
| PageDown/PageUp, down/up arrows | 100px step and clamp | Requires real keyboard focus; see step 4 |
| Text, Backspace/left/right/Return | Focused dummy SDL input and form navigation | Authorized for targeted tests; WSLg focus may prevent delivery (step 4) |

## 2. Start and identify one real window

Use a page known to overflow vertically. For the current visual fixture:

```bash
SDL_VIDEO_X11_XINPUT2=0 ./build/tai-browser --window 'https://browser.engineering/scheduling.html'
```

Keep that process running. In another terminal or tool call, obtain its X11 ID:

```bash
xdotool search --name '^Tai Gar$'
```

Resolve multiple matches to the process you started before sending input. If
network or display access is denied by the sandbox, retry through the tool's
required escalation mechanism and record the restriction separately from
browser behavior. This step is complete when exactly one target window is identified and its initial
capture shows page content plus a right-edge blue thumb.

### Authorization and sandbox scope

The standing user authorization and reusable commands are in
[`AGENTS.md`](../../AGENTS.md). Run those scoped
validation actions directly. Resolve the current `Tai Gar` X11 ID after each
launch; apply input and captures only to that window. Close only test windows
started by the agent. Batch consecutive same-window actions when no
intermediate capture is needed. Capture files belong in a temporary path
unless the user requested a destination.

Conversation authorization and sandbox execution approval are separate. If a
scoped command fails under sandboxing, retry it with the tool's required
escalation parameter and a concise justification; reuse a previously approved
scoped command or prefix when available. A platform approval prompt may still
appear for a new command shape. Do not add a separate conversational permission
request or treat each new coordinate as a new user decision. If the execution
platform rejects the scoped command, report that environment restriction.
Whole-desktop captures and commands targeting another application are outside
this browser verification procedure.

## 3. Inject movement and capture each state

For this WSLg/X11 setup, set `SDL_VIDEO_X11_XINPUT2=0` **at launch** and use
window-targeted wheel buttons: `5` moves down, `4` moves up. Substitute the
actual X11 ID below; an ID from an earlier process may be stale.

```bash
tai_window_id=REPLACE_WITH_ID # Replace with the ID found in step 2.
xdotool click --window "$tai_window_id" 5
xdotool click --window "$tai_window_id" --repeat 30 --delay 20 5
xdotool click --window "$tai_window_id" 4
```

The first command checks one 100px step. Use small batches, capture, and
repeat until the thumb reaches the requested region. On the scheduling page
at an 800×532 viewport, about 180 downward ticks placed the 20px thumb at
`y=255..274`; treat that count as an observation, not a fixed target for
other page lengths or window sizes.

For agent inspection, capture to a temporary path. For a user-requested
image, omit `--mode temp` so the helper saves to the OS screenshot location;
use `--path` when the user specified a destination:

```bash
tai_capture_helper=/home/paulboul/.codex/skills/screenshot/scripts/take_screenshot.py
python3 "$tai_capture_helper" --mode temp --window-id "$tai_window_id"
python3 "$tai_capture_helper" --window-id "$tai_window_id"
```

Inspect the returned file, not only the command's exit status. WSLg sometimes
returns an all-white `import` image for a live window; retry the capture. If
needed, capture the same window by its ID:

```bash
xwd -silent -id "$tai_window_id" -out /tmp/tai-window.xwd
convert /tmp/tai-window.xwd /tmp/tai-window.png
```

A white capture alone is not a browser failure. Replace the helper path above
with the installed `screenshot` skill path when working outside this machine.

For an 800×532 capture, inspect the right-edge pixel column and locate the
opaque `#0000FF` run:

```bash
tai_shot=/tmp/tai-window.png # Replace with the captured PNG path.
convert "$tai_shot" -crop 1x532+794+0 txt:- | rg '#0000FF' | head -1
convert "$tai_shot" -crop 1x532+794+0 txt:- | rg '#0000FF' | tail -1
```

Adapt the crop to the actual image dimensions: sample inside the rightmost
12px, away from its edge. A middle-position claim requires a nonblank page,
different visible content from the top capture, and a blue run whose center
is within 40–60% of the viewport height. A movement claim requires both the page
content and thumb to move in the expected direction. Save and link the actual
image used for the claim.

## 4. Interpret failures at the correct boundary

`xdotool key --window "$tai_window_id" Page_Down` produces a synthetic X11 core
`KeyPress`, but SDL's XInput2 path can skip it. With XInput2 disabled, this
WSLg session still lost keyboard focus and emitted an SDL key event with
`windowID=0`; the browser correctly ignored that event. Use the focused dummy
SDL test for PageUp/PageDown, arrow, and text/control behavior, and targeted
wheel input for real-window visual evidence. Do not change the browser to
accept unfocused `windowID=0` keys just to satisfy desktop automation. Targeted
keyboard injection is authorized for validation, but delivery still needs
matching before/after evidence before it counts as verified behavior.

WSLg revokes X11 focus about 1ms after `xdotool windowfocus`, so XTEST keys
reach no SDL window there. For real X11 keyboard evidence, run the same binary
on an isolated server without a window manager, where `XSetInputFocus` holds.
Launch with SDL event logging so the delivered order and `windowid` are
recorded:

```bash
Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
DISPLAY=:99 SDL_EVENT_LOGGING=1 SDL_VIDEO_DRIVER=x11 \
  ./build/tai-browser --window http://127.0.0.1:8765/index.html 2> /tmp/tai-xvfb-events.log
DISPLAY=:99 xdotool search --name '^Tai Gar$'
DISPLAY=:99 xdotool mousemove --window "$tai_window_id" 760 63 click 1
DISPLAY=:99 xdotool key --repeat 80 --delay 5 BackSpace
DISPLAY=:99 xdotool type --delay 30 'http://127.0.0.1:8765/second.html'
DISPLAY=:99 xdotool key Return
```

Omit `--window` on `key`/`type` so XTEST uses the server's focus. The Python
reference places the address cursor at the click x and ignores Ctrl+A, so
click past the text end and clear it with BackSpace. Evidence requires
`windowid` equal to the SDL window (not `0`) on every key and text event, one
TEXT_INPUT per typed character, the expected request in the fixture server
log, and `xwd -id` captures before and after. This verifies X server→SDL
delivery; it does not verify the WSLg or Wayland compositor focus path.

If a targeted wheel does not move the page, check the launch environment,
window ID, overflow, and scroll clamp, then compare captures. A command that
exits successfully without visible movement is not verification. Record which
boundary passed: injected SDL events, X11 event delivery, or real-window
pixels. The procedure is complete only when the requested action has a
matching nonblank before/after image and any unverified input path is named.

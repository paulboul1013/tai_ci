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
| Text, Backspace/left/right/Return | Focused dummy SDL input and form navigation | Not covered by the standing X11 automation scope |

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
network or display access is denied by the sandbox, retry with the required
approval and record the restriction separately from browser behavior. This
step is complete when exactly one target window is identified and its initial
capture shows page content plus a right-edge blue thumb.

### Elevated X11 authorization scope

Desktop automation can require elevated sandbox permission. When the user has
granted standing consent for native-window verification, invoke the scoped
commands directly; do not pause for a second conversational confirmation. The
permitted scope is limited to the `Tai Gar` window started in this procedure
and its newly resolved X11 ID:

- `xdotool search --name '^Tai Gar$'` to find the window;
- `xdotool click --window <current-id> 4|5` for targeted wheel input;
- `xdotool windowclose <current-id>` only to clean up that test window; and
- `xwd -silent -id <current-id>` to capture that same window.

Do not broaden this consent to whole-desktop captures, active-window lookup,
general keyboard injection, or another application's window ID. Capture files
belong in a temporary path unless the user requested a destination. If the
execution platform still refuses a scoped elevated command, report that as an
environment restriction rather than requesting browser code changes.

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
accept unfocused `windowID=0` keys just to satisfy desktop automation. The
standing native-window authorization does not include keyboard injection.

If a targeted wheel does not move the page, check the launch environment,
window ID, overflow, and scroll clamp, then compare captures. A command that
exits successfully without visible movement is not verification. Record which
boundary passed: injected SDL events, X11 event delivery, or real-window
pixels. The procedure is complete only when the requested action has a
matching nonblank before/after image and any unverified input path is named.

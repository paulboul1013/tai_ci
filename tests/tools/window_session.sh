#!/usr/bin/env bash
# Drive one real `tai-browser --window` session with PID-checked X11 input.
# Usage: window_session.sh [--dir D] CMD [ARGS]   (or TAI_WS_DIR=D)
#   start --url URL [--fixture DIR] [--port N] [--display :N | --xvfb]
#         [--data-home DIR] [--size WxH] [--bin PATH]
#   click X Y | key KEYS...(NAME*N repeats) | type TEXT | wheel up|down [N] | resize W H
#   shot NAME [--crop WxH+X+Y] | events [PATTERN] | requests | status | stop
# Only processes started by `start` are ever signalled; every action re-checks
# that the saved window id still belongs to the saved browser PID.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DIR=${TAI_WS_DIR:-${TMPDIR:-/tmp}/tai-ws-$(id -u)}
SETTLE=${TAI_WS_SETTLE:-0.4}

die() { echo "window_session: $*" >&2; exit 1; }
get() { [[ -f $DIR/$1 ]] && cat "$DIR/$1" || true; }
put() { printf '%s\n' "$2" > "$DIR/$1"; }
alive() { [[ -n ${1:-} ]] && kill -0 "$1" 2>/dev/null; }
# cmdline_has PID WORD: guard against signalling a recycled PID.
cmdline_has() { alive "$1" && tr '\0' ' ' < "/proc/$1/cmdline" | grep -q -- "$2"; }

if [[ ${1:-} == --dir ]]; then DIR=$2; shift 2; fi
CMD=${1:-status}; shift || true

load() {
  [[ -f $DIR/pid ]] || die "no session in $DIR (run start)"
  export DISPLAY; DISPLAY=$(get display)
  PID=$(get pid); WID=$(get wid)
}

# Re-verify window -> pid before any action.
check() {
  load
  alive "$PID" || die "browser pid $PID not running (rc=$(get rc))"
  local owner; owner=$(xdotool getwindowpid "$WID" 2>/dev/null || true)
  [[ $owner == "$PID" ]] || die "window $WID owner '$owner' != browser pid $PID"
}
settle() { sleep "$SETTLE"; }

free_display() {
  local n; for n in $(seq 90 199); do
    [[ -e /tmp/.X11-unix/X$n || -e /tmp/.X$n-lock ]] || { echo ":$n"; return; }
  done; die "no free X display"
}
free_port() {
  python3 -c 'import socket
for p in range(8765, 8900):
    s = socket.socket()
    try: s.bind(("127.0.0.1", p)); print(p); break
    except OSError: pass
    finally: s.close()'
}

cmd_start() {
  local url="" fixture="" port="" display="" xvfb=0 data="" size="" bin=${TAI_BROWSER:-$REPO/build/tai-browser}
  while (($#)); do case $1 in
    --url) url=$2; shift 2;; --fixture) fixture=$2; shift 2;;
    --port) port=$2; shift 2;; --display) display=$2; shift 2;;
    --xvfb) xvfb=1; shift;; --data-home) data=$2; shift 2;;
    --size) size=$2; shift 2;; --bin) bin=$2; shift 2;;
    *) die "start: unknown arg $1";; esac; done
  [[ -n $url ]] || die "start: --url required"
  [[ -x $bin ]] || die "start: browser not executable: $bin"
  if [[ -f $DIR/pid ]] && alive "$(get pid)"; then die "session already running in $DIR"; fi
  mkdir -p "$DIR"; rm -f "$DIR"/{pid,wid,rc,xvfb_pid,server_pid,server_port,display,url}
  bin=$(readlink -f "$bin")

  if ((xvfb)); then
    [[ -n $display ]] || display=$(free_display)
    [[ -e /tmp/.X11-unix/X${display#:} ]] && die "display $display already in use"
    setsid Xvfb "$display" -screen 0 1280x800x24 -nolisten tcp </dev/null >"$DIR/xvfb.log" 2>&1 &
    put xvfb_pid $!
    for _ in $(seq 50); do [[ -e /tmp/.X11-unix/X${display#:} ]] && break; sleep 0.1; done
    DISPLAY=$display xset r off 2>/dev/null || true   # no autorepeat while typing
  fi
  display=${display:-${DISPLAY:-}}; [[ -n $display ]] || die "no DISPLAY; pass --display or --xvfb"
  put display "$display"; export DISPLAY=$display

  if [[ -n $fixture ]]; then
    [[ -d $fixture ]] || die "fixture dir missing: $fixture"
    port=${port:-$(free_port)}
    setsid python3 -u -m http.server "$port" --bind 127.0.0.1 --directory "$fixture" \
      </dev/null >"$DIR/server.log" 2>&1 &
    put server_pid $!; put server_port "$port"
    for _ in $(seq 50); do
      python3 -c "import socket;socket.create_connection(('127.0.0.1',$port),0.2)" 2>/dev/null && break
      sleep 0.1; done
  fi

  [[ -n $data ]] || { data=$DIR/data; rm -rf "$data"; }
  mkdir -p "$data"; put url "$url"
  # Wrapper keeps the real browser as its child: $! inside is the exec'd
  # tai-browser (env execs), and the wrapper records the exit code.
  setsid bash -c 'env "$@" & echo $! > "$0/pid.tmp"; mv "$0/pid.tmp" "$0/pid"; wait $!; echo $? > "$0/rc"' \
    "$DIR" SDL_EVENT_LOGGING=1 SDL_VIDEO_X11_XINPUT2=0 SDL_VIDEO_DRIVER=x11 \
    DISPLAY="$display" XDG_DATA_HOME="$data" "$bin" --window "$url" \
    </dev/null >"$DIR/browser.log" 2>&1 &
  for _ in $(seq 50); do [[ -f $DIR/pid ]] && break; sleep 0.1; done
  local pid; pid=$(get pid); [[ -n $pid ]] || die "browser did not start"
  sleep 0.2
  [[ $(readlink "/proc/$pid/exe" 2>/dev/null) == "$bin" ]] || die "pid $pid is not $bin (rc=$(get rc)); see $DIR/browser.log"

  local wid=""
  for _ in $(seq 100); do
    for w in $(xdotool search --name '^Tai Gar$' 2>/dev/null || true); do
      [[ $(xdotool getwindowpid "$w" 2>/dev/null || true) == "$pid" ]] && { wid=$w; break; }
    done
    [[ -n $wid ]] && break; alive "$pid" || break; sleep 0.1
  done
  [[ -n $wid ]] || die "no 'Tai Gar' window owned by pid $pid (rc=$(get rc))"
  put wid "$wid"
  if [[ -n $size ]]; then xdotool windowsize "$wid" "${size%x*}" "${size#*x}"; fi
  sleep 1
  echo "started dir=$DIR display=$display pid=$pid wid=$wid${port:+ port=$port} data=$data"
  [[ $display == :0 ]] && echo "note: WSLg :0 drops synthetic key focus; use --xvfb for keyboard" || true
}

focus() { xdotool windowfocus "$WID" 2>/dev/null || true; }

cmd_click() { (($# == 2)) || die "click X Y"; check
  xdotool mousemove --window "$WID" "$1" "$2" click 1; settle; echo "click $1,$2 ok"; }
# key accepts NAME*N as shorthand for N repeats (e.g. BackSpace*40).
cmd_key() { (($#)) || die "key KEYS..."; check; focus
  local k keys=()
  for k in "$@"; do
    if [[ $k =~ ^(.+)\*([0-9]+)$ ]]; then
      for _ in $(seq "${BASH_REMATCH[2]}"); do keys+=("${BASH_REMATCH[1]}"); done
    else keys+=("$k"); fi
  done
  xdotool key --delay 80 "${keys[@]}"; settle; echo "key ${#keys[@]} keys ok"; }
cmd_type() { (($# == 1)) || die "type TEXT"; check; focus
  xdotool type --delay 80 -- "$1"; settle; echo "type ${#1} chars ok"; }
cmd_wheel() {
  local dir=${1:-}; local n=${2:-1} b
  case $dir in down) b=5;; up) b=4;; *) die "wheel up|down [N]";; esac
  check
  xdotool mousemove --window "$WID" 400 300 click --repeat "$n" --delay 20 "$b"
  settle; echo "wheel $dir x$n ok"; }
cmd_resize() { (($# == 2)) || die "resize W H"; check
  xdotool windowsize "$WID" "$1" "$2"; sleep 1; echo "resize $1x$2 ok"; }

cmd_shot() {
  local name=${1:-}; shift || true; local crop=""
  [[ -n $name ]] || die "shot NAME [--crop WxH+X+Y]"
  [[ ${1:-} == --crop ]] && crop=$2
  check
  local out=$DIR/$name.png
  xwd -silent -id "$WID" -out "$DIR/$name.xwd"
  convert "$DIR/$name.xwd" ${crop:+-crop "$crop" +repage} "$out"; rm -f "$DIR/$name.xwd"
  local info; info=$(convert "$out" -format '%wx%h sd=%[fx:standard_deviation]' info:)
  local sd=${info##*sd=}
  echo "$out $info$(awk -v s="$sd" 'BEGIN{if (s+0 < 0.005) print " BLANK?"}')"
}

cmd_events() {
  load; local log=$DIR/browser.log pat=${1:-}
  [[ -f $log ]] || die "no event log"
  echo "counts:"; grep -o 'SDL_EVENT_[A-Z_]*' "$log" | sort | uniq -c | sort -rn | head -15 | sed 's/^/ /'
  local keys; keys=$(grep -E 'SDL_EVENT_(KEY_DOWN|KEY_UP|TEXT_INPUT)' "$log" || true)
  if [[ -n $keys ]]; then
    local total zero
    total=$(printf '%s\n' "$keys" | wc -l)
    zero=$(printf '%s\n' "$keys" | grep -ciE 'windowid=0([^0-9]|$)' || true)
    echo "key/text events=$total windowid=0:$zero ids=$(printf '%s\n' "$keys" | grep -oiE 'windowid=[0-9]+' | sort -u | tr '\n' ' ')"
    echo "text: $(grep 'SDL_EVENT_TEXT_INPUT' "$log" | grep -oE "text='[^']*'" | sed "s/text='//;s/'$//" | tr -d '\n')"
  fi
  [[ -n $pat ]] && { grep -E -- "$pat" "$log" | tail -20 || true; }
  grep -iE 'error|badwindow' "$log" | grep -v SDL_EVENT | tail -5 || true
}

cmd_requests() {
  load; [[ -f $DIR/server.log ]] || die "no fixture server in this session"
  grep -oE '"[A-Z]+ [^"]*" [0-9]{3}' "$DIR/server.log" | sed 's/ HTTP\/[0-9.]*"/"/' || true
}

cmd_status() {
  load; local s; alive "$PID" && s=running || s="exited rc=$(get rc)"
  echo "dir=$DIR display=$DISPLAY pid=$PID ($s) wid=$WID url=$(get url) port=$(get server_port)"
}

stop_pid() { # stop_pid FILE WORD
  local p; p=$(get "$1"); [[ -n $p ]] || return 0
  if cmdline_has "$p" "$2"; then kill -TERM "$p"
    for _ in $(seq 30); do alive "$p" || break; sleep 0.1; done
    alive "$p" && kill -KILL "$p"; echo "$1 $p stopped"; fi
  rm -f "$DIR/$1"
}

cmd_stop() {
  load; local rc=""
  if alive "$PID"; then
    [[ $(readlink "/proc/$PID/exe" 2>/dev/null) == *tai-browser ]] || die "pid $PID is not tai-browser"
    # SIGTERM -> SDL_EVENT_QUIT. Not windowclose: on Xvfb without a WM it
    # destroys the window and SDL exits 1 with X BadWindow.
    kill -TERM "$PID"
    for _ in $(seq 100); do [[ -f $DIR/rc ]] && break; sleep 0.1; done
    if [[ ! -f $DIR/rc ]]; then kill -KILL "$PID" 2>/dev/null || true; sleep 0.3; echo "browser KILLed after timeout"; fi
  fi
  rc=$(get rc); echo "browser exit=${rc:-unknown}"
  stop_pid server_pid http.server
  stop_pid xvfb_pid Xvfb
  rm -f "$DIR/pid"
  [[ $rc == 0 ]]
}

case $CMD in
  start|click|key|type|wheel|resize|shot|events|requests|status|stop) "cmd_$CMD" "$@";;
  -h|--help|help) sed -n '2,9p' "$0";;
  *) die "unknown command $CMD";;
esac

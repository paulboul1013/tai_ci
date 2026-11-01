#!/usr/bin/env bash
# Start a private Xvfb and print ":N PID" once it answers; the caller stops PID.
# Usage: xvfb.sh LOG [:N]   (default: the first free number in :90-:199)
# The only script that runs Xvfb (tests/tools/check_xvfb_owner.py enforces it).
# Two callers can both see a number free and start on it; only one X server
# wins its lock file, so a number counts only once the lock holds our pid.
# WSLg mounts /tmp/.X11-unix read-only, so readiness is probed with xset.
set -euo pipefail
(($# == 1 || $# == 2)) || { echo "usage: xvfb.sh LOG [:N]" >&2; exit 2; }
log=$1 first=90 last=199
[[ -n ${2:-} ]] && { first=${2#:}; last=${2#:}; }
for ((n = first; n <= last; n++)); do
  [[ -e /tmp/.X$n-lock ]] && continue
  setsid Xvfb ":$n" -screen 0 1280x800x24 -nolisten tcp </dev/null >"$log" 2>&1 &
  pid=$!
  for _ in $(seq 100); do
    kill -0 "$pid" 2>/dev/null || break
    lock=$(tr -d ' \n' 2>/dev/null <"/tmp/.X$n-lock" || true)
    [[ -n $lock && $lock != "$pid" ]] && break
    if [[ $lock == "$pid" ]] && xset -display ":$n" q >/dev/null 2>&1; then
      echo ":$n $pid"; exit 0; fi
    sleep 0.1
  done
  kill "$pid" 2>/dev/null || true
done
echo "xvfb.sh: no free X display in :$first-:$last" >&2
exit 1

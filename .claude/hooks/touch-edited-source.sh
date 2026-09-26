#!/usr/bin/env bash
# WSL2 clock skew can leave an edited source older than its object file, so
# Ninja silently keeps the stale .o. Bump the mtime of every edited C source.
set -u
path=$(jq -r '.tool_input.file_path // empty' 2>/dev/null) || exit 0
case "$path" in
  */src/*.c|*/src/*.h|*/src/*.inc|*/include/*.h|*/tests/*.c|*/CMakeLists.txt)
    [ -f "$path" ] && touch "$path" ;;
esac
exit 0

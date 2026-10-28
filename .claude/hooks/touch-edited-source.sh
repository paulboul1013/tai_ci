#!/usr/bin/env bash
# WSL2 clock skew can leave an edited source older than its objects, so Ninja
# silently keeps them. Bump the edited file's mtime and delete every object
# built from it (tests/tools/clear_stale_objects.sh), since a clock that went
# backwards makes the touch alone insufficient.
set -u
path=$(jq -r '.tool_input.file_path // empty' 2>/dev/null) || exit 0
case "$path" in
  */src/*.c|*/src/*.h|*/src/*.inc|*/src/*.js|*/include/*.h|*/tests/*.c|\
  */tests/*.h|*/tests/reference/runtime.js|*/CMakeLists.txt)
    [ -f "$path" ] || exit 0
    touch "$path"
    "${CLAUDE_PROJECT_DIR:-.}"/tests/tools/clear_stale_objects.sh "$path" \
      >/dev/null 2>&1 ;;
esac
exit 0

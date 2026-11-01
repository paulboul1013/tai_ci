#!/usr/bin/env bash
# PreToolUse(Bash). Edits made through Bash or python skip the Edit/Write hook,
# and WSL2's clock can step backwards, so Ninja may say "no work to do" and
# tests then run stale binaries. Before any build or test command:
#   * cmake --build / ninja: delete stale objects so the build relinks;
#   * ctest without a build in the same command: also build the test
#     directory first, since ctest runs whatever executables exist.
# A failed build blocks the ctest call and reports the error.
set -u
command=$(jq -r '.tool_input.command // empty' 2>/dev/null) || exit 0
root=${CLAUDE_PROJECT_DIR:-.}
builds=0 tests=0
[[ $command =~ (^|[^[:alnum:]_])(cmake[[:space:]]+--build|ninja)([[:space:]]|$) ]] && builds=1
[[ $command =~ (^|[^[:alnum:]_])ctest([[:space:]]|$) ]] && tests=1
((builds || tests)) || exit 0

"$root"/tests/tools/clear_stale_objects.sh >/dev/null 2>&1
((tests && !builds)) || exit 0

dirs=$(grep -oE -- '--test-dir[[:space:]]+[^[:space:];&|]+' <<<"$command" |
       awk '{print $2}' | sort -u)
[[ -n $dirs ]] || dirs=build
for dir in $dirs; do
  [[ $dir = /* ]] || dir=$root/$dir
  [[ -f $dir/build.ninja ]] || continue
  if ! output=$(cmake --build "$dir" 2>&1); then
    jq -n --arg reason "fresh-build-before-test: cmake --build $dir failed before ctest:
$(tail -n 30 <<<"$output")" \
      '{hookSpecificOutput: {hookEventName: "PreToolUse",
        permissionDecision: "deny", permissionDecisionReason: $reason}}'
    exit 0
  fi
done
exit 0

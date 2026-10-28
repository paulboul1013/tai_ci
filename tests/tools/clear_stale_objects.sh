#!/usr/bin/env bash
# WSL2's clock can step backwards, so an edited file may carry an mtime older
# than the objects built from it, and Ninja then silently keeps stale objects
# (touch cannot help: "now" is itself in the past). This deletes, in every
# build*/ directory, each object whose recorded dependencies include a given
# file, plus any object dated in the future, so Ninja must rebuild and relink.
#
#   tests/tools/clear_stale_objects.sh [FILE...]
#
# Without FILE it uses every file changed since HEAD, untracked ones included.
# Embedded JavaScript (src/js_*.js, tests/reference/runtime.js) also drops the
# generated tai_js_sources.h. Run it before building after any edit.
set -u
root=$(git rev-parse --show-toplevel) || exit 1
cd "$root" || exit 1
if (($# == 0)); then
  mapfile -t files < <({ git diff --name-only HEAD
                         git ls-files --others --exclude-standard; } | sort -u)
else
  files=("$@")
fi

removed=0
drop() {
  rm -f "$1" && echo "removed $1" && removed=$((removed + 1))
}

for dir in build*/; do
  dir=${dir%/}
  [ -f "$dir/build.ninja" ] || continue
  targets=()
  for file in "${files[@]}"; do
    file=${file#"$root"/}
    case "$file" in
      src/*.js|tests/reference/runtime.js)
        generated="$dir/generated/tai_js_sources.h"
        [ -f "$generated" ] && drop "$generated"
        targets+=("$root/$generated") ;;
      src/*|include/*|tests/*.c|tests/*.h)
        targets+=("$root/$file") ;;
    esac
  done
  if ((${#targets[@]})); then
    # ninja -t deps prints "OBJECT: #deps ..." followed by indented inputs.
    while IFS= read -r object; do
      [ -f "$dir/$object" ] && drop "$dir/$object"
    done < <(ninja -C "$dir" -t deps 2>/dev/null | awk -v list="$(
               printf '%s\n' "${targets[@]}")" '
      BEGIN { n = split(list, wanted, "\n"); for (i = 1; i <= n; i++) want[wanted[i]] = 1 }
      /^[^ ]/ { object = $1; sub(/:$/, "", object); next }
      { sub(/^ +/, ""); if ($0 in want) hit[object] = 1 }
      END { for (o in hit) print o }')
  fi
  while IFS= read -r object; do
    drop "$object"
  done < <(find "$dir" -name '*.o' -type f -newermt "@$(date +%s)")
done
echo "clear_stale_objects: $removed file(s) removed"

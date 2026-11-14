#!/bin/sh
# Stand-in for WolvenKit.CLI used by the tests: mimics import / pack / uncook file outputs.
# uncook copies matching files from $FAKE_CP_SOURCE (a WolvenKit project "source" folder).
cmd="$1"; shift
case "$cmd" in
  import)  echo "import format=$GltfImportArgs__ImportFormat"; dir="$2"; find "$dir" -type f ! -name '*.glb' -exec sh -c 'echo IMPORTED >> "$1"' _ {} \; ;;
  pack)    src="$2"; out="$4"; ( cd "$src" && tar cf "$out/$(basename "$src").archive" . ) ;;
  uncook)
    while [ $# -gt 0 ]; do case "$1" in -p) p="$2"; shift;; -o) o="$2"; shift;; -or) r="$2"; shift;; -w) w="$2"; shift;; esac; shift; done
    name="${w#\*}"; echo "uncook $name from $p"
    find "$FAKE_CP_SOURCE/archive" -type f -name "$name" | while read f; do rel="${f#$FAKE_CP_SOURCE/archive/}"; mkdir -p "$o/$(dirname "$rel")"; cp "$f" "$o/$rel"; done
    find "$FAKE_CP_SOURCE/raw" -type f \( -name "$name.glb" -o -name "${name%.*}.glb" \) | while read f; do rel="${f#$FAKE_CP_SOURCE/raw/}"; mkdir -p "$r/$(dirname "$rel")"; cp "$f" "$r/$rel"; done ;;
  unbundle)
    while [ $# -gt 0 ]; do case "$1" in -p) p="$2"; shift;; -o) o="$2"; shift;; -r) rx="$2"; shift;; -w) w="$2"; shift;; esac; shift; done
    echo "unbundle regex=$rx from $p"
    find "$FAKE_CP_SOURCE/archive" -type f | while read f; do rel="${f#$FAKE_CP_SOURCE/archive/}";
      if printf '%s' "$rel" | grep -Eq "$rx"; then mkdir -p "$o/$(dirname "$rel")"; cp "$f" "$o/$rel"; fi; done ;;
  *) echo "unknown $cmd"; exit 1 ;;
esac

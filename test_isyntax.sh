#!/usr/bin/env bash
# Check vips-isyntax on a real slide: compare regions read through vips (the plugin) with the same
# regions read directly through libisyntax (isyntax-crop, built by build.sh). Pixels must be identical.
# usage: test_isyntax.sh slide.isyntax [path/to/isyntax-crop]
# Public test slide: https://zenodo.org/record/5037046/files/testslide.isyntax?download=1
set -euo pipefail
in=$1; crop=${2:-$(dirname "$0")/isyntax-crop}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
vipsheader "$in"
hdr=$(vipsheader -a "$in"); grep -E '^isyntax\.(level-count|mpp-x|tile-width|barcode|model)|^xres|^icc' <<<"$hdr" | cut -c1-90 || true
n=$(vipsheader -f isyntax.level-count "$in"); fail=0
for level in 0 $(( n > 2 ? n / 2 : 0 )) $(( n - 1 )); do
  w=$(vipsheader -f width "$in[level=$level]"); h=$(vipsheader -f height "$in[level=$level]")
  s=$(( w < 3000 ? w : 3000 )); t=$(( h < 3000 ? h : 3000 ))
  for pos in "0 0" "$(( (w - s) / 2 )) $(( (h - t) / 2 ))" "$(( w - s )) $(( h - t ))"; do   # corner, centre, far corner
    set -- $pos
    vips crop "$in[level=$level]" "$tmp/v.v" "$1" "$2" "$s" "$t"
    vips rawsave "$tmp/v.v" "$tmp/v.rgb"
    "$crop" "$in" "$level" "$1" "$2" "$s" "$t" "$tmp/r.rgb"
    if cmp -s "$tmp/v.rgb" "$tmp/r.rgb"; then r=identical; else r=DIFFERENT; fail=1; fi
    echo "level $level (${w}x${h}) region $1,$2 ${s}x${t}: $r"
  done
done
# whole-slide thumbnail for a visual check
vips thumbnail "$in" "$tmp/thumb.jpg" 1024 && cp "$tmp/thumb.jpg" "$(basename "$in" .isyntax)_thumb.jpg"
echo "thumbnail: $(basename "$in" .isyntax)_thumb.jpg"
for a in label macro; do   # associated images (a slide may lack one; that's reported, not a failure)
  if vips isyntaxload "$in" "$(basename "$in" .isyntax)_$a.png" --associated "$a" 2>/dev/null; then
    echo "$a: $(vipsheader "$(basename "$in" .isyntax)_$a.png" | cut -d: -f2)"
  else echo "$a: not present or not readable"; fi
done
(( fail == 0 )) && echo "PASS" || { echo "FAIL"; exit 1; }

#!/usr/bin/env bash
# Build vips-isyntax against the libvips found by pkg-config and install it where that libvips
# looks for plugins. Needs: gcc, cmake, curl, pkg-config, libvips-dev (headers of the SAME libvips
# that will load the plugin: the loader class layout differs between versions).
#
#   bash build.sh            # build + install (run as root, or set DESTDIR)
#   bash build.sh --no-install
#   LIBISYNTAX_COMMIT=<sha> bash build.sh
set -euo pipefail

LIBISYNTAX_COMMIT=${LIBISYNTAX_COMMIT:-92471dbf6c70a6736e53e1e45564b170b74a81c1}
here=$(cd "$(dirname "$0")" && pwd)
install=1; [[ ${1:-} == --no-install ]] && install=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

ver=$(pkg-config --modversion vips)
maj=${ver%%.*}; rest=${ver#*.}; min=${rest%%.*}
libdir=$(pkg-config --variable=libdir vips); prefix=$(pkg-config --variable=prefix vips)
echo "libvips $ver (prefix $prefix, libdir $libdir)"

# Plugin location and file name by libvips version
if (( maj > 8 || (maj == 8 && min >= 13) )); then
  dest="$libdir/vips-modules-$maj.$min"; name=vips-isyntax.so
elif (( min >= 10 )); then
  dest="$libdir/vips-plugins-$maj.$min"; name=vips-isyntax.so
else
  dest="$prefix/lib/vips-plugins-$maj.$min"; name=vips-isyntax.plg   # 8.9 and older: .plg suffix
fi

echo "== libisyntax ${LIBISYNTAX_COMMIT:0:7}"
curl -fsSL "https://codeload.github.com/amspath/libisyntax/tar.gz/$LIBISYNTAX_COMMIT" | tar -xz -C "$work"
src=$(echo "$work"/libisyntax-*)
# libisyntax prints messages with printf (stdout); send them to stderr so they can't end up
# inside an image stream when vips writes to a pipe.
sed -i 's|^#define console_print printf$|#define console_print(...) fprintf(stderr, __VA_ARGS__)|; s|if (is_verbose_mode) fprintf(stdout, __VA_ARGS__)|if (is_verbose_mode) fprintf(stderr, __VA_ARGS__)|' \
  "$src/src/platform/common.h"
grep -q 'define console_print(...) fprintf(stderr' "$src/src/platform/common.h" || { echo "patch failed" >&2; exit 1; }
cmake -S "$src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
cmake --build "$work/build" --target isyntax -j"$(nproc)" >/dev/null

echo "== vips-isyntax"
gcc -O2 -Wall -Wno-unused-parameter -fPIC -shared -o "$here/$name" "$here/vips-isyntax.c" \
  -I"$src/src" $(pkg-config --cflags vips gmodule-2.0) \
  "$work/build/libisyntax.a" $(pkg-config --libs vips gmodule-2.0) -lpthread -lm
echo "built $here/$name"
gcc -O2 -o "$here/isyntax-crop" "$here/isyntax-crop.c" -I"$src/src" "$work/build/libisyntax.a" -lpthread -lm
echo "built $here/isyntax-crop (reference reader for test_isyntax.sh)"

if (( install )); then
  mkdir -p "${DESTDIR:-}$dest"
  install -m 0755 "$here/$name" "${DESTDIR:-}$dest/$name"
  echo "installed ${DESTDIR:-}$dest/$name"
  if [[ -z ${DESTDIR:-} ]] && command -v vips >/dev/null; then
    ops=$(vips -l)   # no "vips -l | grep -q": grep exiting early fails the pipe under pipefail
    grep -q isyntaxload <<<"$ops" && echo "OK: vips lists isyntaxload" \
      || { echo "WARNING: vips doesn't list isyntaxload; is 'vips' the same libvips $ver?" >&2; exit 1; }
  fi
fi

#!/bin/sh
# Build-time check: is the vips-isyntax plugin registered for the vips CLI and for pyvips?
# Prints enough detail to diagnose a failure from the build log alone.
echo "== vips: $(vips --version)"
echo "== plugin files:"
find /usr/lib -name 'vips-isyntax*' -exec ls -l {} \; 2>/dev/null
echo "== libvips module/plugin loading:"
VIPS_INFO=1 vips --version 2>&1 | grep -iE "search|load|plugin|module|unable|error" | sed 's/^/   /'
ok=1
if vips -l > /tmp/vips-ops.txt 2>/tmp/vips-err.txt && grep -q isyntaxload /tmp/vips-ops.txt; then
  echo "== vips CLI: isyntaxload registered"
else
  echo "== vips CLI: isyntaxload NOT registered"; sed 's/^/   /' /tmp/vips-err.txt; ok=0
fi
if python3 - <<'P'
import sys
try:
    import pyvips
except Exception as e:
    print("== pyvips: import failed:", repr(e)); sys.exit(1)
print("== pyvips", getattr(pyvips, "__version__", "?"), "on libvips %d.%d" % (pyvips.version(0), pyvips.version(1)),
      "| API mode" if getattr(pyvips, "API_mode", False) else "| ABI mode")
found = pyvips.type_find("VipsOperation", "isyntaxload")
print("== pyvips: isyntaxload", "registered" if found else "NOT registered")
sys.exit(0 if found else 1)
P
then :; else ok=0; fi
rm -f /tmp/vips-ops.txt /tmp/vips-err.txt
[ "$ok" = 1 ] && echo "== OK" || { echo "== FAILED: see above"; exit 1; }

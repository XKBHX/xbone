#!/usr/bin/env bash
#
# Build xemu for wasm32 with a local emsdk.
#
# Usage:
#   source <emsdk>/emsdk_env.sh
#   wasm/build-deps.sh          # once
#   wasm/build.sh [configure args...]
#
# Output is copied next to wasm/index.html (build/wasm/dist).

set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$src_dir/build/wasm"
T="$out/deps/target"

command -v emcc >/dev/null || {
    echo "emcc not found; source emsdk_env.sh first" >&2
    exit 1
}
[ -d "$T/lib/pkgconfig" ] || {
    echo "dependencies missing; run wasm/build-deps.sh first" >&2
    exit 1
}

export EM_CACHE="${EM_CACHE:-$out/emcache}"
export PKG_CONFIG_PATH="$T/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CPATH="$T/include"

# emsdk >= 4 only predefines __EMSCRIPTEN__; the tree still tests EMSCRIPTEN.
# glib's gsize is unsigned int but size_t is unsigned long on wasm32. Both
# are 32 bits, so the pointer mismatch is harmless.
cflags="-DXBOX=1 -DEMSCRIPTEN=1 -Wno-error=incompatible-pointer-types -I$T/include"

mkdir -p "$out/xemu"
cd "$out/xemu"

if [ ! -f build.ninja ] || [ $# -gt 0 ]; then
    emconfigure "$src_dir/configure" \
        --disable-docs \
        --static \
        --disable-tools \
        --disable-werror \
        --target-list=i386-softmmu \
        --extra-cflags="$cflags" \
        --extra-ldflags="-L$T/lib" \
        "$@"

    # configs/meson/emscripten.txt is loaded after configure's cross file and
    # replaces its c_args/link_args, dropping --extra-cflags. Append them back.
    merge=()
    while IFS= read -r arg; do merge+=("$arg"); done < <(
        pyvenv/bin/meson introspect --buildoptions . |
        CFLAGS_EXTRA="$cflags" LDFLAGS_EXTRA="-L$T/lib" pyvenv/bin/python3 -c '
import json, os, sys
opts = {o["name"]: o["value"] for o in json.load(sys.stdin)}
cflags = os.environ["CFLAGS_EXTRA"].split()
ldflags = os.environ["LDFLAGS_EXTRA"].split()
for name, extra in (("c_args", cflags), ("cpp_args", cflags),
                    ("objc_args", cflags), ("c_link_args", ldflags),
                    ("cpp_link_args", ldflags)):
    if name in opts:
        value = opts[name] + [f for f in extra if f not in opts[name]]
        print("-D%s=%s" % (name, json.dumps(value)))
')
    pyvenv/bin/meson configure . "${merge[@]}"
fi

pyvenv/bin/python3 -c "import yaml" 2>/dev/null ||
    pyvenv/bin/python3 -m pip install -q pyyaml

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ninja -j"$jobs" qemu-system-i386.js

mkdir -p "$out/dist"
cp qemu-system-i386.js qemu-system-i386.wasm "$out/dist/"
[ -f qemu-system-i386.worker.js ] && cp qemu-system-i386.worker.js "$out/dist/"
cp "$src_dir/wasm/index.html" "$out/dist/"
echo "Built $out/dist"

#!/usr/bin/env bash
#
# Cross-compile xemu's third-party dependencies for wasm32 with a local
# emsdk. Based on tests/docker/dockerfiles/emsdk-wasm32-cross.docker, plus
# the libraries xemu adds on top of QEMU (libsamplerate, libpcap, libepoxy).
#
# Usage: source <emsdk>/emsdk_env.sh && wasm/build-deps.sh [workdir]
# Output: <workdir>/target (default build/wasm/deps/target)

set -euo pipefail

ZLIB_VERSION=1.3.1
GLIB_MINOR_VERSION=2.84
GLIB_VERSION=${GLIB_MINOR_VERSION}.0
PIXMAN_VERSION=0.44.2
FFI_VERSION=v3.4.7
SAMPLERATE_VERSION=0.2.2
PCAP_VERSION=1.10.5
EPOXY_VERSION=1.5.10
MESON_VERSION=1.5.0

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${1:-$src_dir/build/wasm/deps}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"

export TARGET="$work/target"
export CPATH="$TARGET/include"
export PKG_CONFIG_PATH="$TARGET/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CFLAGS="-O3 -pthread -DWASM_BIGINT"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-sWASM_BIGINT -sASYNCIFY=1 -L$TARGET/lib"
export EM_CACHE="${EM_CACHE:-$src_dir/build/wasm/emcache}"

command -v emcc >/dev/null || {
    echo "emcc not found; source emsdk_env.sh first" >&2
    exit 1
}

nproc=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
mkdir -p "$TARGET/lib" "$TARGET/include" "$work/src" "$work/stamp"

if [ ! -x "$work/venv/bin/meson" ]; then
    python3 -m venv "$work/venv"
    "$work/venv/bin/pip" install -q "meson==$MESON_VERSION" tomli packaging
fi
meson="$work/venv/bin/meson"

list_args() {
    printf "'%s', " "$@" | sed 's/, $//'
}

# shellcheck disable=SC2086
cat > "$work/cross.meson" <<EOF
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
ranlib = 'emranlib'
pkg-config = ['pkg-config', '--static']

[built-in options]
c_args = [$(list_args $CFLAGS -Wno-incompatible-function-pointer-types -Wno-error=incompatible-pointer-types)]
cpp_args = [$(list_args $CXXFLAGS)]
c_link_args = [$(list_args $LDFLAGS)]
cpp_link_args = [$(list_args $LDFLAGS)]
EOF

fetch() { # url dir
    local url=$1 dir=$2
    if [ ! -d "$dir" ]; then
        mkdir -p "$dir.tmp"
        curl -fLs "$url" | tar xJC "$dir.tmp" --strip-components=1 ||
            curl -fLs "$url" | tar xzC "$dir.tmp" --strip-components=1
        mv "$dir.tmp" "$dir"
    fi
}

step() { # name
    if [ -e "$work/stamp/$1" ]; then
        echo "== $1: already built"
        return 1
    fi
    echo "== $1"
}

done_step() { touch "$work/stamp/$1"; }

if step zlib; then
    fetch "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.xz" \
        "$work/src/zlib"
    (cd "$work/src/zlib" &&
        emconfigure ./configure --prefix="$TARGET" --static &&
        emmake make install AR=emar ARFLAGS=rcs -j"$nproc")
    done_step zlib
fi

if step libffi; then
    [ -d "$work/src/libffi" ] ||
        git clone -q --depth 1 -b "$FFI_VERSION" https://github.com/libffi/libffi "$work/src/libffi"
    (cd "$work/src/libffi" &&
        autoreconf -fi &&
        emconfigure ./configure --host=wasm32-unknown-linux \
            --prefix="$TARGET" --enable-static \
            --disable-shared --disable-dependency-tracking \
            --disable-builddir --disable-multi-os-directory \
            --disable-raw-api --disable-docs &&
        emmake make install SUBDIRS='include' -j"$nproc")
    done_step libffi
fi

if step pixman; then
    [ -d "$work/src/pixman" ] ||
        git clone -q --depth 1 -b "pixman-$PIXMAN_VERSION" \
            https://gitlab.freedesktop.org/pixman/pixman "$work/src/pixman"
    (cd "$work/src/pixman" &&
        "$meson" setup _build --prefix="$TARGET" --cross-file="$work/cross.meson" \
            --default-library=static --buildtype=release \
            -Dtests=disabled -Ddemos=disabled &&
        "$meson" install -C _build)
    done_step pixman
fi

if step glib; then
    mkdir -p "$work/src/stub"
    cat > "$work/src/stub/res_query.c" <<'EOF'
#include <netdb.h>
int res_query(const char *name, int class,
              int type, unsigned char *dest, int len)
{
    h_errno = HOST_NOT_FOUND;
    return -1;
}
EOF
    emcc $CFLAGS -c "$work/src/stub/res_query.c" -fPIC -o "$work/src/stub/libresolv.o"
    emar rcs "$TARGET/lib/libresolv.a" "$work/src/stub/libresolv.o"

    fetch "https://download.gnome.org/sources/glib/$GLIB_MINOR_VERSION/glib-$GLIB_VERSION.tar.xz" \
        "$work/src/glib"
    (cd "$work/src/glib" &&
        rm -rf _build &&
        "$meson" setup _build --prefix="$TARGET" --cross-file="$work/cross.meson" \
            --default-library=static --buildtype=release --force-fallback-for=pcre2 \
            -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
            -Dtests=false -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=false &&
        sed -i.bak -E "/#define HAVE_POSIX_SPAWN 1/d" ./_build/config.h &&
        sed -i.bak -E "/#define HAVE_PTHREAD_GETNAME_NP 1/d" ./_build/config.h &&
        "$meson" install -C _build)
    done_step glib
fi

if step libsamplerate; then
    fetch "https://github.com/libsndfile/libsamplerate/releases/download/$SAMPLERATE_VERSION/libsamplerate-$SAMPLERATE_VERSION.tar.xz" \
        "$work/src/libsamplerate"
    (cd "$work/src/libsamplerate" &&
        emconfigure ./configure --host=wasm32-unknown-linux --prefix="$TARGET" \
            --enable-static --disable-shared --disable-sndfile --disable-alsa \
            --disable-fftw &&
        emmake make install -j"$nproc")
    done_step libsamplerate
fi

if step libpcap; then
    fetch "https://www.tcpdump.org/release/libpcap-$PCAP_VERSION.tar.xz" \
        "$work/src/libpcap"
    (cd "$work/src/libpcap" &&
        emconfigure ./configure --host=wasm32-unknown-linux --prefix="$TARGET" \
            --with-pcap=null --disable-shared --disable-usb --disable-netmap \
            --disable-bluetooth --disable-dbus --disable-rdma --without-libnl \
            --disable-remote &&
        emmake make install-static install-headers install-pc -j"$nproc" ||
        emmake make install -j"$nproc")
    done_step libpcap
fi

if step libepoxy; then
    fetch "https://github.com/anholt/libepoxy/archive/refs/tags/$EPOXY_VERSION.tar.gz" \
        "$work/src/libepoxy"
    (cd "$work/src/libepoxy" &&
        rm -rf _build &&
        "$meson" setup _build --prefix="$TARGET" --cross-file="$work/cross.meson" \
            --default-library=static --buildtype=release \
            -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false &&
        "$meson" install -C _build)
    done_step libepoxy
fi

if step sdl3; then
    # Same revision and options as subprojects/sdl3.wrap and meson.build.
    # meson's CMake subproject support can't load Emscripten's toolchain file.
    sdl_src="$src_dir/subprojects/sdl3"
    [ -f "$sdl_src/CMakeLists.txt" ] ||
        "$meson" subprojects download --sourcedir "$src_dir" sdl3
    rm -rf "$work/src/sdl3-build"
    emcmake cmake -S "$sdl_src" -B "$work/src/sdl3-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TARGET" \
        -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
        -DSDL_PTHREADS=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON \
        -DSDL_CAMERA=OFF -DSDL_DISKAUDIO=OFF -DSDL_DUMMYVIDEO=OFF \
        -DSDL_FRIBIDI=OFF -DSDL_GPU=OFF -DSDL_HAPTIC=OFF -DSDL_LIBTHAI=OFF \
        -DSDL_POWER=OFF -DSDL_RENDER=OFF -DSDL_SENSOR=OFF \
        -DSDL_TEST_LIBRARY=OFF -DSDL_TRAY=OFF -DSDL_VIRTUAL_JOYSTICK=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_TESTS=OFF
    cmake --build "$work/src/sdl3-build"
    cmake --install "$work/src/sdl3-build"
    done_step sdl3
fi

echo "Dependencies installed to $TARGET"

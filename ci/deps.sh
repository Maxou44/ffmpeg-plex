#!/usr/bin/env bash
# Builds the third-party libraries used by the Plex configuration as static
# libraries into $PREFIX. Sourced by build.sh, which sets up the toolchain
# variables (TARGET, PREFIX, HOST, CROSS, CC, CXX, AR, RANLIB, NM, CFLAGS...).
#
# Plex builds the same set of libraries with conan (see `Plex Transcoder
# -buildconf`); the versions below are current releases of those libraries.

set -euo pipefail

DEPS_VERSION=1   # bump to invalidate the CI cache

ZLIB_VER=1.3.1
FREETYPE_VER=2.13.3
FRIBIDI_VER=1.0.16
HARFBUZZ_VER=10.1.0
LIBASS_VER=0.17.3
OGG_VER=1.3.5
VORBIS_VER=1.3.7
OPUS_VER=1.5.2
DAV1D_VER=1.5.0
LIBXML2_VER=2.13.5
OPENSSL_VER=3.0.15
LAME_VER=3.100
X264_REF=stable
OPENCL_VER=v2024.10.24
LIBVPL_VER=2.14.0
LIBDRM_VER=2.4.124
LIBVA_VER=2.22.0
NVCODEC_VER=13.0.19.0

SRC_DIR=${SRC_DIR:-$PWD/.deps-src}
BLD_DIR=${BLD_DIR:-$PWD/.deps-build/$TARGET}
mkdir -p "$SRC_DIR" "$BLD_DIR" "$PREFIX/lib/pkgconfig" "$PREFIX/include"

log() { echo "::group::$*" >&2 2>/dev/null || true; echo "=== $*" >&2; }
endlog() { echo "::endgroup::" >&2 || true; }

# fetch NAME URL -> extracts into $BLD_DIR/NAME and cd into it
fetch() {
    local name=$1 url=$2 file
    file="$SRC_DIR/$name-$(basename "$url")"
    if [ ! -f "$file" ]; then
        curl -fsSL --retry 5 -o "$file.tmp" "$url"
        mv "$file.tmp" "$file"
    fi
    rm -rf "$BLD_DIR/$name"; mkdir -p "$BLD_DIR/$name"
    tar -xf "$file" -C "$BLD_DIR/$name" --strip-components=1
    cd "$BLD_DIR/$name"
}

host_args() { [ -n "${CROSS:-}" ] && echo "--host=$HOST" || true; }

autotools() {
    ./configure $(host_args) --prefix="$PREFIX" --libdir="$PREFIX/lib" \
        --enable-static --disable-shared --with-pic "$@"
    make -j"$JOBS"
    make install
}

meson_build() {
    local cross=()
    [ -n "${CROSS:-}" ] && cross=(--cross-file "$MESON_CROSS")
    meson setup build ${cross[@]+"${cross[@]}"} --prefix="$PREFIX" --libdir=lib \
        --buildtype=release --default-library=static -Db_staticpic=true \
        -Dc_args="$CFLAGS" -Dcpp_args="$CXXFLAGS" "$@"
    ninja -C build -j"$JOBS"
    ninja -C build install
}

cmake_build() {
    local cross=()
    [ -n "${CROSS:-}" ] && cross=(-DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN")
    cmake -S . -B build -G Ninja ${cross[@]+"${cross[@]}"} -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" "$@"
    cmake --build build -j"$JOBS"
    cmake --install build
}

# --------------------------------------------------------------------------
build_zlib() {
    fetch zlib https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz
    if [ "$OS" = windows ]; then
        make -f win32/Makefile.gcc -j"$JOBS" libz.a CC="$CC" AR="$AR" RC="$WINDRES" CFLAGS="$CFLAGS"
        cp zlib.h zconf.h "$PREFIX/include/"; cp libz.a "$PREFIX/lib/"
        cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library
Version: $ZLIB_VER
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
    else
        CFLAGS="$CFLAGS" ./configure --static --prefix="$PREFIX" --libdir="$PREFIX/lib"
        make -j"$JOBS" && make install
    fi
}

build_freetype() {
    fetch freetype https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VER.tar.xz
    autotools --with-zlib=yes --without-png --without-bzip2 --without-brotli \
        --without-harfbuzz --enable-freetype-config=no
}

build_fribidi() {
    fetch fribidi https://github.com/fribidi/fribidi/releases/download/v$FRIBIDI_VER/fribidi-$FRIBIDI_VER.tar.xz
    meson_build -Ddocs=false -Dbin=false -Dtests=false
}

build_harfbuzz() {
    fetch harfbuzz https://github.com/harfbuzz/harfbuzz/releases/download/$HARFBUZZ_VER/harfbuzz-$HARFBUZZ_VER.tar.xz
    meson_build -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled \
        -Dcairo=disabled -Dicu=disabled -Dchafa=disabled -Dtests=disabled \
        -Ddocs=disabled -Dutilities=disabled -Dintrospection=disabled
}

build_libass() {
    fetch libass https://github.com/libass/libass/releases/download/$LIBASS_VER/libass-$LIBASS_VER.tar.xz
    autotools --disable-fontconfig --disable-require-system-font-provider \
        --disable-libunibreak --disable-test --disable-compare --disable-profile \
        --disable-fuzz
}

build_ogg() {
    fetch libogg https://github.com/xiph/ogg/releases/download/v$OGG_VER/libogg-$OGG_VER.tar.xz
    autotools
}

build_vorbis() {
    fetch libvorbis https://github.com/xiph/vorbis/releases/download/v$VORBIS_VER/libvorbis-$VORBIS_VER.tar.xz
    # configure passes -force_cpusubtype_ALL on Darwin, which the current
    # Apple linker no longer accepts
    sed -i.bak 's/-force_cpusubtype_ALL//g' configure
    autotools --disable-docs --disable-examples --disable-oggtest
}

build_opus() {
    fetch opus https://github.com/xiph/opus/releases/download/v$OPUS_VER/opus-$OPUS_VER.tar.gz
    autotools --disable-doc --disable-extra-programs
}

build_dav1d() {
    fetch dav1d https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VER/dav1d-$DAV1D_VER.tar.xz
    meson_build -Denable_tools=false -Denable_tests=false -Denable_examples=false
}

build_libxml2() {
    fetch libxml2 https://download.gnome.org/sources/libxml2/${LIBXML2_VER%.*}/libxml2-$LIBXML2_VER.tar.xz
    autotools --without-python --without-lzma --without-zlib --without-iconv \
        --without-icu --without-http --without-ftp --without-debug
}

build_openssl() {
    fetch openssl https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz
    local t
    case $TARGET in
        linux-x86_64)   t=linux-x86_64 ;;
        linux-arm64)    t=linux-aarch64 ;;
        macos-x86_64)   t=darwin64-x86_64-cc ;;
        macos-arm64)    t=darwin64-arm64-cc ;;
        windows-x86_64) t=mingw64 ;;
    esac
    ./Configure "$t" no-shared no-tests --prefix="$PREFIX" \
        --libdir=lib $CFLAGS
    make -j"$JOBS" build_libs
    make install_dev
}

build_lame() {
    fetch lame https://downloads.sourceforge.net/project/lame/lame/$LAME_VER/lame-$LAME_VER.tar.gz
    # lame 3.100 exports a symbol that no longer exists
    sed -i.bak '/lame_init_old/d' include/libmp3lame.sym
    local nasm=--disable-nasm
    case $TARGET in *x86_64) nasm=--enable-nasm ;; esac
    autotools --disable-frontend --disable-gtktest --disable-decoder $nasm
}

build_x264() {
    fetch x264 https://code.videolan.org/videolan/x264/-/archive/$X264_REF/x264-$X264_REF.tar.bz2
    local cross=()
    [ -n "${CROSS:-}" ] && cross=(--host="$HOST" --cross-prefix="$HOST-")
    ./configure ${cross[@]+"${cross[@]}"} --prefix="$PREFIX" --libdir="$PREFIX/lib" \
        --enable-static --enable-pic --disable-cli --extra-cflags="$CFLAGS"
    make -j"$JOBS" && make install-lib-static
}

build_opencl() {
    fetch opencl-headers https://github.com/KhronosGroup/OpenCL-Headers/archive/refs/tags/$OPENCL_VER.tar.gz
    cmake_build -DBUILD_TESTING=OFF
    fetch opencl-icd-loader https://github.com/KhronosGroup/OpenCL-ICD-Loader/archive/refs/tags/$OPENCL_VER.tar.gz
    # static ICD loader: the vendor drivers are still found and loaded at
    # runtime, like Plex's "opencl-runtime-loader"
    cmake_build -DBUILD_TESTING=OFF -DOPENCL_ICD_LOADER_HEADERS_DIR="$PREFIX/include" \
        -DENABLE_OPENCL_LAYERS=OFF
    local libs="-ldl -lpthread"
    if [ "$OS" = windows ]; then
        libs="-lcfgmgr32 -lole32 -ladvapi32 -lshlwapi"
        # the loader is named OpenCL.a with mingw
        [ -f "$PREFIX/lib/OpenCL.a" ] && cp "$PREFIX/lib/OpenCL.a" "$PREFIX/lib/libOpenCL.a"
    fi
    cat > "$PREFIX/lib/pkgconfig/OpenCL.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: OpenCL
Description: Khronos OpenCL ICD Loader (static)
Version: 3.0
Libs: -L\${libdir} -lOpenCL
Libs.private: $libs
Cflags: -I\${includedir} -DCL_TARGET_OPENCL_VERSION=300
EOF
}

build_libvpl() {
    fetch libvpl https://github.com/intel/libvpl/archive/refs/tags/v$LIBVPL_VER.tar.gz
    cmake_build -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF \
        -DINSTALL_EXAMPLES=OFF -DINSTALL_DEV=ON
}

build_libdrm() {
    fetch libdrm https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz
    meson_build -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
        -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled \
        -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled \
        -Dtegra=disabled -Dcairo-tests=disabled -Dman-pages=disabled \
        -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false
}

build_libva() {
    fetch libva https://github.com/intel/libva/releases/download/$LIBVA_VER/libva-$LIBVA_VER.tar.bz2
    # libva hardcodes shared libraries: build them static so that the binary
    # does not need libva at runtime. The VAAPI drivers are still loaded at
    # runtime from the usual distribution locations.
    sed -i.bak 's/shared_library(/library(/' va/meson.build
    meson_build -Dwith_x11=no -Dwith_glx=no -Dwith_wayland=no -Dwith_win32=no \
        -Denable_docs=false \
        -Ddriverdir=/usr/lib/x86_64-linux-gnu/dri:/usr/lib64/dri:/usr/lib/dri:/usr/local/lib/dri
}

build_nvcodec() {
    fetch nv-codec-headers https://github.com/FFmpeg/nv-codec-headers/releases/download/n$NVCODEC_VER/nv-codec-headers-$NVCODEC_VER.tar.gz
    make PREFIX="$PREFIX" install
}

build_omx_headers() {
    # OpenMAX IL headers; the OMX library itself is loaded at runtime
    local d="$PREFIX/include" b=https://raw.githubusercontent.com/raspberrypi/userland/master/interface/vmcs_host/khronos/IL
    for h in OMX_Audio OMX_Broadcom OMX_Component OMX_Core OMX_ILCS \
             OMX_IVCommon OMX_Image OMX_Index OMX_Other OMX_Types OMX_Video; do
        curl -fsSL --retry 5 -o "$d/$h.h" "$b/$h.h"
    done
}

build_all_deps() {
    local list="zlib freetype fribidi harfbuzz libass ogg vorbis opus dav1d libxml2 openssl lame x264"
    case $TARGET in
        linux-x86_64)   list="$list nvcodec opencl libdrm libva" ;;
        linux-arm64)    list="$list omx_headers" ;;
        windows-x86_64) list="$list nvcodec opencl libvpl" ;;
        macos-*)        ;; # OpenCL, VideoToolbox... are system frameworks
    esac
    for d in $list; do
        # per-dependency stamp: an interrupted build resumes where it stopped
        [ -f "$PREFIX/.dep-$d" ] && continue
        log "dependency: $d"
        # no "|| ..." here: it would disable set -e inside the build function
        trap 'echo "FAILED: $d" >&2' ERR
        ( set -e; build_$d )
        trap - ERR
        touch "$PREFIX/.dep-$d"
        endlog
    done
}

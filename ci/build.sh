#!/usr/bin/env bash
# Builds a static ffmpeg with the Plex Transcoder configuration.
#
#   ci/build.sh <target>
#
# targets: linux-x86_64, linux-arm64, windows-x86_64 (cross-compiled from
# Linux with llvm-mingw), macos-x86_64, macos-arm64
#
# Environment:
#   JOBS             parallel jobs (default: number of CPUs)
#   LLVM_MINGW       llvm-mingw root for windows-x86_64 (default /opt/llvm-mingw)
#   LLVM_VER         suffix of the clang/lld tools on Linux (e.g. 15 -> clang-15)
#   FFMPEG_LTO=0     disable ThinLTO (Plex builds with -flto=thin)
#   DEPS_ONLY=1      only build the dependencies
#
# Output: dist/<target>/ffmpeg[.exe] (stripped), dist/<target>/debug/ (with
# debug info, like Plex's unstripped binary), dist/<target>/buildconf.txt

set -euo pipefail

TARGET=${1:?usage: $0 <target>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CI=$ROOT/ci
cd "$ROOT"

case $(uname -s) in
    Darwin) NCPU=$(sysctl -n hw.ncpu) ;;
    *)      NCPU=$(nproc) ;;
esac
export JOBS=${JOBS:-$NCPU}
export TARGET
export PREFIX=$ROOT/.deps/$TARGET
export OS=${TARGET%%-*}
ARCH=${TARGET#*-}
LTO=${FFMPEG_LTO:-1}

# --------------------------------------------------------------------------
# toolchain
CROSS=
DEPLOY=
case $TARGET in
    linux-x86_64|linux-arm64)
        s=${LLVM_VER:+-$LLVM_VER}
        export CC=clang$s CXX=clang++$s AR=llvm-ar$s RANLIB=llvm-ranlib$s NM=llvm-nm$s STRIP=llvm-strip$s
        NVCC=clang$s
        BASE_CFLAGS="-O3 -fdata-sections -ffunction-sections -fPIC"
        ;;
    windows-x86_64)
        export HOST=x86_64-w64-mingw32
        LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
        export PATH=$LLVM_MINGW/bin:$PATH
        export CC=$HOST-clang CXX=$HOST-clang++ AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm
        export STRIP=llvm-strip WINDRES=$HOST-windres
        # llvm-mingw has no NVPTX backend: compile the CUDA kernels with the
        # host clang, like Plex does
        s=${LLVM_VER:+-$LLVM_VER}
        NVCC=$(command -v clang$s || true)
        CROSS=1
        BASE_CFLAGS="-O3 -fdata-sections -ffunction-sections"
        ;;
    macos-x86_64|macos-arm64)
        # same minimum versions as Plex: 10.13 (x86_64), 11.0 (arm64)
        [ "$ARCH" = arm64 ] && mmin=11.0 || mmin=10.13
        export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-$mmin}
        march=$ARCH; [ "$ARCH" = arm64 ] || march=x86_64
        export CC="clang -arch $march" CXX="clang++ -arch $march" AR=ar RANLIB=ranlib NM=nm STRIP=strip
        BASE_CFLAGS="-O3 -fdata-sections -ffunction-sections -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
        DEPLOY=-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET
        # cross-compiling x86_64 on an arm64 runner (or the opposite)
        if [ "$(uname -m)" != "$march" ]; then
            CROSS=1
            export HOST=$([ "$march" = arm64 ] && echo aarch64 || echo x86_64)-apple-darwin
        fi
        ;;
    *) echo "unknown target $TARGET" >&2; exit 1 ;;
esac
export CROSS
export CFLAGS=$BASE_CFLAGS CXXFLAGS=$BASE_CFLAGS
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
unset PKG_CONFIG_PATH
export PATH=$PREFIX/bin:$PATH

# cross files for meson and cmake
if [ -n "$CROSS" ]; then
    mkdir -p "$ROOT/.deps"
    export MESON_CROSS=$ROOT/.deps/$TARGET.meson
    export CMAKE_TOOLCHAIN=$ROOT/.deps/$TARGET.cmake
    case $OS in
        windows) msys=windows; mcpu=x86_64; csys=Windows ;;
        macos)   msys=darwin; mcpu=$([ "$ARCH" = arm64 ] && echo aarch64 || echo x86_64); csys=Darwin ;;
    esac
    cc0=${CC%% *}; cxx0=${CXX%% *}
    cat > "$MESON_CROSS" <<EOF
[binaries]
c = [$(printf "'%s'," $CC | sed 's/,$//')]
cpp = [$(printf "'%s'," $CXX | sed 's/,$//')]
ar = '$AR'
strip = '$STRIP'
${WINDRES:+windres = '$WINDRES'}
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
[host_machine]
system = '$msys'
cpu_family = '$mcpu'
cpu = '$mcpu'
endian = 'little'
EOF
    cat > "$CMAKE_TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME $csys)
set(CMAKE_SYSTEM_PROCESSOR $mcpu)
set(CMAKE_C_COMPILER $cc0)
set(CMAKE_CXX_COMPILER $cxx0)
${WINDRES:+set(CMAKE_RC_COMPILER $WINDRES)}
set(CMAKE_FIND_ROOT_PATH $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    [ "$OS" = macos ] && echo "set(CMAKE_OSX_ARCHITECTURES $march)" >> "$CMAKE_TOOLCHAIN"
fi

# --------------------------------------------------------------------------
# dependencies (cached by CI on the hash of deps.sh)
DEPS_STAMP=$PREFIX/.done-$(cat "$CI/deps.sh" "$CI/build.sh" | (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-16)
if [ ! -f "$DEPS_STAMP" ]; then
    source "$CI/deps.sh"
    build_all_deps
    touch "$DEPS_STAMP"
fi
cd "$ROOT"
[ -n "${DEPS_ONLY:-}" ] && exit 0

# --------------------------------------------------------------------------
# configure options: Plex configuration + toolchain
read_args() { grep -v '^[[:space:]]*#' "$1" | sed '/^[[:space:]]*$/d'; }

ARGS=()
while IFS= read -r a; do ARGS+=("$a"); done < <(
    read_args "$CI/config/common.txt"
    read_args "$CI/config/codecs.txt"
    case $OS in
        macos) read_args "$CI/config/macos.txt" ;;
        *)     read_args "$CI/config/$TARGET.txt" ;;
    esac)

# FFmpeg 8.x warns that the OpenMAX encoders are deprecated, which
# --fatal-warnings turns into an error: keep h264_omx like Plex, drop the flag
if [ "$TARGET" = linux-arm64 ]; then
    for i in "${!ARGS[@]}"; do [ "${ARGS[$i]}" = --fatal-warnings ] && unset 'ARGS[i]'; done
    ARGS=("${ARGS[@]}")
fi

# compiler flags from the Plex configuration
PCFLAGS="-O3 -fdata-sections -ffunction-sections -fno-omit-frame-pointer -g2 -gdwarf-4 -fcommon"
PLDFLAGS="-g2"
if [ "$LTO" = 1 ]; then
    PCFLAGS="$PCFLAGS -flto=thin -fwhole-program-vtables"
    PLDFLAGS="$PLDFLAGS -flto=thin -fwhole-program-vtables"
fi
PCFLAGS="$PCFLAGS -DLIBXML_STATIC -DFRIBIDI_LIB_STATIC -DNDEBUG -I$PREFIX/include"
PLDFLAGS="$PLDFLAGS -L$PREFIX/lib"
[ "$ARCH" = x86_64 ] && PCFLAGS="-m64 $PCFLAGS" && PLDFLAGS="-m64 $PLDFLAGS"

EXE=
case $OS in
    linux)
        PLDFLAGS="$PLDFLAGS -fuse-ld=lld${LLVM_VER:+-$LLVM_VER} -Wl,--gc-sections -Wl,-z,noexecstack -Wl,-z,relro"
        PLDFLAGS="$PLDFLAGS -Wl,--build-id=sha1 -Wl,--icf=all -Wl,-O2 -static-libgcc"
        # harfbuzz is C++: link the C++ runtime statically, glibc stays dynamic
        # so that the hardware drivers (CUDA, VAAPI, OpenCL ICDs) can be dlopen()ed
        ARGS+=(--cc="$CC" --cxx="$CXX" --ar="$AR" --nm="$NM" --ranlib="$RANLIB"
               --extra-libs="-l:libstdc++.a -lpthread -ldl -lm")
        [ "$TARGET" = linux-x86_64 ] && ARGS+=(--nvcc="$NVCC")
        ;;
    windows)
        PCFLAGS="$PCFLAGS -gcodeview"
        PLDFLAGS="$PLDFLAGS -Wl,--gc-sections -Wl,--pdb= -Wl,-O2 -static"
        ARGS+=(--cc="$CC" --cxx="$CXX" --ar="$AR" --nm="$NM" --ranlib="$RANLIB"
               --windres="$WINDRES" --cross-prefix=llvm- --nvcc="$NVCC"
               --extra-libs="-lc++")
        EXE=.exe
        ;;
    macos)
        PCFLAGS="$PCFLAGS $DEPLOY -Werror=unguarded-availability"
        PLDFLAGS="$PLDFLAGS $DEPLOY -Wl,-dead_strip -framework CoreFoundation -framework CoreGraphics -framework CoreText"
        ARGS+=(--arch="$([ "$ARCH" = arm64 ] && echo aarch64 || echo x86_64)"
               --cc="${CC%% *}" --cxx="${CXX%% *}" --extra-libs="-lc++ -liconv")
        [ "$ARCH" = arm64 ] && PCFLAGS="-arch arm64 $PCFLAGS" && PLDFLAGS="-arch arm64 $PLDFLAGS"
        [ "$ARCH" = x86_64 ] && PCFLAGS="-arch x86_64 $PCFLAGS" && PLDFLAGS="-arch x86_64 $PLDFLAGS"
        [ -n "$CROSS" ] && ARGS+=(--enable-cross-compile)
        ;;
esac
ARGS+=(--prefix="$ROOT/dist/$TARGET/install" --pkg-config=pkg-config
       --extra-cflags="$PCFLAGS" --extra-ldflags="$PLDFLAGS"
       --extra-version=plex)

BUILD=$ROOT/.build/$TARGET
mkdir -p "$BUILD"
cd "$BUILD"
printf '%q ' "$ROOT/configure" "${ARGS[@]}" > configure-cmdline.txt; echo >> configure-cmdline.txt
if ! "$ROOT/configure" "${ARGS[@]}"; then
    tail -60 ffbuild/config.log
    exit 1
fi
if [ "$OS" = windows ] && [ "$LTO" = 1 ]; then
    # the inline asm jump tables of the x86 MLP DSP are dropped by ThinLTO
    # on COFF targets: build that file without LTO
    echo 'libavcodec/x86/mlpdsp_init.o: CFLAGS += -fno-lto -fno-whole-program-vtables' >> ffbuild/config.mak
fi
make -j"$JOBS"

OUT=$ROOT/dist/$TARGET
rm -rf "$OUT"; mkdir -p "$OUT/debug"
cp "ffmpeg_g$EXE" "$OUT/debug/ffmpeg$EXE"
[ -f ffmpeg.pdb ] && cp ffmpeg.pdb "$OUT/debug/" || true
# --strip=true (Plex): make does not produce a stripped ffmpeg, strip ffmpeg_g
cp "ffmpeg_g$EXE" "$OUT/ffmpeg$EXE"
$STRIP "$OUT/ffmpeg$EXE" 2>/dev/null || true
cp configure-cmdline.txt "$OUT/"
echo "built $OUT/ffmpeg$EXE"
ls -la "$OUT"

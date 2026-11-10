#!/usr/bin/env bash
# Installs the build tools in an Ubuntu 22.04 container (CI and local builds).
#   ci/setup-linux.sh [windows]   (also installs llvm-mingw)
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates curl git make perl python3 python3-pip xz-utils bzip2 \
    pkg-config autoconf automake libtool nasm cmake ninja-build \
    clang lld llvm libc6-dev libstdc++-12-dev file > /dev/null
pip3 install -q meson==1.6.1
if [ "${1:-}" = windows ]; then
    V=20241217
    curl -fsSL --retry 5 -o /tmp/llvm-mingw.tar.xz \
        https://github.com/mstorsjo/llvm-mingw/releases/download/$V/llvm-mingw-$V-ucrt-ubuntu-20.04-x86_64.tar.xz
    mkdir -p /opt/llvm-mingw
    tar -xf /tmp/llvm-mingw.tar.xz -C /opt/llvm-mingw --strip-components=1
    rm /tmp/llvm-mingw.tar.xz
fi
clang --version | head -1

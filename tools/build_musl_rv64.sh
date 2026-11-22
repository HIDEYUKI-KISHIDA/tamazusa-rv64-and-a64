#!/usr/bin/env bash
# build_musl_rv64.sh — RISC-V の Linux 用プログラムを作るための musl を用意する（一度だけ）
#
#   1. RISC-V 用のコンパイラ:  sudo apt install gcc-riscv64-linux-gnu
#   2. この台本:               bash tools/build_musl_rv64.sh
#   できるもの: toolchain/musl-rv64/bin/musl-gcc（Makefile が自動で使う）
#   （Linux のヘッダも写すので、Ubuntu の linux-libc-dev-riscv64-cross が要る。
#     gcc-riscv64-linux-gnu と一緒に入る）
set -eu
cd "$(dirname "$0")/.."
TOP=$(pwd)
MUSL_VER=v1.2.5
command -v riscv64-linux-gnu-gcc >/dev/null || {
    echo "riscv64-linux-gnu-gcc がありません: sudo apt install gcc-riscv64-linux-gnu"; exit 1; }
mkdir -p toolchain
if [ ! -d toolchain/musl-src ]; then
    git clone --depth 1 --branch "$MUSL_VER" https://git.musl-libc.org/git/musl toolchain/musl-src \
      || git clone --depth 1 --branch "$MUSL_VER" https://github.com/kraj/musl.git toolchain/musl-src
fi
rm -rf toolchain/musl-build && mkdir -p toolchain/musl-build
cd toolchain/musl-build
../musl-src/configure --target=riscv64-linux-gnu CROSS_COMPILE=riscv64-linux-gnu- \
    --prefix="$TOP/toolchain/musl-rv64" --disable-shared
make -j"$(nproc)"
make install
# busybox などが使う Linux のヘッダ（linux/, asm/ など）を musl の include へ写す
INC="$TOP/toolchain/musl-rv64/include"
# （Windows のフォルダ /mnt/c は大文字と小文字を区別しないので、linux/netfilter の
#   xt_CONNMARK.h と xt_connmark.h のような組は片方しか入らない。使わないので無視して続ける）
for d in linux asm asm-generic mtd; do
    if [ -d "/usr/riscv64-linux-gnu/include/$d" ]; then
        rm -rf "$INC/$d"
        cp -r "/usr/riscv64-linux-gnu/include/$d" "$INC/" 2>/dev/null || true
    fi
done
[ -f "$INC/asm/types.h" ] || { echo "Linux のヘッダ（asm/types.h）が写せませんでした: sudo apt install linux-libc-dev-riscv64-cross"; exit 1; }
echo "OK: $TOP/toolchain/musl-rv64/bin/musl-gcc"

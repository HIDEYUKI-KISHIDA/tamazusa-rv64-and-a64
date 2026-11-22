#!/usr/bin/env bash
# build_busybox_a64.sh — Tamazusa 用の busybox（ash と約 400 のコマンド）を作る（一度だけ）
#
#   先に musl を用意する:  bash tools/build_musl_a64.sh
#   この台本:              bash tools/build_busybox_a64.sh
#   できるもの: toolchain/busybox-a64/busybox（make がディスクに busybox.elf として入れる）
#   設定は tools/busybox_tamazusa.cfg（defconfig の上に重ねる）。
set -eu
cd "$(dirname "$0")/.."
TOP=$(pwd)
BB_TAG=1_36_1
MUSL_GCC="$TOP/toolchain/musl-a64/bin/musl-gcc"
[ -x "$MUSL_GCC" ] || { echo "musl がありません: bash tools/build_musl_a64.sh"; exit 1; }
[ -d "$TOP/toolchain/musl-a64/include/linux" ] || {
    echo "musl に Linux のヘッダがありません: bash tools/build_musl_a64.sh をもう一度"; exit 1; }
mkdir -p toolchain
if [ ! -d toolchain/busybox-src ]; then
    git clone --depth 1 --branch "$BB_TAG" https://github.com/mirror/busybox.git toolchain/busybox-src
fi
cd toolchain/busybox-src
make distclean >/dev/null 2>&1 || true
make defconfig >/dev/null
# 設定を重ねる（同じ名前の行は置き換え、なければ足す）
grep -v '^#' "$TOP/tools/busybox_tamazusa.cfg" | while IFS= read -r line; do
    [ -n "$line" ] || continue
    key=${line%%=*}
    sed -i "/^$key=/d; /^# $key is not set/d" .config
    case "$line" in
        *=n) echo "# $key is not set" >> .config ;;
        *)   echo "$line" >> .config ;;
    esac
done
yes "" | make oldconfig >/dev/null
make -j"$(nproc)" CC="$MUSL_GCC" HOSTCC=gcc STRIP=aarch64-linux-gnu-strip \
    CROSS_COMPILE=aarch64-linux-gnu-
make busybox.links CC="$MUSL_GCC" HOSTCC=gcc CROSS_COMPILE=aarch64-linux-gnu- >/dev/null
mkdir -p "$TOP/toolchain/busybox-a64"
cp busybox "$TOP/toolchain/busybox-a64/busybox"
cp busybox.links "$TOP/toolchain/busybox-a64/busybox.links"   # コマンドの一覧（/bin/ls などを探すのに使う）
echo "OK: $TOP/toolchain/busybox-a64/busybox"

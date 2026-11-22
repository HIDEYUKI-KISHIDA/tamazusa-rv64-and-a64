#!/usr/bin/env bash
# build_bash_rv64.sh — Tamazusa 用の GNU bash を作る（一度だけ）
#
#   先に musl を用意する:  bash tools/build_musl_rv64.sh
#   この台本:              bash tools/build_bash_rv64.sh
#   できるもの: toolchain/bash-rv64/bash（make がディスクに bash.elf として入れる）
#   ソースは GNU の配布場所から取る（だめなら Ubuntu の保管庫から）。
set -eu
cd "$(dirname "$0")/.."
TOP=$(pwd)
BASH_VER=5.2.37
MUSL_GCC="$TOP/toolchain/musl-rv64/bin/musl-gcc"
[ -x "$MUSL_GCC" ] || { echo "musl がありません: bash tools/build_musl_rv64.sh"; exit 1; }
mkdir -p toolchain
if [ ! -d toolchain/bash-src ]; then
    rm -f toolchain/bash.tar
    curl -fL -o toolchain/bash.tar "https://ftp.gnu.org/gnu/bash/bash-$BASH_VER.tar.gz" \
      || curl -fL -o toolchain/bash.tar "http://archive.ubuntu.com/ubuntu/pool/main/b/bash/bash_$BASH_VER.orig.tar.xz"
    mkdir -p toolchain/bash-src
    tar xf toolchain/bash.tar -C toolchain/bash-src --strip-components=1
fi
cd toolchain/bash-src
# bash 5.2 には古い C の書き方（引数を書かない関数の宣言・宣言なしの関数の呼び出し・
# 型の合わないポインタ）が多い。新しい gcc は、既定が C23 で（15 以降）、これらを最初から
# エラーにする（14 以降）。C17（gnu17）で、エラーを警告に戻してコンパイルする
# （古い gcc は -fpermissive を「C では使えない」と警告するだけ）
make distclean >/dev/null 2>&1 || true
OLDC="-std=gnu17 -fpermissive -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-int"
CC="$MUSL_GCC" ./configure --host=riscv64-linux-gnu --build="$(gcc -dumpmachine)" \
    --without-bash-malloc --enable-static-link --disable-nls --without-curses \
    CFLAGS="-Os $OLDC" CFLAGS_FOR_BUILD="-O2 $OLDC"
make -j"$(nproc)"
mkdir -p "$TOP/toolchain/bash-rv64"
riscv64-linux-gnu-strip -o "$TOP/toolchain/bash-rv64/bash" bash
echo "OK: $TOP/toolchain/bash-rv64/bash"

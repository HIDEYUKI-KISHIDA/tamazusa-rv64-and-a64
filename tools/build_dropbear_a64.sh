#!/usr/bin/env bash
# build_dropbear_a64.sh — Tamazusa 用の ssh・sshd（Dropbear）を作る（一度だけ）
#
#   先に musl を用意する:  bash tools/build_musl_a64.sh
#   この台本:              bash tools/build_dropbear_a64.sh
#   できるもの:
#     toolchain/dropbear-a64/dropbear        dropbear・dbclient（ssh）・dropbearkey・scp を 1 つにしたもの
#     toolchain/dropbear-a64/dropbear.links  コマンドの一覧（ssh や scp の名前で dropbear.elf を探すのに使う）
#     toolchain/dropbear-a64/etc/dropbear/dropbear_ed25519_host_key
#         sshd のホスト鍵。ディスクは読み取り専用なので、パソコンの上で作っておく
#         （パソコン用の dropbearkey もここで作る）。作り直すまで同じ鍵を使う
#   make がディスクに dropbear.elf・dropbear.links・etc/dropbear/ として入れる。
set -eu
cd "$(dirname "$0")/.."
TOP=$(pwd)
DB_TAG=DROPBEAR_2024.86
MUSL_GCC="$TOP/toolchain/musl-a64/bin/musl-gcc"
OUT="$TOP/toolchain/dropbear-a64"
[ -x "$MUSL_GCC" ] || { echo "musl がありません: bash tools/build_musl_a64.sh"; exit 1; }
mkdir -p toolchain
if [ ! -d toolchain/dropbear-src ]; then
    git clone --depth 1 --branch "$DB_TAG" https://github.com/mkj/dropbear.git toolchain/dropbear-src
fi
OPTS="--disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx
      --disable-pututline --disable-pututxline --disable-syslog"
LOCALOPT='#define DEFAULT_PATH "/bin:/"
#define DROPBEAR_X11FWD 0'

# AArch64 用（musl で静的リンク）
rm -rf toolchain/dropbear-build-a64 && cp -r toolchain/dropbear-src toolchain/dropbear-build-a64
( cd toolchain/dropbear-build-a64
  CC="$MUSL_GCC" ./configure --host=aarch64-linux-gnu --build="$(gcc -dumpmachine)" --enable-static $OPTS CFLAGS="-Os"
  printf '%s\n' "$LOCALOPT" > localoptions.h
  make -j"$(nproc)" PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1 )
mkdir -p "$OUT/etc/dropbear"
aarch64-linux-gnu-strip -o "$OUT/dropbear" toolchain/dropbear-build-a64/dropbearmulti
printf '/usr/sbin/dropbear\n/usr/bin/dbclient\n/usr/bin/ssh\n/usr/bin/dropbearkey\n/usr/bin/scp\n' > "$OUT/dropbear.links"

# ホスト鍵（パソコン用の dropbearkey で作る。すでにあれば作り直さない）
KEY="$OUT/etc/dropbear/dropbear_ed25519_host_key"
if [ ! -f "$KEY" ]; then
    rm -rf toolchain/dropbear-host && cp -r toolchain/dropbear-src toolchain/dropbear-host
    ( cd toolchain/dropbear-host
      ./configure $OPTS
      make -j"$(nproc)" PROGRAMS=dropbearkey )
    toolchain/dropbear-host/dropbearkey -t ed25519 -f "$KEY"
fi
echo "OK: $OUT/dropbear（root のパスワードは tamazusa。user/disk/etc/shadow）"

#!/bin/sh
# build_basic_win.sh — T88-BASIC の Windows 版（basic.exe）を作る
#
#   WSL・Linux から MinGW-w64 で作る。要るもの: sudo apt install gcc-mingw-w64-x86-64
#   できるもの: build-win/basic.exe と build-win/LICENSE.txt（2 つをいっしょに配る）
#   Windows 10 1903 以降で動く（ファイル名に UTF-8 を使うため）
set -e
cd "$(dirname "$0")/.."
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
command -v "$CC" >/dev/null || { echo "ERROR: $CC がありません（sudo apt install gcc-mingw-w64-x86-64）"; exit 1; }
mkdir -p build-win
(cd user/windows && "$WINDRES" basic.rc -O coff -o ../../build-win/basic_res.o)
"$CC" -O2 -Wall -static -o build-win/basic.exe user/linux/basic.c build-win/basic_res.o -lm -lpthread -lcomdlg32
cp user/windows/LICENSE.txt build-win/LICENSE.txt
echo "OK: build-win/basic.exe（LICENSE.txt といっしょに配る）"

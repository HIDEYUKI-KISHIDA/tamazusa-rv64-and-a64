#!/usr/bin/env python3
"""tools/mkfont.py — 画面の文字（kernel/gui/font16.c）を GNU Unifont から作る

  使い方: python3 tools/mkfont.py [unifont.otf] [unifont_jp.otf] > kernel/gui/font16.c
  （Debian・Ubuntu では apt install fonts-unifont で /usr/share/fonts/opentype/unifont/ に入る）

  入れる文字: ASCII（0x20〜0x7e）、半角カナ（U+FF61〜FF9F）、シフト JIS（cp932）の 2 バイト文字すべて
  （JIS X 0208 と NEC の特殊文字・拡張）、罫線（U+2500〜257F）、ブロック（U+2580〜259F）。
  漢字・かなは unifont_jp（日本の字形）、そのほかは unifont から取る。
  幅 8（半角）と 16（全角）の 2 つの表に分け、それぞれ文字の番号の順に並べる（二分探索で引く）。

  GNU Unifont は SIL Open Font License 1.1 と GPL 2 以降（フォント埋め込みの例外つき）の二重ライセンス。
  Tamazusa は OFL 1.1 で使う（出力のファイルの頭に、OFL の求める著作権表示を入れる）。
"""
import sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

base = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/opentype/unifont/unifont.otf'
jp = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/fonts/opentype/unifont/unifont_jp.otf'

def cmap_of(path):
    return TTFont(path).getBestCmap()

cm_base, cm_jp = cmap_of(base), cmap_of(jp)
f_base = ImageFont.truetype(base, 16)
f_jp = ImageFont.truetype(jp, 16)

codes = set(range(0x20, 0x7f)) | set(range(0xff61, 0xffa0)) | set(range(0x2500, 0x25a0))
for hi in list(range(0x81, 0xa0)) + list(range(0xe0, 0xfd)):
    for lo in list(range(0x40, 0x7f)) + list(range(0x80, 0xfd)):
        try:
            s = bytes([hi, lo]).decode('cp932')
        except UnicodeDecodeError:
            continue
        if len(s) == 1:
            codes.add(ord(s))
codes.add(0xfffd)

def is_jp(c):
    return 0x3000 <= c <= 0x9fff or 0xf900 <= c <= 0xfaff or 0xff00 <= c <= 0xffef

def render(c):
    use_jp = is_jp(c) and c in cm_jp
    font = f_jp if use_jp else f_base
    if c not in (cm_jp if use_jp else cm_base):
        return None
    w = int(round(font.getlength(chr(c))))
    if w not in (8, 16):
        return None
    im = Image.new('1', (w, 16), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = '1'
    asc, _ = font.getmetrics()
    d.text((0, 0), chr(c), font=font, fill=1)
    rows = []
    for y in range(16):
        v = 0
        for x in range(w):
            if im.getpixel((x, y)):
                v |= 1 << (w - 1 - x)
        rows.append(v)
    return w, rows

narrow, wide = [], []
for c in sorted(codes):
    r = render(c)
    if r is None:
        continue
    (narrow if r[0] == 8 else wide).append((c, r[1]))

out = sys.stdout
out.write('''/* kernel/gui/font16.c — 画面の文字（16 ドット）。tools/mkfont.py で作ったもの（手で直さない）
 *
 *   元のフォント: GNU Unifont 15.1.01（unifont.otf・unifont_jp.otf）
 *   Copyright (C) 1998-2023 Roman Czyborra, Paul Hardy, Qianqian Fang, Andrew Miller,
 *   Johnnie Weaver, David Corbett, Nils Moskopp, Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, et al.
 *   SIL Open Font License, Version 1.1 で使う（https://openfontlicense.org/ ・ docs/licenses/OFL-1.1.txt）。
 *   Unifont は OFL 1.1 と GNU GPL 2 以降（フォント埋め込みの例外つき）の二重ライセンス。
 *   この表は元のフォントから点の形を写したもので、名前は「Unifont」のまま（予約名はない）。
 */
#include <stdint.h>
#include "font16.h"

''')
out.write('const unsigned font16_narrow_n = %d;\n' % len(narrow))
out.write('const uint16_t font16_narrow_code[] = {\n')
for i in range(0, len(narrow), 12):
    out.write('    ' + ', '.join('0x%04x' % c for c, _ in narrow[i:i+12]) + ',\n')
out.write('};\nconst uint8_t font16_narrow_bits[][16] = {\n')
for c, rows in narrow:
    out.write('    {' + ','.join('0x%02x' % v for v in rows) + '},\n')
out.write('};\n\n')
out.write('const unsigned font16_wide_n = %d;\n' % len(wide))
out.write('const uint16_t font16_wide_code[] = {\n')
for i in range(0, len(wide), 12):
    out.write('    ' + ', '.join('0x%04x' % c for c, _ in wide[i:i+12]) + ',\n')
out.write('};\nconst uint16_t font16_wide_bits[][16] = {\n')
for c, rows in wide:
    out.write('    {' + ','.join('0x%04x' % v for v in rows) + '},\n')
out.write('};\n')
sys.stderr.write('narrow %d, wide %d\n' % (len(narrow), len(wide)))

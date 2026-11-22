/* kernel/gui/font16.h — 画面の文字（16 ドット、GNU Unifont から。font16.c・tools/mkfont.py） */
#ifndef FONT16_H
#define FONT16_H
#include <stdint.h>

extern const unsigned font16_narrow_n;
extern const uint16_t font16_narrow_code[];
extern const uint8_t  font16_narrow_bits[][16];     /* 幅 8: 1 行 1 バイト（上位ビットが左） */
extern const unsigned font16_wide_n;
extern const uint16_t font16_wide_code[];
extern const uint16_t font16_wide_bits[][16];       /* 幅 16: 1 行 2 バイト（上位ビットが左） */

#endif

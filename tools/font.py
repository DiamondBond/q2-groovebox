#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""src/font.h: printable ASCII from Liberation Sans Bold (SIL OFL 1.1) at 20 px (the strip) and 14 px (pad
labels), an alpha byte a pixel, after Buddy's tools/font.py (q2-fidget, MIT). Run when a size or the font
changes; the build needs no Python."""
import pathlib
from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).parent
out = ['/* tools/font.py: Liberation Sans Bold, ASCII 32-126, an alpha byte a pixel */']
for size in (20, 14):
    font = ImageFont.truetype(str(HERE / 'LiberationSans-Bold.ttf'), size)
    ascent, descent = font.getmetrics()
    h = ascent + descent
    adv, off, px = [], [], bytearray()
    for c in map(chr, range(32, 127)):
        w = max(1, round(font.getlength(c)))
        im = Image.new('L', (w, h))
        ImageDraw.Draw(im).text((0, 0), c, font=font, fill=255)
        adv.append(w), off.append(len(px)), px.extend(im.tobytes())
    out += [f'#define F{size}_H {h}',
            f'static const unsigned char f{size}_adv[95] = {{' + ','.join(map(str, adv)) + '};',
            f'static const unsigned short f{size}_off[95] = {{' + ','.join(map(str, off)) + '};',
            f'static const unsigned char f{size}_px[] = {{']
    out += [','.join(map(str, px[i:i + 40])) + ',' for i in range(0, len(px), 40)]
    out.append('};')
(HERE.parent / 'src' / 'font.h').write_text('\n'.join(out) + '\n')

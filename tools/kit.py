#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""kits/default: sixteen sounds from Felucca's CC0 generator (tools/gen_waves.py, Leo Kuroshita / Hügelton
Instruments), rendered at 44.1 kHz: eleven drums, three plucks, and two single-cycle waves held for a
second, as a pad plays a sound once. Pad order is the file's number. Run when the generator changes; the
build needs no Python."""
import array, pathlib, shutil, sys, tempfile, wave
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gen_waves as g

g.SR = 44100
ORDER = ['D KICK', 'D SNARE', 'D CHAT', 'D OHAT', 'D CLAP', 'D TOM LO', 'D TOM HI', 'D RIM',
         'D COWBELL', 'D CRASH', 'D RIDE', 'P PLUCK C2', 'P PLUCK C3', 'P PLUCK C4', 'W ORGAN', 'W GLASS']
NAMES = ['Kick', 'Snare', 'Hat', 'Open Hat', 'Clap', 'Tom Lo', 'Tom Hi', 'Rim',
         'Cowbell', 'Crash', 'Ride', 'Pluck Lo', 'Pluck', 'Pluck Hi', 'Organ', 'Glass']
out = pathlib.Path(__file__).parent.parent / 'kits' / 'default'

def held(src, dst, secs=1.0, fade=0.05):
    """the wave's cycle repeated for secs, faded out so the end does not click"""
    with wave.open(str(src)) as w:
        cycle = array.array('h', w.readframes(w.getnframes()))
    n, f = int(secs * g.SR), int(fade * g.SR)
    pcm = array.array('h', (int(cycle[i % len(cycle)] * min(1, (n - i) / f)) for i in range(n)))
    with wave.open(str(dst), 'wb') as w:
        w.setparams((1, 2, g.SR, 0, 'NONE', ''))
        w.writeframes(pcm.tobytes())


with tempfile.TemporaryDirectory() as tmp:
    g.main(tmp)
    for p in out.glob('*.wav'):
        p.unlink()
    for i, (src, name) in enumerate(zip(ORDER, NAMES)):
        dst = out / f'{i + 1:02d} {name}.wav'
        if src.startswith('W '):
            held(pathlib.Path(tmp) / f'{src}.wav', dst)
        else:
            shutil.copy(pathlib.Path(tmp) / f'{src}.wav', dst)
print(sorted(p.name for p in out.glob('*.wav')))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""kits/default: sixteen sounds from Felucca's CC0 generator (tools/gen_waves.py, Leo Kuroshita / Hügelton
Instruments), rendered at 44.1 kHz: eleven drums, three plucks, two single-cycle waves. Pad order is the
file's number. Run when the generator changes; the build needs no Python."""
import pathlib, shutil, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gen_waves as g

g.SR = 44100
ORDER = ['D KICK', 'D SNARE', 'D CHAT', 'D OHAT', 'D CLAP', 'D TOM LO', 'D TOM HI', 'D RIM',
         'D COWBELL', 'D CRASH', 'D RIDE', 'P PLUCK C2', 'P PLUCK C3', 'P PLUCK C4', 'W ORGAN', 'W GLASS']
NAMES = ['Kick', 'Snare', 'Hat', 'Open Hat', 'Clap', 'Tom Lo', 'Tom Hi', 'Rim',
         'Cowbell', 'Crash', 'Ride', 'Pluck Lo', 'Pluck', 'Pluck Hi', 'Organ', 'Glass']
out = pathlib.Path(__file__).parent.parent / 'kits' / 'default'
with tempfile.TemporaryDirectory() as tmp:
    g.main(tmp)
    for p in out.glob('*.wav'):
        p.unlink()
    for i, (src, name) in enumerate(zip(ORDER, NAMES)):
        shutil.copy(pathlib.Path(tmp) / f'{src}.wav', out / f'{i + 1:02d} {name}.wav')
print(sorted(p.name for p in out.glob('*.wav')))

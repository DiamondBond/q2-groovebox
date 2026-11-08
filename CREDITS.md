# Credits

Groovebox is GPL-3.0-only, Copyright (C) 2026 Diamond Bond. It builds on:

- **Felucca** by Leo Kuroshita (Hügelton Instruments): `tools/gen_waves.py`, the generator of the
  default kit (the Hügelton Sample Pack), CC0-1.0. `kits/default/` is its output at 44.1 kHz.
- **q2-fidget (Buddy)** by Diamond Bond, MIT: the framebuffer, input socket and bitmap font
  recipe that `src/platform_q2.c` and `tools/font.py` follow.
- **Rockbox**: `src/crt-q2.c`, the Q2 start file (GPL-2.0-or-later, Copyright (C) 2026 Diamond Bond).
- **Liberation Sans Bold** (SIL OFL 1.1), rendered into `src/font.h`.
- **Q2 Pod**'s q2video: the Bluetooth soft-volume curve (`bt_gain`) and the ALSA notes.

Licence texts are in `LICENSES/`.

# Credits

Groovebox is GPL-3.0-only, Copyright (C) 2026 Diamond Bond. It builds on:

- **Felucca** by Leo Kuroshita (Hügelton Instruments): `tools/gen_waves.py`, the generator of the
  default kit (the Hügelton Sample Pack), CC0-1.0. `kits/default/` is its output at 44.1 kHz.
- **SLOOP** by isod89 (itself a fork of Felucca, Copyright (C) 2026 Leo Kuroshita, Hügelton
  Instruments), GPL-3.0-only: `src/punch.c` (the punch-in effects), `src/sloop_buses.c` (the tempo
  delay and reverb, from `fx.c`), `src/sloop_dsp.h` (from `dsp.c`) and `src/sloop_tables.h` (generated
  by its `tools/gen_tables.py`). Commit bac9c05.
- **X0X** by Charles Vestal, GPL-3.0-only, commit a0d1a13: `kits/909/` and `kits/808/` are rendered from
  its drum models by `tools/x0x_render.c`. The 909 is ported from **9W9** (athousanddetails) after
  **ER-99** (Matthew Cieplak), both GPL-3.0, which also supply its hi-hat, ride and crash samples. The
  808 is ported from **8W8** (athousanddetails), GPL-3.0; its rim shot is after sc808 (Yoshinosuke
  Horiuchi / Sam Aaron). TR-808 and TR-909 are trademarks of Roland Corporation, used only to describe
  what the models emulate.
- **VSCO-2 Community Edition** and **VCSL** by Versilian Studios (Sam Gossner), CC0-1.0, via Felucca's
  `assets/samples-cc0` (commit a4f007c): `kits/melodic/`, a second of each, some pitched.
- **q2-fidget (Buddy)** by Diamond Bond, MIT: the framebuffer, input socket and bitmap font
  recipe that `src/platform_q2.c` and `tools/font.py` follow.
- **Rockbox**: `src/crt-q2.c`, the Q2 start file (GPL-2.0-or-later, Copyright (C) 2026 Diamond Bond).
- **Liberation Sans Bold** (SIL OFL 1.1), rendered into `src/font.h`.
- **Q2 Pod**'s q2video: the Bluetooth soft-volume curve (`bt_gain`) and the ALSA notes.

Licence texts are in `LICENSES/`.

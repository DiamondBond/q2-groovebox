/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The six Felucca engines, a voice a pad. */
#ifndef SYNTH_H
#define SYNTH_H
#include "engine.h"
#define SYNTH_ENGINES 6
enum { SY_ATK = 8, SY_DEC, SY_SUS, SY_REL, SY_FENV, SY_PRESET, SY_NEW, SY_MUTATE }; /* the machine's boxes: eight of the engine's first */
void synth_init(void);
const char *synth_engine(int e);
int synth_presets(int e);
const char *synth_preset_name(int e, int p);
void synth_preset(int e, int p, int *vals, int *echo, int *reverb); /* vals[0..SY_FENV] and its sends */
void synth_apply(int pad, const int *vals);
void synth_on(int pad, int e, int note, int vel, int fine, int slide);
void synth_off(int pad);  /* the key lifts: the release */
void synth_kill(int pad); /* gone at once */
int synth_fill(int pad, int e, float *s); /* CTL frames, Q15; 0 once it has ended */
int synth_tile(int pad, int e, int i, tile_t *t);
#endif

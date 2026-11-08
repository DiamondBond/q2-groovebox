/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sampler: sixteen voices, one a pad, into a Q15 stereo mix in CTL-frame blocks. */
#ifndef ENGINE_H
#define ENGINE_H
#include "groovebox.h"
typedef struct {
    short *pcm; /* mono, FS */
    int n;
    char name[32], file[256];
} sample_t;
/* SOUND's sixteen tiles, in pad order (Load is ui.c's) */
enum { P_LEVEL, P_PAN, P_PITCH, P_FINE, P_START, P_END, P_LOOP, P_REVERSE,
       P_ATTACK, P_DECAY, P_CUTOFF, P_RESO, P_ECHO, P_REVERB, P_CHOKE, P_LOAD, P_N };
typedef struct {
    int p[P_N];
    sample_t smp;
} sound_t;
extern sound_t sounds[NPAD];
extern const char *const param_names[P_N];
extern const int param_min[P_N], param_max[P_N], param_def[P_N];

void engine_init(void);
/* pad sounds: level 0..127 or UNSET (the sound's), pitch in semitones or UNSET, decay and cutoff 0..127 or UNSET */
void engine_trigger(int pad, int level, int pitch, int decay, int cutoff);
void engine_block(int *out); /* CTL frames, interleaved Q15 in int, mixed, swept, limited */
void engine_gain(int q16);   /* the master soft volume, 65536 unity */
void engine_sweep(int v);    /* PLAY's knob: -100 (low-pass closed) .. 0 (off) .. 100 (high-pass) */
int engine_peak(void);       /* the last block's peak, 0..32767 */
unsigned engine_blocks(void);           /* blocks rendered since start */
unsigned engine_last_hit(int pad);      /* the block the pad last sounded in */
int bt_gain(int vol);                   /* hciplayer's soft-volume curve, 0..100 to Q16 */
#endif

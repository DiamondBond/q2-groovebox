/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sampler: sixteen voices, one a pad, into a Q15 stereo mix in CTL-frame blocks. */
#ifndef ENGINE_H
#define ENGINE_H
#include "groovebox.h"
typedef struct {
    short *pcm; /* mono, FS */
    int n;
    char name[32];
    int loaded; /* from Load, kept beside the project as pad%02d.wav */
} sample_t;
/* SOUND's sixteen tiles, in pad order (Load is ui.c's) */
enum { P_LEVEL, P_PAN, P_PITCH, P_FINE, P_START, P_END, P_LOOP, P_REVERSE,
       P_ATTACK, P_DECAY, P_CUTOFF, P_RESO, P_ECHO, P_REVERB, P_CHOKE, P_LOAD, P_N };
enum { SRC_SAMPLE, SRC_SYNTH, SRC_909, SRC_808, SRC_303, SRC_BREAK, SRC_N }; /* what a pad plays */
#define NTILE 16
typedef struct { const char *name; int min, max; const char *const *names; int action; } tile_t; /* one of SOUND 2's */
enum { ACT_NONE, ACT_NEW, ACT_MUTATE };
typedef struct {
    int p[P_N];
    sample_t smp;
    int src, sub, preset; /* SRC_*; the synth engine, or the drum in its machine; the synth's preset */
    int e[NTILE];         /* SOUND 2's values */
} sound_t;
enum { NOTE_ACCENT = 1, NOTE_SLIDE = 2 };
extern sound_t sounds[NPAD];
extern const char *const param_names[P_N];
extern const int param_min[P_N], param_max[P_N], param_def[P_N];

void engine_init(void);
/* pad sounds: level 0..127 or UNSET (the sound's), pitch in semitones or UNSET, decay and cutoff 0..127 or UNSET */
void engine_trigger(int pad, int level, int pitch, int decay, int cutoff);
/* with NOTE_* flags and a gate in frames for what holds a note: 0 a short one, -1 until engine_release */
void engine_note(int pad, int level, int pitch, int decay, int cutoff, int flags, int gate);
void engine_release(int pad);
void engine_break_step(int pad, int step, unsigned bar, int frames, int on, int level, int decay, int cutoff); /* each sixteenth */
void engine_transport_stop(void);
void engine_load(int percent);          /* how much of its time the audio thread is using */
void engine_block(int *out); /* CTL frames, interleaved Q15 in int, mixed, swept, limited */
void engine_gain(int q16);   /* the master soft volume, 65536 unity */
void engine_sweep(int v);    /* PLAY's knob: -100 (low-pass closed) .. 0 (off) .. 100 (high-pass) */
int engine_peak(void);       /* the last block's peak, 0..32767 */
unsigned engine_blocks(void);           /* blocks rendered since start */
unsigned engine_last_hit(int pad);      /* the block the pad last sounded in */
void engine_stop(int pad);              /* cuts the pad's voice, under ui_lock */
void engine_offline(int on);            /* under ui_lock: the audio thread renders silence, the UI thread the engine */
int engine_is_offline(void);
int bt_gain(int vol);                   /* hciplayer's soft-volume curve, 0..100 to Q16 */
#endif

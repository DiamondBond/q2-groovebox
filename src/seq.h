/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sequencer: sixteen patterns of sixteen tracks of sixteen steps, a clock in frames. */
#ifndef SEQ_H
#define SEQ_H
#include "groovebox.h"
typedef struct {
    signed char on, level, pitch, decay, cutoff, nudge; /* locks: UNSET is the sound's own */
} step_t;
typedef struct {
    signed char step, len, fx; /* an effect (fx.h) over len sixteenths from step */
} fxev_t;
#define NFXEV 16
typedef struct {
    step_t s[NPAD][NSTEP];
    unsigned char len[NPAD]; /* 1..16 steps a track, so tracks drift (polymeter) */
    fxev_t ev[NFXEV];        /* the bar's recorded effects; a later one wins where they overlap */
    int nev;
} pattern_t;
typedef struct {
    pattern_t pat[NPATT];
    int bpm, swing;   /* 40..240; 0..100, the off steps delayed up to a third of a step */
    int cur, cue;     /* the pattern playing; the one cued for the bar's end, or -1 */
    int chain[2];     /* two patterns taking turns, or -1 */
    int playing, rec;
    int pos;          /* frames into the bar */
    unsigned bar;     /* bars since play began */
    struct { signed char pat, pad, step; } take[256]; /* pad -1: step is an FX event's index */
    int ntake; /* what this REC pass wrote, for Return's undo */
    struct { int on, pat, step, start; unsigned bar; } fxrec; /* the event the finger is writing */
} seq_t;
extern seq_t seq;
void seq_init(void);
int seq_step_frames(void);          /* frames a sixteenth */
void seq_block(void);               /* CTL frames on: the steps in them sound (engine_trigger) */
void seq_play(int on);
void seq_cue(int pattern);          /* now when stopped, at the bar's end when playing */
void seq_tap(int pad);              /* PLAY's pad: sounds now; with REC on and playing, lands on its step */
void seq_rec(int on);
void seq_fx_start(int fx);          /* REC on and playing: an FX event starts, quantised */
int seq_fx_end(void);               /* and its length is set: 1 if one was being written */
void seq_undo(void);
int seq_playhead(void);             /* the sixteenth sounding, 0..15; -1 stopped */
int seq_quantise(int pos);          /* the nearest sixteenth to pos frames, 0..16 */
#endif

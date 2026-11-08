/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sequencer: sixteen patterns of sixteen tracks of up to sixty-four steps, a clock in frames. */
#ifndef SEQ_H
#define SEQ_H
#include "groovebox.h"
typedef struct {
    signed char on, level, pitch, decay, cutoff, nudge; /* locks: UNSET is the sound's own */
    signed char ratchet, chance, flags; /* hits in the step (0 and 1: one); percent, 0 always; NOTE_* */
} step_t;
typedef struct {
    signed char step, len, fx; /* an effect (fx.h) over len sixteenths from step */
} fxev_t;
#define NFXEV 16
#define NSONG 64
#define RATE_MIN (-2) /* a track's rate: 1/32 1/16T 1/16 1/8T 1/8 1/4 */
#define RATE_MAX 3
#define NSCALE 6
typedef struct {
    step_t s[NPAD][NSTEP];
    unsigned char len[NPAD]; /* 1..64 steps a track, so tracks drift (polymeter) */
    signed char rate[NPAD];  /* RATE_MIN..RATE_MAX, 0 a sixteenth */
    fxev_t ev[NFXEV];        /* the pattern's recorded effects; a later one wins where they overlap */
    int nev;
} pattern_t;
typedef struct {
    pattern_t pat[NPATT];
    int bpm, swing;   /* 40..240; 0..100, the off steps delayed up to a third of a step */
    int key, scale;   /* Notes and the generator: semitones from C3, -24..24; NSCALE */
    int cur, cue;     /* the pattern playing; the one cued for its end, or -1 */
    int song[NSONG];  /* the patterns in turn, looping, when there are two or more */
    int nsong, song_at;
    int playing, rec;
    int pos;          /* frames into the bar */
    unsigned bar;     /* bars since play began */
    unsigned pbar;    /* since this pattern began: a long track's page, a short one's drift */
    struct { signed char pat, pad, step; } take[256]; /* pad -1: step is an FX event's index */
    int ntake; /* what this REC pass wrote, for Undo */
    struct { int on, pat, step, start; unsigned bar; } fxrec; /* the event the finger is writing */
    struct { step_t s[NSTEP]; int pat, pad, len, valid; } gen; /* the track before TB-3PO rewrote it */
} seq_t;
extern seq_t seq;
void seq_init(void);
int seq_step_frames(void);          /* frames a sixteenth */
int seq_ticks(int rate);            /* a step of that rate, in 96ths of a bar */
const char *seq_rate_name(int rate);
const char *seq_scale_name(int scale);
int seq_scale_note(int i);          /* semitones above the key of the scale's i-th note, up through the octaves */
int seq_bars(const pattern_t *p);   /* how long a pattern plays before a cue or the song moves on */
int seq_song_frames(void);          /* one pass of the song, or of the current pattern */
void seq_block(void);               /* CTL frames on: the steps in them sound (engine_note) */
void seq_play(int on);
void seq_cue(int pattern);          /* now when stopped, at the pattern's end when playing */
void seq_tap(int pad);              /* a pad: sounds now, held; with REC on and playing, lands on its step */
void seq_note(int pad, int pitch);  /* Notes: the same, at a pitch, which a step keeps */
void seq_rec(int on);
void seq_fx_start(int fx);          /* REC on and playing: an FX event starts, quantised */
int seq_fx_end(void);               /* and its length is set: 1 if one was being written */
void seq_undo(void);
int seq_playhead(int track);        /* the step of the track sounding; -1 stopped */
int seq_quantise(int pos);          /* the nearest sixteenth to pos frames, 0..16 */
void seq_song_add(int pattern);     /* onto the end; the first add starts from the pattern playing */
void seq_song_pop(void);
void seq_starter(void);              /* a project with no steps at all starts with a plain beat */
#endif

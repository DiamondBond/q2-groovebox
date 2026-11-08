/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The clock advances a block at a time; each block fires the step onsets that fall in it. A
 * sixteenth is 15 / bpm seconds; the off steps (odd sixteenths) are delayed by swing, a nudge moves
 * one step up to half a step either way. Patterns change on the bar. ponytail: a nudge before the
 * bar's first onset, or past its last, falls outside the bar and is not heard. */
#include <string.h>
#include "seq.h"
#include "engine.h"

seq_t seq;

void seq_init(void) {
    memset(&seq, 0, sizeof seq);
    seq.bpm = 120;
    seq.cue = seq.chain[0] = seq.chain[1] = -1;
    for (int p = 0; p < NPATT; ++p)
        for (int t = 0; t < NPAD; ++t) {
            seq.pat[p].len[t] = NSTEP;
            for (int s = 0; s < NSTEP; ++s) {
                step_t *st = &seq.pat[p].s[t][s];
                st->on = 0;
                st->level = st->pitch = st->decay = st->cutoff = UNSET;
                st->nudge = 0;
            }
        }
}
int seq_step_frames(void) { return FS * 15 / seq.bpm; }
int seq_quantise(int pos) {
    int f = seq_step_frames();
    return (pos + f / 2) / f;
}
static int onset(int s, int stepf, int nudge) {
    int o = s * stepf + (s & 1 ? stepf * seq.swing / 300 : 0);
    return o + nudge * stepf / 256;
}
static void fire(int a, int b) { /* onsets in [a, b) of the current bar */
    int stepf = seq_step_frames();
    pattern_t *p = &seq.pat[seq.cur];
    for (int t = 0; t < NPAD; ++t)
        for (int s = 0; s < NSTEP; ++s) {
            step_t *st = &p->s[t][(seq.bar * NSTEP + (unsigned)s) % p->len[t]];
            if (!st->on) continue;
            int o = onset(s, stepf, st->nudge);
            if (o >= a && o < b) engine_trigger(t, st->level, st->pitch, st->decay, st->cutoff);
        }
}
static void bar_end(void) {
    if (seq.cue >= 0) seq.cur = seq.cue, seq.cue = -1;
    else if (seq.chain[0] >= 0 && seq.chain[1] >= 0)
        seq.cur = seq.cur == seq.chain[0] ? seq.chain[1] : seq.chain[0];
    ++seq.bar;
}
void seq_block(void) {
    if (!seq.playing) return;
    int barf = seq_step_frames() * NSTEP, end = seq.pos + CTL;
    fire(seq.pos, end < barf ? end : barf);
    if (end >= barf) {
        bar_end();
        seq.pos = end - barf;
        fire(0, seq.pos);
    } else
        seq.pos = end;
}
void seq_play(int on) {
    if (on == seq.playing) return;
    seq.playing = on;
    seq.pos = 0;
    seq.bar = 0;
    if (on && seq.cue >= 0) seq.cur = seq.cue, seq.cue = -1;
}
void seq_cue(int pattern) {
    pattern = ((pattern % NPATT) + NPATT) % NPATT;
    if (seq.playing) seq.cue = pattern == seq.cur ? -1 : pattern;
    else seq.cur = pattern, seq.cue = -1;
}
void seq_rec(int on) {
    seq.rec = on;
    seq.ntake = 0;
}
void seq_tap(int pad) {
    if (!seq.playing || !seq.rec) { engine_trigger(pad, UNSET, UNSET, UNSET, UNSET); return; }
    int n = seq_quantise(seq.pos), stepf = seq_step_frames();
    pattern_t *p = &seq.pat[seq.cur];
    int idx = (int)((seq.bar * NSTEP + (unsigned)n) % p->len[pad]);
    step_t *st = &p->s[pad][idx];
    if (!st->on && seq.ntake < (int)(sizeof seq.take / sizeof *seq.take)) {
        seq.take[seq.ntake].pad = (signed char)pad;
        seq.take[seq.ntake].step = (signed char)idx;
        ++seq.ntake;
    }
    st->on = 1;
    if (n * stepf <= seq.pos) engine_trigger(pad, UNSET, UNSET, UNSET, UNSET); /* its onset has passed */
}
void seq_undo(void) {
    while (seq.ntake > 0) {
        --seq.ntake;
        seq.pat[seq.cur].s[(int)seq.take[seq.ntake].pad][(int)seq.take[seq.ntake].step].on = 0;
    }
}
int seq_playhead(void) {
    if (!seq.playing) return -1;
    int s = seq.pos / seq_step_frames();
    return s < NSTEP ? s : NSTEP - 1;
}

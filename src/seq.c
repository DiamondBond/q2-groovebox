/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The clock advances a block at a time; each block fires the step onsets that fall in it. A
 * sixteenth is 15 / bpm seconds; the off steps (odd sixteenths) are delayed by swing, a nudge moves
 * one step up to half a step either way. Patterns change on the bar. ponytail: a nudge before the
 * bar's first onset, or past its last, falls outside the bar and is not heard. */
#include <string.h>
#include "seq.h"
#include "engine.h"
#include "fx.h"

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
static int fx_at(int pos) { /* the current pattern's effect at pos frames into the bar, -1 none */
    const pattern_t *p = &seq.pat[seq.cur];
    int s = pos / seq_step_frames();
    for (int i = p->nev - 1; i >= 0; --i)
        if (s >= p->ev[i].step && s < p->ev[i].step + p->ev[i].len) return p->ev[i].fx;
    return -1;
}
void seq_block(void) {
    fx_clock(seq.playing, seq.bpm, seq.pos % (seq_step_frames() * 4));
    if (!seq.playing) return;
    fx_seq(fx_at(seq.pos));
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
    if (!on) fx_seq(-1);
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
        seq.take[seq.ntake].pat = (signed char)seq.cur;
        seq.take[seq.ntake].pad = (signed char)pad;
        seq.take[seq.ntake].step = (signed char)idx;
        ++seq.ntake;
    }
    st->on = 1;
    if (n >= NSTEP || onset(n, stepf, st->nudge) <= seq.pos) /* its onset has passed: sound it now */
        engine_trigger(pad, UNSET, UNSET, UNSET, UNSET);
}
void seq_fx_start(int fx) {
    pattern_t *p = &seq.pat[seq.cur];
    if (!seq.playing || !seq.rec || p->nev >= NFXEV) return;
    int n = seq_quantise(seq.pos);
    if (seq.ntake < (int)(sizeof seq.take / sizeof *seq.take)) {
        seq.take[seq.ntake].pat = (signed char)seq.cur;
        seq.take[seq.ntake].pad = -1;
        seq.take[seq.ntake].step = (signed char)p->nev;
        ++seq.ntake;
    }
    p->ev[p->nev].step = (signed char)(n % NSTEP);
    p->ev[p->nev].len = 1;
    p->ev[p->nev].fx = (signed char)fx;
    seq.fxrec.on = 1, seq.fxrec.pat = seq.cur, seq.fxrec.step = p->nev, seq.fxrec.start = n, seq.fxrec.bar = seq.bar;
    ++p->nev;
}
int seq_fx_end(void) {
    if (!seq.fxrec.on) return 0;
    seq.fxrec.on = 0;
    if (!seq.playing || seq.fxrec.step >= seq.pat[seq.fxrec.pat].nev) return 1; /* stopped, or undone meanwhile */
    fxev_t *e = &seq.pat[seq.fxrec.pat].ev[seq.fxrec.step];
    int len = (int)(seq.bar - seq.fxrec.bar) * NSTEP + seq_quantise(seq.pos) - seq.fxrec.start, room = NSTEP - e->step;
    e->len = (signed char)(len < 1 ? 1 : len > room ? room : len);
    return 1;
}
void seq_undo(void) {
    while (seq.ntake > 0) {
        --seq.ntake;
        int pat = seq.take[seq.ntake].pat, pad = seq.take[seq.ntake].pad, step = seq.take[seq.ntake].step;
        if (pad < 0) seq.pat[pat].nev = step;
        else seq.pat[pat].s[pad][step].on = 0;
    }
}
int seq_playhead(void) {
    if (!seq.playing) return -1;
    int s = seq.pos / seq_step_frames();
    return s < NSTEP ? s : NSTEP - 1;
}

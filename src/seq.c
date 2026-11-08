/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The clock advances a block at a time; each block fires the step onsets that fall in it. A
 * sixteenth is 15 / bpm seconds; the off sixteenths are delayed by swing, a nudge moves one step up
 * to half a step either way, a ratchet divides a step into hits. A track's rate says how many steps
 * a bar has. Patterns change at their end. ponytail: a nudge before the bar's first onset, or past
 * its last, falls outside the bar and is not heard. */
#include <string.h>
#include "seq.h"
#include "engine.h"
#include "fx.h"
#include "x0x.h"

seq_t seq;

void seq_init(void) {
    memset(&seq, 0, sizeof seq);
    seq.bpm = 120;
    seq.key = -3; /* A: where acid lives */
    seq.cue = -1;
    for (int p = 0; p < NPATT; ++p)
        for (int t = 0; t < NPAD; ++t) {
            seq.pat[p].len[t] = BAR;
            for (int s = 0; s < NSTEP; ++s) {
                step_t *st = &seq.pat[p].s[t][s];
                st->level = st->pitch = st->decay = st->cutoff = UNSET;
            }
        }
}
int seq_step_frames(void) { return FS * 15 / seq.bpm; }
int seq_ticks(int rate) {
    static const unsigned char t[] = { 3, 4, 6, 8, 12, 24 };
    return t[rate - RATE_MIN];
}
const char *seq_rate_name(int rate) {
    static const char *const n[] = { "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4" };
    return n[rate - RATE_MIN];
}
static const signed char scales[NSCALE][7] = { { 0, 2, 3, 5, 7, 8, 10 }, { 0, 1, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 8, 11 },
                                               { 0, 3, 5, 7, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 5, 7, 9, 11 } }; /* TB-3PO's */
const char *seq_scale_name(int s) {
    static const char *const n[NSCALE] = { "Minor", "Phrygian", "Harm Min", "Min Pent", "Dorian", "Major" };
    return n[s];
}
int seq_scale_note(int i) {
    int n = seq.scale == 3 ? 5 : 7;
    return scales[seq.scale][i % n] + 12 * (i / n);
}
int seq_quantise(int pos) {
    int f = seq_step_frames();
    return (pos + f / 2) / f;
}
int seq_bars(const pattern_t *p) {
    int bars = 1;
    for (int t = 0; t < NPAD; ++t) {
        int any = 0;
        for (int s = 0; s < p->len[t]; ++s) any |= p->s[t][s].on;
        int b = any ? (p->len[t] * seq_ticks(p->rate[t]) + 95) / 96 : 1;
        if (b > bars) bars = b;
    }
    return bars;
}
int seq_song_frames(void) {
    int barf = seq_step_frames() * BAR, bars = 0;
    if (seq.nsong < 2) return seq_bars(&seq.pat[seq.cur]) * barf;
    for (int i = 0; i < seq.nsong; ++i) bars += seq_bars(&seq.pat[seq.song[i]]);
    return bars * barf;
}
static int onset(int s, int stepf, int nudge) {
    int o = s * stepf + (s & 1 ? stepf * seq.swing / 300 : 0);
    return o + nudge * stepf / 256;
}
static int track_onset(int s, int frames, int ticks, int nudge) { /* swing is the sixteenths' */
    return ticks == 6 ? onset(s, frames, nudge) : s * frames + nudge * frames / 256;
}
static int chance_ok(int t, int idx, int chance) { /* the same answer for every hit of a step */
    if (!chance) return 1;
    unsigned h = ((unsigned)t * 31 + (unsigned)idx) * 2654435761u ^ seq.bar * 2246822519u;
    h ^= h >> 15, h *= 2654435761u, h ^= h >> 13;
    return (int)((h >> 8 & 0xffff) % 100) < chance;
}
static void fire(int a, int b) { /* onsets in [a, b) of the current bar */
    int stepf = seq_step_frames();
    pattern_t *p = &seq.pat[seq.cur];
    for (int t = 0; t < NPAD; ++t) {
        if (sounds[t].src == SRC_BREAK) { /* its generator wants every sixteenth, heard or not */
            for (int s = 0; s < BAR; ++s) {
                int o = onset(s, stepf, 0), idx = (int)((seq.pbar * BAR + (unsigned)s) % p->len[t]);
                const step_t *st = &p->s[t][idx];
                if (o >= a && o < b) engine_break_step(t, s, seq.pbar, stepf, st->on && chance_ok(t, idx, st->chance), st->level, st->decay, st->cutoff);
            }
            continue;
        }
        int ticks = seq_ticks(p->rate[t]), n = 96 / ticks, frames = stepf * ticks / 6;
        for (int s = 0; s < n; ++s) {
            int idx = (int)((seq.pbar * (unsigned)n + (unsigned)s) % p->len[t]);
            const step_t *st = &p->s[t][idx];
            if (!st->on) continue;
            int o = track_onset(s, frames, ticks, st->nudge), hits = st->ratchet > 1 ? st->ratchet : 1;
            for (int h = 0; h < hits; ++h) {
                int oh = o + h * frames / hits;
                if (oh >= a && oh < b && chance_ok(t, idx, st->chance))
                    engine_note(t, st->level, st->pitch, st->decay, st->cutoff, st->flags, frames / hits);
            }
        }
    }
}
static void bar_end(void) {
    ++seq.bar, ++seq.pbar;
    if (seq.pbar % (unsigned)seq_bars(&seq.pat[seq.cur])) return;
    int was = seq.cur;
    if (seq.cue >= 0) seq.cur = seq.cue, seq.cue = -1;
    else if (seq.nsong > 1) seq.song_at = (seq.song_at + 1) % seq.nsong, seq.cur = seq.song[seq.song_at];
    if (seq.cur != was) seq.pbar = 0;
}
static int fx_at(int pos) { /* the current pattern's effect at pos frames into the bar, -1 none */
    const pattern_t *p = &seq.pat[seq.cur];
    int s = (int)(seq.pbar % (unsigned)seq_bars(p)) * BAR + pos / seq_step_frames();
    for (int i = p->nev - 1; i >= 0; --i)
        if (s >= p->ev[i].step && s < p->ev[i].step + p->ev[i].len) return p->ev[i].fx;
    return -1;
}
void seq_block(void) {
    fx_clock(seq.playing, seq.bpm, seq.pos % (seq_step_frames() * 4));
    if (!seq.playing) return;
    fx_seq(fx_at(seq.pos));
    int barf = seq_step_frames() * BAR, end = seq.pos + CTL;
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
    seq.bar = seq.pbar = 0;
    if (on && seq.cue >= 0) seq.cur = seq.cue, seq.cue = -1;
    if (on && seq.nsong > 1) { /* from this pattern's place in the song, or the top */
        seq.song_at = 0;
        for (int i = seq.nsong - 1; i >= 0; --i) if (seq.song[i] == seq.cur) seq.song_at = i;
        seq.cur = seq.song[seq.song_at];
    }
    if (!on) fx_seq(-1), engine_transport_stop();
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
void seq_note(int pad, int pitch) {
    if (!seq.playing || !seq.rec) { engine_note(pad, UNSET, pitch, UNSET, UNSET, 0, -1); return; }
    pattern_t *p = &seq.pat[seq.cur];
    int ticks = seq_ticks(p->rate[pad]), steps = 96 / ticks, frames = seq_step_frames() * ticks / 6;
    int n = (seq.pos + frames / 2) / frames, idx = (int)((seq.pbar * (unsigned)steps + (unsigned)n) % p->len[pad]);
    step_t *st = &p->s[pad][idx];
    if (!st->on && seq.ntake < (int)(sizeof seq.take / sizeof *seq.take)) {
        seq.take[seq.ntake].pat = (signed char)seq.cur;
        seq.take[seq.ntake].pad = (signed char)pad;
        seq.take[seq.ntake].step = (signed char)idx;
        ++seq.ntake;
    }
    st->on = 1;
    if (pitch != UNSET) st->pitch = (signed char)pitch;
    if (n >= steps || track_onset(n, frames, ticks, st->nudge) <= seq.pos) /* its onset has passed: sound it now */
        engine_note(pad, UNSET, pitch, UNSET, UNSET, 0, frames);
}
void seq_tap(int pad) { seq_note(pad, UNSET); }
void seq_fx_start(int fx) {
    pattern_t *p = &seq.pat[seq.cur];
    if (!seq.playing || !seq.rec || p->nev >= NFXEV) return;
    int n = seq_quantise(seq.pos) + (int)(seq.pbar % (unsigned)seq_bars(p)) * BAR;
    if (seq.ntake < (int)(sizeof seq.take / sizeof *seq.take)) {
        seq.take[seq.ntake].pat = (signed char)seq.cur;
        seq.take[seq.ntake].pad = -1;
        seq.take[seq.ntake].step = (signed char)p->nev;
        ++seq.ntake;
    }
    p->ev[p->nev].step = (signed char)(n % NSTEP);
    p->ev[p->nev].len = 1;
    p->ev[p->nev].fx = (signed char)fx;
    seq.fxrec.on = 1, seq.fxrec.pat = seq.cur, seq.fxrec.step = p->nev, seq.fxrec.start = seq_quantise(seq.pos), seq.fxrec.bar = seq.bar;
    ++p->nev;
}
int seq_fx_end(void) {
    if (!seq.fxrec.on) return 0;
    seq.fxrec.on = 0;
    if (!seq.playing || seq.fxrec.step >= seq.pat[seq.fxrec.pat].nev) return 1; /* stopped, or undone meanwhile */
    fxev_t *e = &seq.pat[seq.fxrec.pat].ev[seq.fxrec.step];
    int len = (int)(seq.bar - seq.fxrec.bar) * BAR + seq_quantise(seq.pos) - seq.fxrec.start, room = NSTEP - e->step;
    e->len = (signed char)(len < 1 ? 1 : len > room ? room : len);
    return 1;
}
void seq_undo(void) {
    if (!seq.ntake && seq.gen.valid) { /* nothing recorded: a generated line goes back */
        memcpy(seq.pat[seq.gen.pat].s[seq.gen.pad], seq.gen.s, sizeof seq.gen.s);
        seq.gen.valid = 0;
        return;
    }
    while (seq.ntake > 0) {
        --seq.ntake;
        int pat = seq.take[seq.ntake].pat, pad = seq.take[seq.ntake].pad, step = seq.take[seq.ntake].step;
        if (pad < 0) seq.pat[pat].nev = step;
        else seq.pat[pat].s[pad][step].on = 0, seq.pat[pat].s[pad][step].pitch = UNSET;
    }
}
int seq_playhead(int track) {
    if (!seq.playing) return -1;
    const pattern_t *p = &seq.pat[seq.cur];
    int ticks = seq_ticks(p->rate[track]), steps = 96 / ticks, frames = seq_step_frames() * ticks / 6, s = seq.pos / frames;
    if (s >= steps) s = steps - 1;
    return (int)((seq.pbar * (unsigned)steps + (unsigned)s) % p->len[track]);
}
void seq_song_add(int pattern) {
    if (seq.nsong >= NSONG) return;
    if (!seq.nsong && pattern != seq.cur) seq.song[seq.nsong++] = seq.cur;
    seq.song[seq.nsong++] = pattern;
}
void seq_song_pop(void) { if (seq.nsong) --seq.nsong; if (seq.nsong < 2) seq.nsong = 0; }

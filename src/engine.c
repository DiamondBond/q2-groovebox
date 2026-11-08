/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sampler. Each voice reads its sample at a 16.16 position with linear interpolation, through
 * an AD envelope and a state-variable low-pass, into a stereo Q15 accumulator; the mix takes the
 * master gain, PLAY's filter sweep and a limiter. Levels taper as level², so the top half of the
 * range is where the ear expects it. ponytail: triggers land at block starts (0.7 ms); echo and
 * reverb sends are stored but not yet rendered. */
#include <math.h>
#include <string.h>
#include "engine.h"

sound_t sounds[NPAD];
const char *const param_names[P_N] = { "Level", "Pan",    "Pitch",  "Fine",  "Start", "End",
                                       "Loop",  "Reverse", "Attack", "Decay", "Cutoff", "Reso",
                                       "Echo",  "Reverb",  "Choke",  "Load" };
const int param_min[P_N] = { 0, -64, -24, -50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
const int param_max[P_N] = { 127, 63, 24, 50, 127, 127, 1, 1, 127, 127, 127, 127, 127, 127, 15, 0 };
const int param_def[P_N] = { 100, 0, 0, 0, 0, 127, 0, 0, 0, 127, 127, 0, 0, 0, 0, 0 };

typedef struct {
    int on, pad;
    unsigned pos, end, start; /* 16.16 frames into the sample */
    int inc, loop, reverse, gl, gr; /* gains Q15 */
    float env, att_step, dec_k;     /* attack adds, decay multiplies, per frame */
    float f, q, lp, bp;             /* the voice's filter */
} voice_t;
static voice_t voices[NPAD];
static struct {
    int gain, sweep;
    float sf, sq, lp[2], bp[2]; /* the master sweep's filter, a channel each */
    float lim;                  /* the limiter's gain, 0..1 */
    int peak;
    unsigned blocks, hit[NPAD];
} eg;

int bt_gain(int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    return 65536 * (v < 50 ? 2 * v : 18 * v - 800) / 1000;
}

void engine_init(void) {
    for (int i = 0; i < NPAD; ++i)
        for (int k = 0; k < P_N; ++k) sounds[i].p[k] = param_def[k];
    memset(voices, 0, sizeof voices);
    memset(&eg, 0, sizeof eg);
    eg.gain = 65536;
    eg.lim = 1;
}
void engine_gain(int q16) { eg.gain = q16; }
int engine_peak(void) { return eg.peak; }
unsigned engine_blocks(void) { return eg.blocks; }
unsigned engine_last_hit(int pad) { return eg.hit[pad]; }

/* a cutoff 0..127 as the SVF's frequency coefficient: 40 Hz to the top, in octaves */
static float svf_f(int cutoff) {
    float hz = 40 * exp2f(cutoff * (9.0f / 127));
    float f = 2 * sinf(3.14159265f * hz / FS);
    return f > 1.2f ? 1.2f : f;
}
void engine_sweep(int v) {
    eg.sweep = v < -100 ? -100 : v > 100 ? 100 : v;
    int a = eg.sweep < 0 ? -eg.sweep : eg.sweep;
    /* low-pass: 20 kHz down to 100 Hz; high-pass: 20 Hz up to 5 kHz */
    float hz = eg.sweep < 0 ? 20000 * exp2f(-a * (7.6f / 100)) : 20 * exp2f(a * (8.0f / 100));
    float f = 2 * sinf(3.14159265f * hz / FS);
    eg.sf = f > 1.2f ? 1.2f : f;
    eg.sq = 0.9f;
}

void engine_trigger(int pad, int level, int pitch, int decay, int cutoff) {
    sound_t *s = &sounds[pad];
    if (!s->smp.pcm || s->smp.n < 2) return;
    if (s->p[P_CHOKE])
        for (int i = 0; i < NPAD; ++i)
            if (i != pad && voices[i].on && sounds[i].p[P_CHOKE] == s->p[P_CHOKE]) voices[i].on = 0;
    voice_t *v = &voices[pad];
    int lv = level == UNSET ? s->p[P_LEVEL] : level;
    int semis = (pitch == UNSET ? 0 : pitch) + s->p[P_PITCH];
    int dec = decay == UNSET ? s->p[P_DECAY] : decay, cut = cutoff == UNSET ? s->p[P_CUTOFF] : cutoff;
    unsigned n = (unsigned)s->smp.n;
    v->pad = pad;
    v->start = (unsigned)((unsigned long long)n * (unsigned)s->p[P_START] / 127) << 16;
    v->end = (unsigned)((unsigned long long)n * (unsigned)s->p[P_END] / 127) << 16;
    if (v->end <= v->start + (2u << 16)) v->end = v->start + (2u << 16);
    if (v->end > (n - 1) << 16) v->end = (n - 1) << 16;
    v->pos = v->start;
    v->inc = (int)(65536 * exp2f((semis + s->p[P_FINE] * 0.01f) / 12));
    v->loop = s->p[P_LOOP];
    v->reverse = s->p[P_REVERSE];
    int amp = lv * lv * 32767 / (127 * 127), pan = s->p[P_PAN]; /* -64..63 */
    v->gl = amp * (63 - pan) / 127;
    v->gr = amp * (64 + pan) / 127;
    int att = s->p[P_ATTACK];
    float att_s = att * att * (1.0f / (127 * 127)); /* up to a second */
    v->att_step = att_s > 0 ? 1 / (att_s * FS) : 2;
    v->env = att ? 0 : 1;
    float dec_s = dec * dec * (4.0f / (127 * 127)); /* up to four seconds; 127 is the sample's own */
    v->dec_k = dec >= 127 ? 1 : expf(-1 / (dec_s * FS + 1));
    v->f = cut >= 127 ? 0 : svf_f(cut);
    v->q = 1.0f - s->p[P_RESO] * (0.93f / 127);
    v->lp = v->bp = 0;
    v->on = 1;
    eg.hit[pad] = eg.blocks;
}

static void voice_block(voice_t *v, int *acc) {
    const short *pcm = sounds[v->pad].smp.pcm;
    unsigned n = (unsigned)sounds[v->pad].smp.n;
    for (int i = 0; i < CTL; ++i) {
        if (v->pos >= v->end) {
            if (!v->loop) { v->on = 0; return; }
            v->pos = v->start + (v->pos - v->end);
        }
        unsigned idx = v->pos >> 16, frac = v->pos & 0xffff;
        unsigned i0 = v->reverse ? n - 1 - idx : idx, i1 = v->reverse ? (i0 ? i0 - 1 : 0) : i0 + 1;
        int s0 = pcm[i0], s1 = pcm[i1 < n ? i1 : n - 1];
        float s = (float)(s0 + (((s1 - s0) * (int)frac) >> 16));
        if (v->env < 1 && v->att_step < 2) { v->env += v->att_step; if (v->env > 1) v->env = 1; }
        else v->env *= v->dec_k;
        s *= v->env;
        if (v->f > 0) { /* Chamberlin: low-pass out */
            v->lp += v->f * v->bp;
            float hp = s - v->lp - v->q * v->bp;
            v->bp += v->f * hp;
            s = v->lp;
        }
        int si = (int)s;
        acc[2 * i] += si * v->gl >> 15;
        acc[2 * i + 1] += si * v->gr >> 15;
        v->pos += (unsigned)v->inc;
        if (v->env < 0.0005f && v->dec_k < 1) { v->on = 0; return; }
    }
}

void engine_block(int *out) {
    memset(out, 0, CTL * 2 * sizeof *out);
    for (int i = 0; i < NPAD; ++i)
        if (voices[i].on) voice_block(&voices[i], out);
    int a = eg.sweep < 0 ? -eg.sweep : eg.sweep, peak = 0;
    for (int i = 0; i < CTL * 2; ++i) {
        float s = (float)out[i];
        if (a > 2) { /* the master sweep, a filter a channel */
            float *lp = &eg.lp[i & 1], *bp = &eg.bp[i & 1];
            *lp += eg.sf * *bp;
            float hp = s - *lp - eg.sq * *bp;
            *bp += eg.sf * hp;
            s = eg.sweep < 0 ? *lp : hp;
        }
        int v = (int)((long long)(int)s * eg.gain >> 16);
        out[i] = v;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    /* the limiter: the block's peak pulls the gain down at once, it eases back up */
    float want = peak > 32000 ? 32000.0f / (float)peak : 1;
    eg.lim = want < eg.lim ? want : eg.lim + (1 - eg.lim) * 0.02f;
    for (int i = 0; i < CTL * 2; ++i) {
        int v = (int)((float)out[i] * eg.lim);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    eg.peak = (int)((float)peak * eg.lim);
    if (eg.peak > 32767) eg.peak = 32767;
    ++eg.blocks;
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The sampler. Each voice reads its sample at a 16.16 position with linear interpolation, through
 * an AD envelope and a state-variable low-pass, into a stereo Q15 accumulator and the echo and
 * reverb sends; the mix takes the buses (fx.c), PLAY's filter sweep, the punch-in effect, the master
 * gain and a limiter. Levels taper as level², so the top half of the range is where the ear expects
 * it. ponytail: triggers land at block starts (0.7 ms). */
#include <math.h>
#include <string.h>
#include "engine.h"
#include "fx.h"

sound_t sounds[NPAD];
const char *const param_names[P_N] = { "Level", "Pan",    "Pitch",  "Fine",  "Start", "End",
                                       "Loop",  "Reverse", "Attack", "Decay", "Cutoff", "Reso",
                                       "Echo",  "Reverb",  "Choke",  "Load" };
const int param_min[P_N] = { 0, -64, -24, -50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
const int param_max[P_N] = { 127, 63, 24, 50, 127, 127, 1, 1, 127, 127, 127, 127, 127, 127, 15, 0 };
const int param_def[P_N] = { 100, 0, 0, 0, 0, 127, 0, 0, 0, 127, 127, 0, 0, 0, 0, 0 };

typedef struct {
    int on, pad;
    unsigned long long pos, end, start; /* 16.16 frames into the sample, wide enough past 65535 frames */
    int inc, loop, reverse, gl, gr, ge, gv; /* gains Q15: left, right, the echo and reverb sends */
    float env, att_step, dec_k;     /* attack adds, decay multiplies, per frame */
    float f, q, lp, bp;             /* the voice's filter */
} voice_t;
static voice_t voices[NPAD];
static struct {
    int gain, sweep, offline;
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
    fx_init();
}
void engine_gain(int q16) { eg.gain = q16; }
int engine_peak(void) { return eg.peak; }
unsigned engine_blocks(void) { return eg.blocks; }
unsigned engine_last_hit(int pad) { return eg.hit[pad]; }
void engine_stop(int pad) { voices[pad].on = 0; }
int engine_is_offline(void) { return eg.offline; }
void engine_offline(int on) { /* a clean slate either way; on, the file gets no soft volume or sweep */
    static int gain, sweep;
    if (on) gain = eg.gain, sweep = eg.sweep, eg.gain = 65536, eg.sweep = 0;
    else eg.gain = gain, eg.sweep = sweep;
    memset(voices, 0, sizeof voices);
    fx_init();
    eg.lim = 1;
    eg.offline = on;
}

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
    int semis = pitch == UNSET ? s->p[P_PITCH] : pitch; /* a lock is the pitch itself, as WRITE shows it */
    int dec = decay == UNSET ? s->p[P_DECAY] : decay, cut = cutoff == UNSET ? s->p[P_CUTOFF] : cutoff;
    unsigned n = (unsigned)s->smp.n;
    v->pad = pad;
    v->start = ((unsigned long long)n * (unsigned)s->p[P_START] / 127) << 16;
    v->end = ((unsigned long long)n * (unsigned)s->p[P_END] / 127) << 16;
    if (v->end > (unsigned long long)(n - 1) << 16) v->end = (unsigned long long)(n - 1) << 16;
    if (v->end < 1ull << 16) v->end = 1ull << 16; /* End 0 still has a frame */
    if (v->start + (1ull << 16) >= v->end) v->start = v->end - (1ull << 16); /* at least a frame */
    v->pos = v->start;
    v->inc = (int)(65536 * exp2f((semis + s->p[P_FINE] * 0.01f) / 12));
    v->loop = s->p[P_LOOP];
    v->reverse = s->p[P_REVERSE];
    int amp = lv * lv * 32767 / (127 * 127), pan = s->p[P_PAN]; /* -64..63 */
    v->gl = amp * (63 - pan) / 127;
    v->gr = amp * (64 + pan) / 127;
    v->ge = amp * s->p[P_ECHO] / 127;
    v->gv = amp * s->p[P_REVERB] / 127;
    int att = s->p[P_ATTACK];
    float att_s = att * att * (1.0f / (127 * 127)); /* up to a second */
    v->att_step = att_s > 0 ? 1 / (att_s * FS) : 2; /* 2: no attack left */
    v->env = att ? 0 : 1;
    float dec_s = dec * dec * (4.0f / (127 * 127)); /* up to four seconds; 127 is the sample's own */
    v->dec_k = dec >= 127 ? 1 : expf(-1 / (dec_s * FS + 1));
    v->f = cut >= 127 ? 0 : svf_f(cut);
    v->q = 1.0f - s->p[P_RESO] * (0.93f / 127);
    v->lp = v->bp = 0;
    v->on = 1;
    eg.hit[pad] = eg.blocks;
}

static void voice_block(voice_t *v, int *l, int *r, int *dly, int *rev) {
    const short *pcm = sounds[v->pad].smp.pcm;
    unsigned n = (unsigned)sounds[v->pad].smp.n, ends = (unsigned)(v->start >> 16) + (unsigned)(v->end >> 16);
    for (int i = 0; i < CTL; ++i) {
        if (v->pos >= v->end) {
            if (!v->loop) { v->on = 0; return; }
            v->pos = v->start + (v->pos - v->end) % (v->end - v->start);
        }
        unsigned idx = (unsigned)(v->pos >> 16), frac = (unsigned)(v->pos & 0xffff);
        unsigned i0 = v->reverse ? ends - idx : idx, /* Reverse mirrors within the region */
                  i1 = v->reverse ? (i0 ? i0 - 1 : 0) : i0 + 1;
        int s0 = pcm[i0], s1 = pcm[i1 < n ? i1 : n - 1];
        float s = (float)(s0 + (int)(((long long)(s1 - s0) * (int)frac) >> 16));
        if (v->att_step < 2) { /* the attack, then the decay */
            v->env += v->att_step;
            if (v->env >= 1) v->env = 1, v->att_step = 2;
        } else
            v->env *= v->dec_k;
        s *= v->env;
        if (v->f > 0) { /* Chamberlin: low-pass out */
            v->lp += v->f * v->bp;
            float hp = s - v->lp - v->q * v->bp;
            v->bp += v->f * hp;
            s = v->lp;
        }
        int si = (int)s;
        l[i] += (int)((long long)si * v->gl >> 15); /* a resonant peak can top full scale */
        r[i] += (int)((long long)si * v->gr >> 15);
        if (v->ge) dly[i] += (int)((long long)si * v->ge >> 15);
        if (v->gv) rev[i] += (int)((long long)si * v->gv >> 15);
        v->pos += (unsigned)v->inc;
        if (v->att_step >= 2 && v->env < 0.0005f && v->dec_k < 1) { v->on = 0; return; }
    }
}

static void sweep_channel(int *b, int ch) { /* the master sweep */
    float lp = eg.lp[ch], bp = eg.bp[ch];
    for (int i = 0; i < CTL; ++i) {
        float x = (float)b[i];
        lp += eg.sf * bp;
        float hp = x - lp - eg.sq * bp;
        bp += eg.sf * hp;
        b[i] = (int)(eg.sweep < 0 ? lp : hp);
    }
    eg.lp[ch] = lp, eg.bp[ch] = bp;
}

void engine_block(int *out) {
    int l[CTL] = { 0 }, r[CTL] = { 0 }, dly[CTL] = { 0 }, rev[CTL] = { 0 }, peak = 0;
    for (int i = 0; i < NPAD; ++i)
        if (voices[i].on) voice_block(&voices[i], l, r, dly, rev);
    fx_buses_block(l, r, dly, rev);
    if (eg.sweep > 2 || eg.sweep < -2) sweep_channel(l, 0), sweep_channel(r, 1);
    fx_punch_block(l, r);
    for (int i = 0; i < CTL; ++i) {
        out[2 * i] = (int)((long long)l[i] * eg.gain >> 16);
        out[2 * i + 1] = (int)((long long)r[i] * eg.gain >> 16);
    }
    for (int i = 0; i < CTL * 2; ++i) {
        int v = out[i] < 0 ? -out[i] : out[i];
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

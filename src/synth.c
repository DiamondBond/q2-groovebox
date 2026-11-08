/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The host Felucca-Engines asks for: one voice a part, the part a pad, and Felucca's own envelope
 * law, as the presets were voiced against it. The sequencer has one voice a track and the panel one
 * finger, so a pad has no polyphony to lose. Its own translation unit: the engines' names are short. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "synth.h"
#undef W
#undef H
#define NVOICE 1
#define NPART NPAD
#include "felucca/fe_tables.h"
#include "felucca/felucca_engine.h"
enum { P_ATK, P_DEC, P_SUS, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_FENV, P_COUNT };
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
} track_t;
static track_t trk[NPART];
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "felucca/dsp.c"
#include "felucca/eng_analog.c"
#include "felucca/eng_lofi.c"
#include "felucca/eng_trio.c"
#include "felucca/eng_wheel.c"
#include "felucca/eng_noise.c"
#include "felucca/eng_phase.c"

#define LEVEL 0.45f /* one voice of a part made for eight, about as loud as a kit's drum */
static const engine_t *const ENG[SYNTH_ENGINES] = { &ENG_ANALOG, &ENG_LOFI, &ENG_TRIO, &ENG_WHEEL, &ENG_NOISE, &ENG_PHASE };
static uint32_t env_lin[128], env_exp[128], vage;

void synth_init(void) {
    for (int v = 0; v < 128; ++v) { /* tools/gen_tables.py's, Felucca's: 1 ms to 10 s */
        double ms = pow(10000, v / 127.0), k = 65536 * (1 - exp(-CTL / (ms / 1000 / 4.6 * FS)));
        double lin = (1 << 24) * (double)CTL / (ms * FS / 1000);
        env_lin[v] = lin < 1 ? 1 : (uint32_t)lin;
        env_exp[v] = k < 1 ? 1 : k > 65535 ? 65535 : (uint32_t)k;
    }
    memset(trk, 0, sizeof trk);
}
const char *synth_engine(int e) { return ENG[e]->name; }
int synth_presets(int e) { return ENG[e]->npresets; }
const char *synth_preset_name(int e, int p) { return ENG[e]->presets[p].name; }
void synth_preset(int e, int p, int *vals, int *echo, int *reverb) {
    const preset_t *r = &ENG[e]->presets[p];
    for (int k = 0; k < 8; ++k) vals[k] = r->e[k];
    for (int k = 0; k < 4; ++k) vals[SY_ATK + k] = r->env[k];
    vals[SY_FENV] = r->fenv;
    *echo = r->fx[2] ? r->fx[2] - 1 : 0;
    *reverb = r->fx[3] ? r->fx[3] - 1 : 0;
}
void synth_apply(int pad, const int *vals) {
    track_t *t = &trk[pad];
    for (int k = 0; k < 8; ++k) t->p[P_E0 + k] = (int16_t)vals[k];
    t->p[P_ATK] = (int16_t)vals[SY_ATK], t->p[P_DEC] = (int16_t)vals[SY_DEC];
    t->p[P_SUS] = (int16_t)vals[SY_SUS], t->p[P_REL] = (int16_t)vals[SY_REL];
    t->p[P_FENV] = (int16_t)vals[SY_FENV];
}
void synth_on(int pad, int e, int note, int vel, int fine, int slide) {
    track_t *t = &trk[pad];
    const engine_t *en = ENG[e];
    voice_t *v = &t->v[0];
    int sounding = v->active && v->stage != 0;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    v->fine = fine;
    v->pitch16 = note * 16;
    if (slide && sounding) return; /* legato: the envelope runs on, the pitch glides */
    uint32_t ph[3];
    int32_t s[8];
    memcpy(ph, v->ph, sizeof ph);
    memcpy(s, v->s, sizeof s);
    v->active = 1;
    v->stage = 1;
    v->age = ++vage;
    v->pitch_cur = v->pitch16;
    if (!sounding) v->env = v->env_out = 0;
    en->note_on(t, v);
    if (sounding) { /* a retrigger keeps the phases and the slots it names */
        memcpy(v->ph, ph, sizeof ph);
        for (uint32_t k = 0; k < 8; ++k)
            if (k != 2 && k != 3 && (en->keep >> k & 1)) v->s[k] = s[k];
    }
}
void synth_off(int pad) {
    voice_t *v = &trk[pad].v[0];
    v->gate = 0;
    if (v->active && v->stage < 3) v->stage = 3;
}
void synth_kill(int pad) {
    voice_t *v = &trk[pad].v[0];
    v->active = v->gate = 0, v->stage = 0, v->env = v->env_out = 0;
}

static int32_t env_tick(track_t *t, voice_t *v) { /* Felucca's voice.c: Q24 in, Q15 out */
    const int16_t *p = t->p;
    int32_t sus = (int32_t)p[P_SUS] << 17;
    switch (v->stage) {
    case 1:
        v->env += (int32_t)env_lin[p[P_ATK] & 127];
        if (v->env >= 1 << 24) v->env = 1 << 24, v->stage = 2;
        break;
    case 2: v->env += (int32_t)(((int64_t)(sus - v->env) * env_exp[p[P_DEC] & 127]) >> 16); break;
    case 3:
        v->env -= (int32_t)(((int64_t)v->env * env_exp[p[P_REL] & 127]) >> 16);
        if (v->env < 1 << 12) v->env = 0, v->stage = 0, v->active = 0;
        break;
    default: v->env = 0;
    }
    return v->env >> 9;
}
int synth_fill(int pad, int e, float *s) {
    track_t *t = &trk[pad];
    const engine_t *en = ENG[e];
    voice_t *v = &t->v[0];
    int32_t out[CTL];
    memset(out, 0, sizeof out);
    if (en->block) en->block(t);
    if (v->active) {
        int32_t env = env_tick(t, v);
        if (en->amp) env = en->amp(t, v, env);
        if (v->active) {
            vmod_t m;
            memset(&m, 0, sizeof m);
            m.envq15 = env;
            m.amp1 = mulq15(env, v->vel * 258);
            m.amp0 = v->env_out;
            v->env_out = m.amp1;
            if (v->pitch_cur != v->pitch16) { /* the slide: ~45 ms for any interval's first octave */
                int32_t d = v->pitch16 - v->pitch_cur, st = (int32_t)(env_lin[40] >> 14);
                if (st < 1) st = 1;
                v->pitch_cur += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
            }
            m.pitch16 = clamp(v->pitch_cur, 0, 2047);
            m.inc = pitch_inc((uint32_t)m.pitch16);
            m.fine = v->fine;
            if (v->fine) m.inc += (uint32_t)((int32_t)(m.inc >> 12) * v->fine);
            m.cutoff = (env * t->p[P_FENV]) >> 7;
            if (v->vel > 110) m.cutoff += (env * 24) >> 7; /* an accent opens the filter with the envelope */
            m.shape = 64 << 8;
            en->render(t, v, out, CTL, &m);
        }
    }
    for (int i = 0; i < CTL; ++i) s[i] = (float)out[i] * LEVEL;
    return v->active;
}
int synth_tile(int pad, int e, int i, tile_t *t) {
    static const char *const env_names[5] = { "Attack", "Dec", "Sus", "Rel", "FEnv" };
    const engine_t *en = ENG[e];
    memset(t, 0, sizeof *t);
    if (i < 8) {
        const param_desc_t *d = en->desc ? en->desc(&trk[pad], (uint32_t)i) : &en->edit[i];
        if (!d) d = &en->edit[i];
        t->name = d->label, t->min = d->min, t->max = d->max;
        t->names = d->fmt == F_ENUM ? d->names : 0;
    } else if (i <= SY_REL) t->name = env_names[i - SY_ATK], t->max = 127;
    else if (i == SY_FENV) t->name = env_names[4], t->min = -64, t->max = 63;
    else if (i == SY_PRESET) t->name = "Preset", t->max = en->npresets - 1;
    else if (i == SY_NEW) t->name = "New", t->min = 5, t->max = 100, t->action = ACT_NEW;
    else if (i == SY_MUTATE) t->name = "Mutate", t->action = ACT_MUTATE;
    else return 0;
    return 1;
}

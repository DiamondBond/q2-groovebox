/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * X0X's DSP, float and mono, behind the pads: a machine of its own for each pad, so a pad has its own
 * settings and two kicks can differ, with its output through the pad's level, filter and sends like
 * any other sound. The machine's own Level, Pan and sends are left at theirs and not shown. */
#include <stdlib.h>
#include <string.h>
#include "x0x.h"
#include "ui.h"
#undef W
#undef H
#include "x0x/dsp/drum909.h"
#include "x0x/dsp/drum808.h"
#include "x0x/dsp/bass303.h"
#include "x0x/dsp/breaks.h"
#include "x0x/dsp/fxbus.h"
#include "x0x/seq/tb3po.h"

#define BRK_PLAIN 12 /* the pattern settings, then Pitch, Filter and Bars */
#define T_PITCH 12
#define T_FILTER 13
#define T_BARS 14
#define BUS_DLY 88200
static union { drum909_t d9; drum808_t d8; bass303_t b3; breaks_t br; } inst[NPAD];
static uint8_t *brk_mem[NPAD];
static int brk_frames[NPAD], tuned[NPAD];
static fxbus_t bus;
static int16_t bus_dly[BUS_DLY];
static int bus_x0x, bus_pot[BUS_TILES];
static const signed char bus_map[BUS_TILES] = { -1, FX_DIST, FX_DRIVE, FX_COMP, FX_RV_DECAY, FX_RV_TONE, FX_RV_HPF, FX_RV_LEVEL,
                                                FX_DL_TIME, FX_DL_FDBK, FX_DL_TONE, FX_DL_LEVEL, FX_DL_HPF, FX_DL_TYPE, FX_DL_WEAR, FX_DL_PING };

/* a machine's knob by SOUND 2's place: the pad's Level, Pan and sends do those jobs */
static int shown(const char *n) { return strcmp(n, "Level") && strcmp(n, "Pan") && strcmp(n, "Rev") && strcmp(n, "Dly") && strcmp(n, "Volume"); }
static int knobs(int src, int sub) { return src == SRC_909 ? drum909_nparams(sub) : src == SRC_808 ? drum808_nparams(sub) : bass303_nparams(); }
static const x0x_param_t *knob(int src, int sub, int k) { return src == SRC_909 ? drum909_param(sub, k) : src == SRC_808 ? drum808_param(sub, k) : bass303_param(k); }
static int knob_at(int src, int sub, int tile) {
    for (int k = 0, n = knobs(src, sub), t = 0; k < n; ++k)
        if (shown(knob(src, sub, k)->name) && t++ == tile) return k;
    return -1;
}
int x0x_tile(int src, int sub, int i, tile_t *t) {
    memset(t, 0, sizeof *t);
    if (src == SRC_BREAK) {
        static const char *const pitch_name = "Pitch", *const filt_name = "Filter", *const bars_name = "Bars";
        if (i == T_BARS) { t->name = bars_name, t->min = 1, t->max = 8; return 1; }
        int k = i < BRK_PLAIN ? i : i == T_PITCH ? BRK_PITCH : i == T_FILTER ? BRK_FILT : -1;
        if (k < 0) return 0;
        const x0x_param_t *p = breaks_param(k);
        t->name = i == T_PITCH ? pitch_name : i == T_FILTER ? filt_name : p->name, t->max = p->max, t->names = p->names;
        return 1;
    }
    if (src == SRC_303 && i >= 14) {
        t->name = i == 14 ? "New" : "Mutate", t->action = i == 14 ? ACT_NEW : ACT_MUTATE;
        if (i == 14) t->min = 5, t->max = 100;
        return 1;
    }
    int k = knob_at(src, sub, i);
    if (k < 0) return 0;
    const x0x_param_t *p = knob(src, sub, k);
    t->name = p->name, t->max = p->max, t->names = p->names;
    return 1;
}
void x0x_defaults(int src, int sub, int *e) {
    memset(e, 0, NTILE * sizeof *e);
    if (src == SRC_BREAK) {
        for (int i = 0; i < BRK_PLAIN; ++i) e[i] = breaks_param(i)->def;
        e[T_PITCH] = breaks_param(BRK_PITCH)->def, e[T_FILTER] = breaks_param(BRK_FILT)->def, e[T_BARS] = 2;
        e[0] = 50; /* moving, from the start */
        return;
    }
    for (int i = 0; i < NTILE; ++i) {
        int k = knob_at(src, sub, i);
        if (k >= 0) e[i] = knob(src, sub, k)->def;
    }
    if (src == SRC_303) e[14] = 70; /* New's density */
}
/* the kits as kits/909 and kits/808 were rendered (tools/x0x_render.c), hats on one choke */
void x0x_kit(int src, int pad, int *sub, int *e, const char **name, int *choke) {
    static const struct { int v; const char *name; const char *knob; int value; } K[2][NPAD] = {
        { { DR_BD, "Kick", 0, 0 }, { DR_SD, "Snare", 0, 0 }, { DR_CH, "Hat", 0, 0 }, { DR_OH, "Open Hat", 0, 0 }, { DR_CP, "Clap", 0, 0 },
          { DR_LT, "Tom Lo", 0, 0 }, { DR_MT, "Tom Mid", 0, 0 }, { DR_HT, "Tom Hi", 0, 0 }, { DR_RS, "Rim", 0, 0 }, { DR_CR, "Crash", 0, 0 },
          { DR_RD, "Ride", 0, 0 }, { DR_BD, "Kick Long", "Decay", 118 }, { DR_BD, "Kick Tight", "Decay", 40 },
          { DR_SD, "Snare Hi", "Tune", 110 }, { DR_CP, "Clap Tail", "Tail", 120 }, { DR_CH, "Hat Tight", "Decay", 30 } },
        { { D8_BD, "Kick", 0, 0 }, { D8_SD, "Snare", 0, 0 }, { D8_CH, "Hat", 0, 0 }, { D8_OH, "Open Hat", 0, 0 }, { D8_CP, "Clap", 0, 0 },
          { D8_LT, "Tom Lo", 0, 0 }, { D8_MT, "Tom Mid", 0, 0 }, { D8_HT, "Tom Hi", 0, 0 }, { D8_RS, "Rim", 0, 0 }, { D8_CY, "Cymbal", 0, 0 },
          { D8_CB, "Cowbell", 0, 0 }, { D8_RS, "Clave", "Sound", 1 }, { D8_CP, "Maracas", "Sound", 1 },
          { D8_LT, "Conga Lo", "Sound", 1 }, { D8_MT, "Conga Mid", "Sound", 1 }, { D8_HT, "Conga Hi", "Sound", 1 } },
    };
    const int m = src == SRC_808;
    *sub = K[m][pad].v, *name = K[m][pad].name;
    *choke = !strncmp(*name, "Hat", 3) || !strcmp(*name, "Open Hat") ? 1 : 0;
    x0x_defaults(src, *sub, e);
    if (K[m][pad].knob)
        for (int i = 0; i < NTILE; ++i) {
            tile_t t;
            if (x0x_tile(src, *sub, i, &t) && !strcmp(t.name, K[m][pad].knob)) e[i] = K[m][pad].value;
        }
}
static void pot(int pad, int src, int sub, int k, int v) {
    if (src == SRC_909) drum909_set(&inst[pad].d9, sub, k, v);
    else if (src == SRC_808) drum808_set(&inst[pad].d8, sub, k, v);
    else bass303_set(&inst[pad].b3, k, v);
}
void x0x_set(int pad, int src, int sub, int tile, int v) {
    if (src == SRC_BREAK) {
        breaks_t *b = &inst[pad].br;
        if (tile == T_BARS) { b->bank[0].sp16 = (float)brk_frames[pad] / (float)(v * 16); return; }
        breaks_set(b, tile < BRK_PLAIN ? tile : tile == T_PITCH ? BRK_PITCH : BRK_FILT, v);
        return;
    }
    int k = knob_at(src, sub, tile);
    if (k >= 0) pot(pad, src, sub, k, v);
}
static void volume_up(int pad, int src, int sub) { /* the pad's Level is the only fader */
    for (int k = 0, n = knobs(src, sub); k < n; ++k)
        if (src == SRC_303 && !strcmp(knob(src, sub, k)->name, "Volume")) pot(pad, src, sub, k, knob(src, sub, k)->max);
}
void x0x_assign(int pad, int src, int sub, const int *e) {
    if (src == SRC_909) drum909_init(&inst[pad].d9);
    else if (src == SRC_808) drum808_init(&inst[pad].d8);
    else if (src == SRC_303) bass303_init(&inst[pad].b3);
    else return;
    volume_up(pad, src, sub);
    for (int i = 0; i < NTILE; ++i) {
        tile_t t;
        if (x0x_tile(src, sub, i, &t) && !t.action) x0x_set(pad, src, sub, i, e[i]);
    }
    tuned[pad] = 0;
}
static void tune_by(int pad, int src, int sub, int semis) {
    if (semis == tuned[pad]) return;
    tuned[pad] = semis;
    for (int i = 0; i < NTILE; ++i) {
        int k = knob_at(src, sub, i);
        if (k < 0 || strcmp(knob(src, sub, k)->name, "Tune")) continue;
        int v = sounds[pad].e[i] + semis * 2, m = knob(src, sub, k)->max;
        pot(pad, src, sub, k, v < 0 ? 0 : v > m ? m : v);
    }
}
void x0x_on(int pad, int src, int sub, int note, int semis, int accent, int slide) {
    if (src == SRC_909) tune_by(pad, src, sub, semis), drum909_trigger(&inst[pad].d9, sub, accent ? 1.0f : 0.75f);
    else if (src == SRC_808) tune_by(pad, src, sub, semis), drum808_trigger(&inst[pad].d8, sub, accent ? 1.0f : D8_VEL_NORMAL);
    else if (src == SRC_303) bass303_note_on(&inst[pad].b3, note, accent, slide);
}
void x0x_off(int pad, int src) { if (src == SRC_303) bass303_note_off(&inst[pad].b3); }
void x0x_kill(int pad, int src) { if (src == SRC_303) bass303_all_off(&inst[pad].b3); }
int x0x_fill(int pad, int src, float *s) {
    float r[CTL], d[CTL];
    int on = 1;
    memset(s, 0, CTL * sizeof *s);
    memset(r, 0, sizeof r);
    memset(d, 0, sizeof d);
    switch (src) {
    case SRC_909: drum909_render(&inst[pad].d9, s, r, d, CTL); on = drum909_active(&inst[pad].d9); break;
    case SRC_808: drum808_render(&inst[pad].d8, s, r, d, CTL); on = drum808_active(&inst[pad].d8); break;
    case SRC_303: bass303_render(&inst[pad].b3, s, CTL); on = !inst[pad].b3.idle; break;
    case SRC_BREAK: breaks_render(&inst[pad].br, s, CTL); on = !breaks_silent(&inst[pad].br); break;
    }
    for (int i = 0; i < CTL; ++i) s[i] *= 32767.0f;
    return on;
}

/* ---- the break ---- */
static const short ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static uint8_t *ima_encode(const short *pcm, int n) { /* low nibble first, from 0 / 0: breaks.h's format */
    static const signed char adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    uint8_t *out = calloc((size_t)n / 2 + 1, 1);
    int pred = 0, idx = 0;
    for (int i = 0; out && i < n; ++i) {
        int step = ima_step[idx], d = pcm[i] - pred, nib = d < 0 ? 8 : 0, diff = step >> 3;
        if (d < 0) d = -d;
        if (d >= step) nib |= 4, d -= step, diff += step;
        if (d >= step >> 1) nib |= 2, d -= step >> 1, diff += step >> 1;
        if (d >= step >> 2) nib |= 1, diff += step >> 2;
        pred += nib & 8 ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += adj[nib & 7];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        out[i >> 1] |= (uint8_t)(nib << (i & 1 ? 4 : 0));
    }
    return out;
}
int x0x_break_guess(int frames) { /* the bars that put the loop nearest 120 BPM */
    int best = 1;
    double err = 1e9;
    for (int b = 1; b <= 8; b <<= 1) {
        double bpm = 240.0 * b * FS / frames, e = bpm > 120 ? bpm / 120 : 120 / bpm;
        if (e < err) err = e, best = b;
    }
    return best;
}
int x0x_break_load(int pad, const short *pcm, int n, int bars, const int *e) {
    static breaks_t b; /* this thread's; the audio thread gets a finished copy */
    uint8_t *mem = ima_encode(pcm, n);
    if (!mem) return 0;
    breaks_init(&b);
    brk_loop_t loop = { mem, (uint32_t)n, FS, (uint8_t)bars };
    breaks_set_loop(&b, 0, &loop);
    breaks_set(&b, BRK_LEVEL, 127);
    for (int i = 0; i < BRK_PLAIN; ++i) breaks_set(&b, i, e[i]);
    breaks_set(&b, BRK_PITCH, e[T_PITCH]), breaks_set(&b, BRK_FILT, e[T_FILTER]);
    ui_lock();
    uint8_t *old = brk_mem[pad];
    memcpy(&inst[pad].br, &b, sizeof b);
    brk_mem[pad] = mem, brk_frames[pad] = n;
    ui_unlock();
    free(old);
    return 1;
}
void x0x_break_free(int pad) { free(brk_mem[pad]); brk_mem[pad] = 0; }
void x0x_break_step(int pad, int step, unsigned bar, int frames, int on) { breaks_step(&inst[pad].br, step, (int)bar, (float)frames, on); }
void x0x_break_stop(int pad) { breaks_stop(&inst[pad].br); }
void x0x_break_live(int pad, int key, int down) { breaks_live(&inst[pad].br, key, down); }

/* X0X's tools/gen_builtin_break.c: two bars at 96 BPM on its own 909, so no recording ships */
typedef struct { int voice; const char *s; } lane_t;
static const lane_t GROOVE[] = {
    { DR_BD, "X.....x...x.....X.....x..x......" }, { DR_SD, "....X..g.g..X..g....X..g.g..X.g." },
    { DR_CH, "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x." }, { DR_OH, "...............x...............x" },
};
static const lane_t FILL[] = {
    { DR_BD, "X.....x...x.x...X..x..x...x.x..." }, { DR_SD, "....X..g.g.gX.gg....X.gX.gX.XgXX" },
    { DR_CH, "x.x.x.xxx.x.x.x.x.xxx.x.x.x....." }, { DR_OH, ".......x.......x.......x........" },
    { DR_LT, "............................x..." }, { DR_MT, "..........................x....." },
    { DR_HT, "........................x......." },
};
int x0x_break_render(int which, short **out) {
    static drum909_t d;
    const lane_t *L = which ? FILL : GROOVE;
    int nl = which ? 7 : 4;
    double spb = FS * 60.0 / 96 / 4;
    int total = (int)(spb * 32 + 0.5), pos = 0, step = 0;
    float *mix = calloc((size_t)total, sizeof *mix), rev[256], dly[256], top = 1e-6f;
    short *pcm = malloc((size_t)total * sizeof *pcm);
    if (!mix || !pcm) return free(mix), free(pcm), *out = 0, 0;
    drum909_init(&d);
    while (pos < total) {
        int next = step < 32 ? (int)(spb * step + 0.5) : total;
        if (pos >= next && step < 32) {
            for (int l = 0; l < nl; ++l) {
                char c = L[l].s[step];
                if (c == 'x' || c == 'X' || c == 'g') drum909_trigger(&d, L[l].voice, c == 'X' ? 1.0f : c == 'g' ? 0.35f : 0.75f);
            }
            ++step;
            continue;
        }
        int n = next - pos > 256 ? 256 : next - pos;
        drum909_render(&d, mix + pos, rev, dly, n);
        pos += n;
    }
    for (int i = 0; i < total; ++i) if (mix[i] > top) top = mix[i]; else if (-mix[i] > top) top = -mix[i];
    for (int i = 0; i < total; ++i) pcm[i] = (short)(mix[i] / top * 28000);
    free(mix);
    *out = pcm;
    return total;
}

/* ---- TB-3PO ---- */
static void to_part(bpart_t *b, const x0x_step_t *s, int len, const x0x_gen_t *g) {
    memset(b, 0, sizeof *b);
    tb3po_defaults(&b->gen, g->seed);
    b->gen.density = (uint8_t)g->density, b->gen.root = (uint8_t)g->root, b->gen.scale = (uint8_t)g->scale, b->gen.base_oct = (uint8_t)g->base_oct;
    b->len = (uint8_t)len;
    for (int i = 0; i < len; ++i)
        b->step[i].note = s[i].note, b->step[i].flags = (uint8_t)((s[i].on ? G_NOTE : G_REST) | (s[i].accent ? BS_ACCENT : 0) | (s[i].slide ? BS_SLIDE : 0));
}
static void from_part(const bpart_t *b, x0x_step_t *s, int len) {
    for (int i = 0; i < len; ++i) {
        int f = b->step[i].flags;
        s[i].on = (f & BS_GATE_MASK) != G_REST, s[i].note = b->step[i].note;
        s[i].accent = (f & BS_ACCENT) != 0, s[i].slide = (f & BS_SLIDE) != 0;
    }
}
void x0x_line(x0x_step_t *s, int len, const x0x_gen_t *g) {
    bpart_t b;
    to_part(&b, s, len, g);
    tb3po_generate(&b);
    from_part(&b, s, len);
}
void x0x_mutate(x0x_step_t *s, int len, const x0x_gen_t *g, unsigned *rng) {
    bpart_t b;
    to_part(&b, s, len, g);
    uint32_t r = *rng;
    tb3po_mutate(&b, &r);
    *rng = r;
    from_part(&b, s, len);
}
unsigned x0x_seed(unsigned entropy) { return tb3po_new_seed(entropy); }

/* ---- the bus: a distortion and a glue compressor on the mix, and a reverb and a delay for the sends ---- */
void x0x_init(void) {
    memset(inst, 0, sizeof inst);
    memset(tuned, 0, sizeof tuned);
    bus_x0x = 0;
    for (int i = 1; i < BUS_TILES; ++i) bus_pot[i] = fxbus_param(bus_map[i])->def;
    bus_pot[0] = 0;
    x0x_bus_reset();
}
void x0x_bus_reset(void) {
    fxbus_init(&bus, bus_dly, BUS_DLY);
    fxbus_set(&bus, FX_VOLUME, 127); /* unity: the master gain is ours */
    for (int i = 1; i < BUS_TILES; ++i) fxbus_set(&bus, bus_map[i], bus_pot[i]);
}
int x0x_bus_tile(int i, tile_t *t) {
    static const char *const kinds[2] = { "1", "2" };
    static const char *const plain[BUS_TILES] = { "FX type", "Distort", "Drive", "Compress", "Rev time", "Rev tone", "Rev thin", "Rev level",
                                                  "Echo time", "Repeats", "Echo tone", "Echo level", "Echo thin", "Echo type", "Wear", "Ping-pong" };
    memset(t, 0, sizeof *t);
    if (i < 0 || i >= BUS_TILES) return 0;
    t->name = plain[i];
    if (!i) { t->max = 1, t->names = kinds; return 1; }
    const x0x_param_t *p = fxbus_param(bus_map[i]);
    t->max = p->max, t->names = p->names;
    return 1;
}
int x0x_bus_get(int i) { return i ? bus_pot[i] : bus_x0x; }
void x0x_bus_set(int i, int v) {
    if (!i) { bus_x0x = v; return; }
    bus_pot[i] = v;
    fxbus_set(&bus, bus_map[i], v);
}
int x0x_bus_sends(void) { return bus_x0x; }
void x0x_bus_block(int *l, int *r, int *dly, int *rev, int bpm) {
    if (!bus_x0x && !fxbus_get(&bus, FX_DIST) && !fxbus_get(&bus, FX_COMP)) return; /* as if it were not there */
    float dl[CTL], dr[CTL], v[CTL], d[CTL], ol[CTL], orr[CTL];
    const float k = 1.0f / 32768;
    for (int i = 0; i < CTL; ++i) {
        dl[i] = (float)l[i] * k, dr[i] = (float)r[i] * k;
        v[i] = rev ? (float)rev[i] * k : 0, d[i] = dly ? (float)dly[i] * k : 0;
    }
    fxbus_set_bpm(&bus, (float)bpm);
    fxbus_process_st(&bus, dl, dr, v, d, ol, orr, CTL);
    for (int i = 0; i < CTL; ++i) { /* inside the mix's own bound: punch.c's filters take it from here */
        float a = ol[i] > 8 ? 8 : ol[i] < -8 ? -8 : ol[i], b = orr[i] > 8 ? 8 : orr[i] < -8 ? -8 : orr[i];
        l[i] = (int)(a * 32768), r[i] = (int)(b * 32768);
    }
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The stage: a strip, sixteen pads and five mode tabs, the pads meaning what the mode says. The
 * wheel is one knob, always the value the strip names; a held pad makes it that pad's. Centre tap
 * is REC, Centre held is the modifier (volume on the wheel, select or copy on a pad). Return taps
 * undo; its hold is the firmware's quit and is never bound here. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "seq.h"
#include "ui.h"
#include "font.h"

#define KEY_RETURN 170
#define KEY_PLAY 171
#define KEY_CENTRE 218
#define KEY_PREV 222
#define KEY_NEXT 223
#define STRIP 28
#define PAD 66
#define GUT 6
#define PAD_X 8
#define TAB_X 296
#define TAB_W 79
#define TAB_H 56
#define DETENT 13          /* wheel units a step: sixteen a turn of 200 */
#define HOLD_MS 300        /* Centre down this long is a hold */
#define IDLE_QUIT_MS (5 * 60 * 1000)
#define AUTOSAVE_MS 2000

enum { L_LEVEL, L_PITCH, L_DECAY, L_CUTOFF, L_NUDGE, L_N }; /* WRITE's lock parameters */
static const char *const lock_names[L_N] = { "Level", "Pitch", "Decay", "Cutoff", "Nudge" };
static const int lock_param[L_N] = { P_LEVEL, P_PITCH, P_DECAY, P_CUTOFF, -1 };
static const char *const mode_names[M_N] = { "PLAY", "WRITE", "SOUND", "FX", "PATT" };
enum { WT_MODE, WT_BPM, WT_VOL }; /* what the wheel turns when no pad is held */

static struct {
    int mode, sound, tile, lock, wheel_to;
    int vol;            /* 0..100, the soft volume */
    int acc;            /* wheel units since the last detent */
    int pad;            /* the pad under the finger, -1 */
    int pad2;           /* a second pad down (PATT's chain), -1 */
    int turned;         /* the wheel moved during this pad or Centre hold */
    int centre, centre_hold; /* Centre down; became a hold */
    long long centre_at, touch_at, last_input, dirty_at;
    int dirty, quit;
    int strip_flash;    /* the value shown large: the param index shown, -1 */
    char strip_name[16], strip_val[16];
    pthread_mutex_t lock_m;
} ui;

void ui_lock(void) { pthread_mutex_lock(&ui.lock_m); }
void ui_unlock(void) { pthread_mutex_unlock(&ui.lock_m); }
int ui_quit(void) { return ui.quit; }
int ui_dirty(void) { return ui.dirty && ui.dirty_at; }
void ui_saved(void) { ui.dirty = 0; }
int ui_lively(long long now) { return seq.playing || now - ui.touch_at < 500; }
static void touched(long long now) { ui.last_input = now; }
static void changed(long long now) { ui.dirty = 1; ui.dirty_at = now; }

void ui_init(void) {
    memset(&ui, 0, sizeof ui);
    pthread_mutex_init(&ui.lock_m, 0);
    ui.vol = 100;
    ui.pad = ui.pad2 = -1;
    engine_gain(plat_start_gain());
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void set_gain(void) { engine_gain(plat_start_gain() / 100 * ui.vol * ui.vol / 100); } /* 65536 x 10000 fits an int */

/* ---- geometry ---- */
static int pad_x(int i) { return PAD_X + (i % 4) * (PAD + GUT); }
static int pad_y(int i) { return STRIP + (3 - i / 4) * (PAD + GUT); } /* 1 is bottom-left */
static int pad_at(int x, int y) {
    for (int i = 0; i < NPAD; ++i)
        if (x >= pad_x(i) && x < pad_x(i) + PAD && y >= pad_y(i) && y < pad_y(i) + PAD) return i;
    return -1;
}
static int tab_at(int x, int y) {
    if (x < TAB_X || y < STRIP || y >= STRIP + M_N * TAB_H) return -1;
    return (y - STRIP) / TAB_H;
}

/* ---- the wheel ---- */
static int detents(int d) { /* units to whole steps, the remainder kept */
    ui.acc += d;
    int n = 0;
    while (ui.acc >= DETENT) ui.acc -= DETENT, ++n;
    while (ui.acc <= -DETENT) ui.acc += DETENT, --n;
    return n;
}
static void turn_param(sound_t *s, int k, int n) { s->p[k] = clampi(s->p[k] + n, param_min[k], param_max[k]); }
static void wheel(int d, long long now) {
    d = -d; /* the firmware's travel rises anticlockwise; here clockwise is up */
    if (ui.centre) ui.centre_hold = 1;
    ui.turned = 1;
    if (ui.centre) { /* volume, or PATT's swing */
        int n = detents(d);
        if (!n) return;
        if (ui.mode == M_PATT) seq.swing = clampi(seq.swing + n * 5, 0, 100);
        else ui.vol = clampi(ui.vol + n * 5, 0, 100), set_gain();
        changed(now);
        return;
    }
    if (ui.pad >= 0) { /* that pad's value */
        int n = detents(d);
        if (!n) return;
        if (ui.mode == M_PLAY) turn_param(&sounds[ui.pad], P_PITCH, n), ui.sound = ui.pad;
        else if (ui.mode == M_SOUND) turn_param(&sounds[ui.sound], ui.pad, n);
        else if (ui.mode == M_WRITE) {
            step_t *st = &seq.pat[seq.cur].s[ui.sound][ui.pad];
            signed char *v = ui.lock == L_LEVEL ? &st->level : ui.lock == L_PITCH ? &st->pitch
                           : ui.lock == L_DECAY ? &st->decay : ui.lock == L_CUTOFF ? &st->cutoff : &st->nudge;
            int k = lock_param[ui.lock];
            if (k < 0) *v = (signed char)clampi(*v + n * 8, -128 + 1, 127);
            else *v = (signed char)clampi((*v == UNSET ? sounds[ui.sound].p[k] : *v) + n, param_min[k], param_max[k]);
            st->on = 1;
        }
        changed(now);
        return;
    }
    if (ui.wheel_to == WT_VOL) { int n = detents(d); if (n) ui.vol = clampi(ui.vol + n * 5, 0, 100), set_gain(); return; }
    if (ui.wheel_to == WT_BPM || ui.mode == M_PATT) { int n = detents(d); if (n) seq.bpm = clampi(seq.bpm + n, 40, 240), changed(now); return; }
    if (ui.mode == M_PLAY) { /* the performance sweep: continuous, a turn end to end */
        static int sweep;
        sweep = clampi(sweep + d, -100, 100);
        if (sweep > -4 && sweep < 4) sweep = 0; /* a rest at the centre */
        engine_sweep(sweep);
        return;
    }
    int n = detents(d);
    if (!n) return;
    if (ui.mode == M_SOUND) turn_param(&sounds[ui.sound], ui.tile, n), changed(now);
    else if (ui.mode == M_WRITE) {
        int k = lock_param[ui.lock];
        if (k >= 0) turn_param(&sounds[ui.sound], k, n);
        else { unsigned char *len = &seq.pat[seq.cur].len[ui.sound]; *len = (unsigned char)clampi(*len + n, 1, NSTEP); }
        changed(now);
    }
}

/* ---- pads ---- */
static void pad_down(int i, long long now) {
    if (ui.pad >= 0 && ui.mode == M_PATT && ui.pad2 < 0 && i != ui.pad) { /* two held: a chain */
        ui.pad2 = i;
        seq.chain[0] = ui.pad, seq.chain[1] = i;
        if (!seq.playing) seq.cur = ui.pad;
        changed(now);
        return;
    }
    if (ui.pad >= 0) return;
    ui.pad = i;
    ui.turned = 0;
    if (ui.centre) ui.centre_hold = 1;
    if (ui.mode == M_PLAY) {
        ui.sound = i;
        if (!ui.centre) seq_tap(i), changed(now);
    } else if (ui.mode == M_SOUND)
        ui.tile = i;
}
static void pad_up(int i, long long now) {
    if (i == ui.pad2) { ui.pad2 = -1; return; }
    if (i != ui.pad) return;
    ui.pad = -1;
    if (ui.turned || ui.pad2 >= 0) { ui.pad2 = -1; return; } /* a turn is not a tap */
    if (ui.mode == M_WRITE) {
        step_t *st = &seq.pat[seq.cur].s[ui.sound][i];
        st->on = !st->on;
        if (!st->on) st->level = st->pitch = st->decay = st->cutoff = UNSET, st->nudge = 0;
        changed(now);
    } else if (ui.mode == M_PATT) {
        if (ui.centre) { seq.pat[i] = seq.pat[seq.cur]; changed(now); } /* Commit: a copy */
        else seq_cue(i);
    }
}

/* ---- keys ---- */
static void key(char what, int code, long long now) {
    int down = what == 'd';
    if (code == KEY_CENTRE) {
        if (down) ui.centre = 1, ui.centre_hold = 0, ui.centre_at = now, ui.turned = 0;
        else {
            if (ui.centre && !ui.centre_hold) seq_rec(!seq.rec), seq.ntake = 0;
            ui.centre = ui.centre_hold = 0;
        }
    } else if (!down) return;
    else if (code == KEY_PLAY) seq_play(!seq.playing);
    else if (code == KEY_PREV || code == KEY_NEXT) seq_cue((seq.cue >= 0 ? seq.cue : seq.cur) + (code == KEY_NEXT ? 1 : -1));
    else if (code == KEY_RETURN) {
        if (ui.mode == M_PATT && seq.chain[0] >= 0) seq.chain[0] = seq.chain[1] = -1;
        else seq_undo();
        changed(now);
    }
}

/* ---- the strip's tap targets ---- */
static void strip_tap(int x) {
    if (x < 60) seq_play(!seq.playing);
    else if (x < 130) ui.wheel_to = ui.wheel_to == WT_BPM ? WT_MODE : WT_BPM;
    else if (x < 290) { if (ui.mode == M_WRITE) ui.lock = (ui.lock + 1) % L_N; ui.wheel_to = WT_MODE; }
    else ui.wheel_to = ui.wheel_to == WT_VOL ? WT_MODE : WT_VOL;
}

void ui_event(const event_t *e, long long now) {
    if (e->kind == 'q') { ui.quit = 1; return; }
    touched(now);
    ui_lock();
    if (e->kind == 'w') wheel(e->a, now);
    else if (e->kind == 'k') key(e->what, e->a, now);
    else if (e->kind == 't') {
        ui.touch_at = now;
        int p = pad_at(e->a, e->b), t = tab_at(e->a, e->b);
        if (e->what == 'd') {
            if (p >= 0) pad_down(p, now);
            else if (t >= 0) { ui.mode = t; ui.wheel_to = WT_MODE; ui.pad = ui.pad2 = -1; }
            else if (e->b < STRIP) strip_tap(e->a);
        } else if (e->what == 'u') {
            if (ui.pad2 >= 0 && (p < 0 || p == ui.pad2)) pad_up(ui.pad2, now);
            if (ui.pad >= 0 && (p < 0 || p == ui.pad || ui.pad2 < 0)) pad_up(ui.pad, now);
            ui.pad = ui.pad2 = -1; /* one finger: everything lifts */
        }
    }
    ui_unlock();
}

void ui_tick(long long now) {
    if (ui.centre && !ui.centre_hold && now - ui.centre_at >= HOLD_MS) ui.centre_hold = 1;
    if (!seq.playing && now - ui.last_input > IDLE_QUIT_MS) ui.quit = 1;
}

/* ---- drawing ---- */
static pix rgb(int r, int g, int b) { return 0xff000000u | (pix)r << 16 | (pix)g << 8 | (pix)b; }
static pix mix(pix a, pix b, int t) { /* t 0..256 of b */
    int r = ((a >> 16 & 255) * (256 - t) + (b >> 16 & 255) * t) >> 8;
    int g = ((a >> 8 & 255) * (256 - t) + (b >> 8 & 255) * t) >> 8;
    int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return rgb(r, g, bl);
}
static void fill(pix *c, int x, int y, int w, int h, pix col) {
    for (int Y = y < 0 ? 0 : y; Y < y + h && Y < H; ++Y)
        for (int X = x < 0 ? 0 : x; X < x + w && X < W; ++X) c[Y * W + X] = col;
}
static void frame(pix *c, int x, int y, int w, int h, int t, pix col) {
    fill(c, x, y, w, t, col), fill(c, x, y + h - t, w, t, col);
    fill(c, x, y, t, h, col), fill(c, x + w - t, y, t, h, col);
}
static int text_w(const char *s, int big) {
    const unsigned char *adv = big ? f20_adv : f14_adv;
    int w = 0;
    for (; *s; ++s) if (*s >= 32 && *s < 127) w += adv[*s - 32];
    return w;
}
static void text(pix *c, int x, int y, const char *s, pix col, int big) {
    const unsigned char *adv = big ? f20_adv : f14_adv, *px = big ? f20_px : f14_px;
    const unsigned short *off = big ? f20_off : f14_off;
    int h = big ? F20_H : F14_H;
    for (; *s; ++s) {
        if (*s < 32 || *s > 126) continue;
        int k = *s - 32, w = adv[k];
        const unsigned char *g = px + off[k];
        for (int r = 0; r < h; ++r)
            for (int i = 0; i < w; ++i) {
                int X = x + i, Y = y + r, a = g[r * w + i];
                if (a && X >= 0 && X < W && Y >= 0 && Y < H) c[Y * W + X] = mix(c[Y * W + X], col, a + (a >> 7));
            }
        x += w;
    }
}
static void text_in(pix *c, int x, int y, int w, int h, const char *s, pix col, int big) {
    text(c, x + (w - text_w(s, big)) / 2, y + (h - (big ? F20_H : F14_H)) / 2, s, col, big);
}
static const pix pal[NPAD] = { 0xffe05050, 0xffe08a40, 0xffe0c040, 0xffa0d040, 0xff50d060, 0xff40d0a0,
                               0xff40c0d0, 0xff4090e0, 0xff5060e0, 0xff9050e0, 0xffd050d0, 0xffe05090,
                               0xffc08060, 0xff80a080, 0xff6080b0, 0xffb0b0b0 };
#define BG 0xff101010
#define PADBG 0xff262626
#define INK 0xffe8e8e8
#define DIM 0xff808080

static void pad_label(int i, char *out, size_t n, int *value) { /* the pad's text and its value's text */
    *value = 0;
    switch (ui.mode) {
    case M_PLAY: snprintf(out, n, "%s", sounds[i].smp.name[0] ? sounds[i].smp.name : "-"); break;
    case M_WRITE: snprintf(out, n, "%d", i + 1); break;
    case M_SOUND:
        snprintf(out, n, "%s", param_names[i]);
        *value = i != P_LOAD;
        break;
    case M_FX: snprintf(out, n, "-"); break;
    case M_PATT: snprintf(out, n, "P%d", i + 1); break;
    }
}
static void strip(pix *c, long long now) {
    (void)now;
    fill(c, 0, 0, W, STRIP, 0xff000000);
    /* play state: a triangle or a square */
    if (seq.playing) for (int r = 0; r < 12; ++r) fill(c, 14, 8 + r, r < 6 ? r * 2 : (11 - r) * 2, 1, INK);
    else fill(c, 14, 9, 10, 10, DIM);
    if (seq.rec) for (int r = 0; r < 10; ++r) { int d = r - 4, w = d * d < 20 ? 2 * (int)(4.5f + __builtin_sqrtf(20 - d * d) - 4.5f) : 0; fill(c, 40 - w / 2, 9 + r, w, 1, 0xffff3030); }
    char b[32];
    snprintf(b, sizeof b, "%d", seq.bpm);
    text(c, 62, 3, b, ui.wheel_to == WT_BPM ? pal[0] : INK, 1);
    snprintf(b, sizeof b, "P%d%s", seq.cur + 1, seq.cue >= 0 ? ">" : "");
    if (seq.cue >= 0) snprintf(b + strlen(b), sizeof b - strlen(b), "%d", seq.cue + 1);
    text(c, 108, 3, b, DIM, 1);
    /* the wheel's name and value */
    const char *name = ""; int val = 0, show = 1;
    if (ui.centre_hold) name = ui.mode == M_PATT ? "Swing" : "Volume", val = ui.mode == M_PATT ? seq.swing : ui.vol;
    else if (ui.wheel_to == WT_VOL) name = "Volume", val = ui.vol;
    else if (ui.wheel_to == WT_BPM) name = "BPM", val = seq.bpm;
    else if (ui.mode == M_PLAY && ui.pad >= 0) name = "Pitch", val = sounds[ui.pad].p[P_PITCH];
    else if (ui.mode == M_PLAY) name = "Sweep", show = 0;
    else if (ui.mode == M_SOUND) name = param_names[ui.pad >= 0 ? ui.pad : ui.tile], val = sounds[ui.sound].p[ui.pad >= 0 ? ui.pad : ui.tile], show = (ui.pad >= 0 ? ui.pad : ui.tile) != P_LOAD;
    else if (ui.mode == M_WRITE) {
        name = lock_names[ui.lock];
        if (ui.pad >= 0) {
            const step_t *st = &seq.pat[seq.cur].s[ui.sound][ui.pad];
            signed char v = ui.lock == L_LEVEL ? st->level : ui.lock == L_PITCH ? st->pitch : ui.lock == L_DECAY ? st->decay : ui.lock == L_CUTOFF ? st->cutoff : st->nudge;
            int k = lock_param[ui.lock];
            val = v == UNSET && k >= 0 ? sounds[ui.sound].p[k] : v;
        } else val = lock_param[ui.lock] >= 0 ? sounds[ui.sound].p[lock_param[ui.lock]] : seq.pat[seq.cur].len[ui.sound];
        if (ui.lock == L_NUDGE && ui.pad < 0) name = "Length";
    } else if (ui.mode == M_PATT) name = "BPM", val = seq.bpm;
    if (show) snprintf(b, sizeof b, "%s %d", name, val); else snprintf(b, sizeof b, "%s", name);
    text(c, 150, 3, b, ui.centre_hold || ui.pad >= 0 ? pal[ui.sound] : INK, 1);
    /* the meter and the volume chip */
    int pk = engine_peak() * 40 / 32767;
    fill(c, 300, 11, 40, 6, PADBG);
    fill(c, 300, 11, pk, 6, pk > 36 ? pal[0] : pal[4]);
    snprintf(b, sizeof b, "%d", ui.vol);
    text(c, 346, 6, b, ui.wheel_to == WT_VOL ? pal[0] : DIM, 0);
}
void ui_render(pix *c, long long now) {
    for (int i = 0; i < W * H; ++i) c[i] = BG;
    unsigned blocks = engine_blocks();
    int head = seq_playhead();
    for (int i = 0; i < NPAD; ++i) {
        int x = pad_x(i), y = pad_y(i);
        pix col = PADBG, ink = INK;
        char label[32];
        int has_value;
        pad_label(i, label, sizeof label, &has_value);
        if (ui.mode == M_PLAY) {
            unsigned since = blocks - engine_last_hit(i);
            if (engine_last_hit(i) && since < 12) col = mix(pal[i], 0xffffffff, (int)(256 - since * 20));
            else if (i == ui.sound) col = mix(PADBG, pal[i], 96);
            if (!sounds[i].smp.pcm) ink = DIM;
        } else if (ui.mode == M_WRITE) {
            const step_t *st = &seq.pat[seq.cur].s[ui.sound][i];
            int in = i < seq.pat[seq.cur].len[ui.sound];
            if (st->on) col = mix(PADBG, pal[ui.sound], in ? 200 : 70), ink = in ? 0xff000000 : DIM;
            else if (!in) ink = DIM;
            if (st->on && (st->level != UNSET || st->pitch != UNSET || st->decay != UNSET || st->cutoff != UNSET || st->nudge)) fill(c, x + PAD - 10, y + 4, 6, 6, INK);
        } else if (ui.mode == M_SOUND) {
            if (i == ui.tile) col = mix(PADBG, pal[ui.sound], 110);
        } else if (ui.mode == M_PATT) {
            if (i == seq.cur) col = mix(PADBG, pal[i], 160), ink = 0xff000000;
            if (i == seq.chain[0] || i == seq.chain[1]) frame(c, x - 2, y - 2, PAD + 4, PAD + 4, 2, pal[i]);
        } else ink = DIM;
        if (i == ui.pad || i == ui.pad2) col = mix(col, 0xffffffff, 60);
        fill(c, x, y, PAD, PAD, col);
        if (ui.mode == M_WRITE && i == head) frame(c, x, y, PAD, PAD, 3, INK);
        if (ui.mode == M_PATT && i == seq.cue) frame(c, x, y, PAD, PAD, 2, INK);
        if (has_value) {
            char v[16];
            int p = sounds[ui.sound].p[i];
            snprintf(v, sizeof v, i == P_LOOP || i == P_REVERSE ? (p ? "On" : "Off") : "%d", p);
            text_in(c, x, y + 6, PAD, F14_H, label, ink, 0);
            text_in(c, x, y + 32, PAD, F20_H, v, ink, 1);
        } else if (ui.mode == M_WRITE || ui.mode == M_PATT)
            text_in(c, x, y, PAD, PAD, label, ink, 1);
        else
            text_in(c, x, y, PAD, PAD, label, ink, 0);
    }
    for (int m = 0; m < M_N; ++m) {
        int y = STRIP + m * TAB_H;
        fill(c, TAB_X, y, TAB_W, TAB_H - 2, m == ui.mode ? mix(PADBG, pal[ui.sound], 140) : 0xff1a1a1a);
        text_in(c, TAB_X, y, TAB_W, TAB_H - 2, mode_names[m], m == ui.mode ? 0xff000000 : DIM, 0);
    }
    strip(c, now);
}

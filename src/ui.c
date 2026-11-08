/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The stage: a strip, sixteen pads and six mode tabs, the pads meaning what the mode says. The
 * wheel is one knob, always the value the strip names; a held pad makes it that pad's. Centre tap
 * is REC, Centre held is the modifier (volume on the wheel, select or copy on a pad). Return taps
 * undo; its hold is the firmware's quit and is never bound here. A second tap on WRITE's or SOUND's
 * tab turns it to its other layer, as the panel has one finger. SOUND's Load opens a picker over
 * the pads, which has Centre and Return to itself while it is open. */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "drop.h"
#include "engine.h"
#include "fx.h"
#include "load.h"
#include "project.h"
#include "seq.h"
#include "source.h"
#include "synth.h"
#include "ui.h"
#include "x0x.h"
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
#define TAB_H 48
#define DETENT 13          /* wheel units a step: sixteen a turn of 200 */
#define HOLD_MS 300        /* Centre down this long is a hold */
#define IDLE_QUIT_MS (5 * 60 * 1000)
#define AUTOSAVE_MS 2000
#define MSG_MS 2000        /* how long a job's result stays on the strip */
#define ROW_H 24           /* the picker's rows */
#define PICK_ROWS 12
#define PICK_MAX 512       /* ponytail: the first 512 of a folder are listed */
#define REGIONS 15         /* the pick's start points, SOUND_SECONDS apart */
#define ASK_ROWS 3         /* a picked file's rows before those */
#define DROP_PORT 8642
#define PAGES (NSTEP / BAR) /* WRITE's pages of steps */

enum { L_LEVEL, L_PITCH, L_DECAY, L_CUTOFF, L_NUDGE, L_N }; /* WRITE's lock parameters */
static const char *const lock_names[L_N] = { "Level", "Pitch", "Decay", "Cutoff", "Nudge" };
static const int lock_param[L_N] = { P_LEVEL, P_PITCH, P_DECAY, P_CUTOFF, -1 };
enum { G_RATCHET, G_CHANCE, G_ACCENT, G_SLIDE, G_RATE, G_N }; /* WRITE's second layer */
static const char *const groove_names[G_N] = { "Ratchet", "Chance", "Accent", "Slide", "Rate" };
static const char *const mode_names[M_N] = { "PLAY", "KEYS", "WRITE", "SOUND", "FX", "PATT" };
enum { WT_MODE, WT_BPM, WT_VOL }; /* what the wheel turns when no pad is held */
/* the slow job ui_tick is to run: a picked row's kind */
enum { J_NONE, J_KIT = 'k', J_PAD = 'r', J_CHOP = 'c', J_HITS = 'h', J_BREAK = 'B', J_BUILTIN = 'g', J_PADBREAK = 't',
       J_SYNTH = 'y', J_VOICE = 'v', J_BASS = '3', J_EXPORT = 'x' };
enum { PM_DIR, PM_ASK, PM_DROP, PM_ENGINE, PM_PRESET, PM_VOICE, PM_BREAK }; /* the picker's screens */

static struct {
    int mode, sound, tile, lock, wheel_to;
    int layer, groove, page; /* WRITE: the second layer and its parameter, the page of steps */
    int sound2, keys_scale;  /* SOUND's machine page; KEYS's wheel is the scale, else the key */
    int vol;            /* 0..100, the soft volume */
    int acc;            /* wheel units since the last detent */
    int pad;            /* the pad under the finger, -1 */
    long long pad_at;   /* when it went down (PATT: held HOLD_MS, it chains) */
    int turned;         /* the wheel moved during this pad or Centre hold */
    int centre, centre_hold; /* Centre down; became a hold */
    long long centre_at, touch_at, last_input, dirty_at;
    int dirty, quit;
    int strip_flash;    /* the value shown large: the param index shown, -1 */
    char strip_name[16], strip_val[16];
    int fx_page, fx_latched;
    int job, job_shown, job_from;
    char job_arg[32], msg[40];
    long long msg_until;
    pthread_mutex_t lock_m;
} ui;

static struct {
    int open, n, cur, top, menu, asked_cur, scan, scan_cur, engine, voice_src, drop_on;
    char dir[640], file[900];
    struct { char kind, name[256]; } row[PICK_MAX]; /* k a kit, d a folder, f a file; the rest see pick_choose */
} pick;

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
    ui.pad = -1;
    ui.last_input = ui.touch_at = plat_ms(); /* the idle clock: CLOCK_MONOTONIC is since boot */
    engine_gain(plat_start_gain());
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int sound2_on(void) { return ui.sound2 && sounds[ui.sound].src != SRC_SAMPLE; } /* a sample has no machine page */
static step_t *step_at(int pad) { return &seq.pat[seq.cur].s[ui.sound][ui.page * BAR + pad]; }
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

/* ---- slow jobs: asked for under the lock, run from ui_tick off it, once the strip has said so ---- */
static void say(const char *m, long long until) {
    snprintf(ui.msg, sizeof ui.msg, "%s", m);
    ui.msg_until = until;
}
static void job_start(int job, const char *m, long long now) {
    ui.job = job, ui.job_shown = 0;
    say(m, now + 3600000);
}
static void job_run(long long now) {
    int job = ui.job, r = 1, from = ui.job_from;
    char name[32] = "";
    ui.job = 0;
    switch (job) {
    case J_KIT:
        if (kit_choose(ui.job_arg)) {
            pads_forget(project_path());
            snprintf(ui.msg, sizeof ui.msg, "Kit: %s", kit_name);
            changed(now);
        } else snprintf(ui.msg, sizeof ui.msg, "Can't read it");
        break;
    case J_EXPORT:
        if (export_pattern(name, sizeof name)) snprintf(ui.msg, sizeof ui.msg, "Saved %s", name);
        else snprintf(ui.msg, sizeof ui.msg, "Export failed");
        ui_lock(); /* the engine started over */
        seq_fx_end();
        ui.fx_latched = 0;
        ui_unlock();
        break;
    default:
        if (job == J_CHOP || job == J_HITS) r = load_chop(pick.file, job == J_HITS);
        else if (job == J_BREAK) r = load_break(ui.sound, pick.file);
        else if (job == J_BUILTIN) r = load_break_builtin(ui.sound, from);
        else if (job == J_PADBREAK) r = source_break(ui.sound, 0);
        else if (job == J_SYNTH) source_synth(ui.sound, pick.engine, from);
        else if (job == J_BASS) source_assign(ui.sound, SRC_303, 0, 0, 0, "303");
        else if (job == J_VOICE) {
            int sub, e[NTILE], choke;
            const char *nm;
            x0x_kit(pick.voice_src, from, &sub, e, &nm, &choke);
            source_assign(ui.sound, pick.voice_src, sub, 0, e, nm);
        } else r = load_pad(ui.sound, pick.file, from * SOUND_SECONDS);
        snprintf(ui.msg, sizeof ui.msg, "%s", r > 0 ? "Loaded" : r < 0 ? "Loaded, not saved" : "Can't read it");
        if (r) changed(now), ui.sound2 = 1; /* a machine's knobs are what comes next */
    }
    ui.msg_until = plat_ms() + MSG_MS; /* a long job has outlived now */
}

/* ---- the picker: the kits and the card's folders and sounds ---- */
static int row_rank(char k) { return k == 'k' ? 0 : k == 'd' ? 1 : 2; }
static int row_cmp(const void *a, const void *b) {
    const char *x = a, *y = b; /* a row's first bytes are its kind, then its name */
    int d = row_rank(x[0]) - row_rank(y[0]);
    return d ? d : strcasecmp(x + 1, y + 1);
}
static void pick_add(char kind, const char *name) {
    if (pick.n >= PICK_MAX) return;
    pick.row[pick.n].kind = kind;
    snprintf(pick.row[pick.n++].name, sizeof pick.row[0].name, "%s", name);
}
static int is_audio(const char *s) {
    static const char *const ext[] = { ".wav", ".mp3", ".flac", ".m4a", ".aac", ".ogg", ".opus", ".ape", ".wma",
                                       ".aif", ".aiff", ".alac", ".dsf", ".dff" };
    const char *dot = strrchr(s, '.');
    if (dot)
        for (size_t i = 0; i < sizeof ext / sizeof *ext; ++i)
            if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}
static int is_dir(const char *parent, const struct dirent *e) {
    if (e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) return e->d_type == DT_DIR;
    char path[1024];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", parent, e->d_name);
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
static void pick_menu(int menu) { pick.menu = menu, pick.n = pick.cur = pick.top = 0; }
static void pick_scan(void) { /* off the lock: a big folder on the card must not hold up the sound */
    int first = 0;
    pick_menu(PM_DIR);
    if (!strcmp(pick.dir, plat_media())) { /* what a pad can be, then the kits, lead the card's root */
        char kits[300];
        pick_add('S', "Synth");
        pick_add('9', "909 drum");
        pick_add('8', "808 drum");
        pick_add('3', "303 bass");
        pick_add('b', "Break");
        pick_add('p', "From phone");
        first = pick.n;
        pick_add('k', KIT_909_LIVE);
        pick_add('k', KIT_808_LIVE);
        snprintf(kits, sizeof kits, "%s/kits", plat_dir());
        DIR *k = opendir(kits);
        for (struct dirent *e; k && (e = readdir(k));)
            if (e->d_name[0] != '.' && is_dir(kits, e)) pick_add('k', e->d_name);
        if (k) closedir(k);
    }
    DIR *d = opendir(pick.dir);
    for (struct dirent *e; d && pick.n < PICK_MAX && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (is_dir(pick.dir, e)) pick_add('d', e->d_name);
        else if (is_audio(e->d_name)) pick_add('f', e->d_name);
    }
    if (d) closedir(d);
    qsort(pick.row + first, (size_t)(pick.n - first), sizeof pick.row[0], row_cmp);
}
static void pick_move(int n) {
    if (!pick.n) return;
    pick.cur = clampi(pick.cur + n, 0, pick.n - 1);
    if (pick.cur < pick.top) pick.top = pick.cur;
    if (pick.cur >= pick.top + PICK_ROWS) pick.top = pick.cur - PICK_ROWS + 1;
}
static void pick_rescan(int cur) { pick.n = 0, pick.menu = PM_DIR, pick.scan = 1, pick.scan_cur = cur; } /* ui_tick does it */
static void pick_open(void) {
    snprintf(pick.dir, sizeof pick.dir, "%s", plat_media());
    pick_rescan(0);
    pick.open = 1;
}
static void pick_ask(void) { /* a file picked: all of it across the kit, or twenty seconds of it to this pad */
    char b[64];
    pick.asked_cur = pick.cur;
    pick_menu(PM_ASK);
    pick_add('h', "Chop at its hits");
    pick_add('c', "Chop into 16 equal");
    snprintf(b, sizeof b, "Break on pad %d", ui.sound + 1);
    pick_add('B', b);
    for (int i = 0; i < REGIONS; ++i) {
        int s = i * SOUND_SECONDS;
        snprintf(b, sizeof b, "To pad %d from %d:%02d", ui.sound + 1, s / 60, s % 60);
        pick_add('r', b);
    }
}
static void pick_engines(void) {
    pick_menu(PM_ENGINE);
    for (int i = 0; i < SYNTH_ENGINES; ++i) pick_add('e', synth_engine(i));
}
static void pick_presets(void) {
    pick_menu(PM_PRESET);
    for (int i = 0; i < synth_presets(pick.engine); ++i) pick_add('y', synth_preset_name(pick.engine, i));
}
static void pick_voices(int src) { /* the live kit's sixteen, tunings and all */
    pick_menu(PM_VOICE);
    pick.voice_src = src;
    for (int i = 0; i < NPAD; ++i) {
        int sub, e[NTILE], choke;
        const char *name;
        x0x_kit(src, i, &sub, e, &name, &choke);
        pick_add('v', name);
    }
}
static void pick_breaks(void) {
    pick_menu(PM_BREAK);
    pick_add('g', "Groove");
    pick_add('g', "Fill");
    if (sounds[ui.sound].smp.pcm) pick_add('t', "This pad's sound");
}
static void pick_back(void) {
    if (pick.menu == PM_PRESET) pick_engines(), pick_move(pick.engine);
    else if (pick.menu != PM_DIR) pick_rescan(pick.menu == PM_ASK ? pick.asked_cur : 0);
    else if (!strcmp(pick.dir, plat_media())) pick.open = 0;
    else *strrchr(pick.dir, '/') = 0, pick_rescan(0);
}
static void pick_choose(long long now) {
    if (!pick.n) return;
    const char *name = pick.row[pick.cur].name;
    size_t l = strlen(pick.dir);
    int kind = pick.row[pick.cur].kind;
    switch (kind) {
    case 'k':
        snprintf(ui.job_arg, sizeof ui.job_arg, "%s", name);
        pick.open = 0;
        job_start(J_KIT, "Loading kit", now);
        break;
    case 'd':
        if (l + 1 + strlen(name) < sizeof pick.dir) snprintf(pick.dir + l, sizeof pick.dir - l, "/%.255s", name), pick_rescan(0);
        break;
    case 'f':
        snprintf(pick.file, sizeof pick.file, "%s/%s", pick.dir, name);
        pick_ask();
        break;
    case 'S': pick_engines(); break;
    case 'e': pick.engine = pick.cur, pick_presets(); break;
    case '9': pick_voices(SRC_909); break;
    case '8': pick_voices(SRC_808); break;
    case 'b': pick_breaks(); break;
    case 'p': pick_menu(PM_DROP); break;
    default: /* r a region, c and h chops, B a break, y a preset, v a drum, 3 the bass, g and t a loop */
        ui.job_from = kind == 'r' ? pick.cur - ASK_ROWS : pick.cur;
        pick.open = 0;
        job_start(kind, "Loading", now);
    }
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
static void turn_tile(int i, int n) {
    if (sound2_on()) source_turn(ui.sound, i, n);
    else turn_param(&sounds[ui.sound], i, n);
}
static void turn_bus(int i, int n) {
    tile_t t;
    if (x0x_bus_tile(i, &t)) x0x_bus_set(i, clampi(x0x_bus_get(i) + n, t.min, t.max));
}
static void turn_rate(int n) {
    signed char *r = &seq.pat[seq.cur].rate[ui.sound];
    *r = (signed char)clampi(*r + n, RATE_MIN, RATE_MAX);
}
/* a step written on a page past the track's end brings the track to that page */
static void step_written(int i) {
    unsigned char *len = &seq.pat[seq.cur].len[ui.sound];
    if (step_at(i)->on && ui.page > (*len - 1) / BAR) *len = (unsigned char)((ui.page + 1) * BAR);
}
static void turn_groove(int i, int n) { /* held: this step's ratchet or chance */
    step_t *st = step_at(i);
    if (ui.groove == G_RATE) { turn_rate(n); return; }
    if (ui.groove == G_RATCHET) st->ratchet = (signed char)clampi((st->ratchet > 1 ? st->ratchet : 1) + n, 1, 8);
    else if (ui.groove == G_CHANCE) {
        int v = clampi((st->chance ? st->chance : 100) + n * 5, 5, 100);
        st->chance = (signed char)(v == 100 ? 0 : v);
    } else return;
    st->on = 1;
    step_written(i);
}
static void wheel(int d, long long now) {
    d = -d; /* the firmware's travel rises anticlockwise; here clockwise is up */
    if (pick.open) { pick_move(detents(d)); return; }
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
    if (ui.pad >= 0 && ui.mode != M_KEYS && (ui.mode != M_FX || ui.fx_page == FX_PAGES)) { /* that pad's value */
        int n = detents(d);
        if (!n) return;
        if (ui.mode == M_PLAY) turn_param(&sounds[ui.pad], P_PITCH, n), ui.sound = ui.pad;
        else if (ui.mode == M_SOUND) turn_tile(ui.pad, n);
        else if (ui.mode == M_FX) turn_bus(ui.pad, n);
        else if (ui.mode == M_WRITE && ui.layer) turn_groove(ui.pad, n);
        else if (ui.mode == M_WRITE) {
            step_t *st = step_at(ui.pad);
            signed char *v = ui.lock == L_LEVEL ? &st->level : ui.lock == L_PITCH ? &st->pitch
                           : ui.lock == L_DECAY ? &st->decay : ui.lock == L_CUTOFF ? &st->cutoff : &st->nudge;
            int k = lock_param[ui.lock];
            if (k < 0) *v = (signed char)clampi(*v + n * 8, -128 + 1, 127);
            else *v = (signed char)clampi((*v == UNSET ? sounds[ui.sound].p[k] : *v) + n, param_min[k], param_max[k]);
            st->on = 1;
            step_written(ui.pad);
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
    if (ui.mode == M_SOUND) turn_tile(ui.tile, n), changed(now);
    else if (ui.mode == M_FX) ui.fx_page = clampi(ui.fx_page + n, 0, FX_PAGES); /* past the effects: the bus */
    else if (ui.mode == M_KEYS) {
        if (ui.keys_scale) seq.scale = clampi(seq.scale + n, 0, NSCALE - 1);
        else seq.key = clampi(seq.key + n, -24, 24);
        changed(now);
    } else if (ui.mode == M_WRITE && ui.layer) {
        if (ui.groove == G_RATE) turn_rate(n), changed(now);
    } else if (ui.mode == M_WRITE) {
        int k = lock_param[ui.lock];
        if (k >= 0) turn_param(&sounds[ui.sound], k, n);
        else { unsigned char *len = &seq.pat[seq.cur].len[ui.sound]; *len = (unsigned char)clampi(*len + n, 1, NSTEP); }
        changed(now);
    }
}

/* ---- pads ---- */
static void fx_stop(long long now) {
    fx_live(-1);
    if (seq_fx_end()) changed(now);
}
static void fx_down(int i, long long now) {
    if (ui.fx_latched) { ui.fx_latched = 0, fx_stop(now); return; } /* the tap that ends a latch does only that */
    if (ui.fx_page == FX_PAGES) return; /* the bus: a tile is held and turned */
    int f = fx_page[ui.fx_page][i];
    if (f < 0) return;
    ui.fx_latched = ui.centre;
    fx_live(f);
    seq_fx_start(f);
    if (seq.fxrec.on) changed(now);
}
static void keys_down(int i, long long now) { /* the scale, or a break's slices and tricks */
    if (sounds[ui.sound].src != SRC_BREAK) seq_note(ui.sound, seq.key + seq_scale_note(i)), changed(now);
    else if (i < 11) x0x_break_live(ui.sound, i, 1);
}
static void keys_up(int i) {
    if (sounds[ui.sound].src != SRC_BREAK) engine_release(ui.sound);
    else if (i < 11) x0x_break_live(ui.sound, i, 0);
}
static void groove_tap(int i) {
    step_t *st = step_at(i);
    if (ui.groove == G_RATE) return;
    if (ui.groove == G_RATCHET) st->ratchet = (signed char)(st->ratchet >= 4 ? 1 : (st->ratchet > 1 ? st->ratchet : 1) + 1);
    else if (ui.groove == G_ACCENT) st->flags ^= NOTE_ACCENT;
    else if (ui.groove == G_SLIDE) st->flags ^= NOTE_SLIDE;
    else {
        int v = (st->chance ? st->chance : 100) - 25;
        st->chance = (signed char)(v > 0 ? v : 0); /* 75 50 25, then always */
    }
    st->on = 1;
}
static void pad_down(int i, long long now) {
    if (ui.pad >= 0) return;
    ui.pad = i;
    ui.pad_at = now;
    ui.turned = 0;
    if (ui.centre) ui.centre_hold = 1;
    if (ui.mode == M_PLAY) {
        ui.sound = i;
        if (!ui.centre) seq_tap(i), changed(now);
    } else if (ui.mode == M_KEYS)
        keys_down(i, now);
    else if (ui.mode == M_SOUND)
        ui.tile = i;
    else if (ui.mode == M_FX)
        fx_down(i, now);
}
static void pad_up(int i, long long now) {
    if (i != ui.pad) return;
    ui.pad = -1;
    if (ui.mode == M_FX && !ui.fx_latched) fx_stop(now);
    if (ui.mode == M_PLAY) engine_release(i);
    else if (ui.mode == M_KEYS) keys_up(i);
    if (ui.turned) return; /* a turn, or PATT's hold, is not a tap */
    if (ui.mode == M_WRITE) {
        step_t *st = step_at(i);
        if (ui.layer) groove_tap(i);
        else if (st->on) *st = (step_t){ 0, UNSET, UNSET, UNSET, UNSET, 0, 0, 0, 0 };
        else st->on = 1;
        step_written(i);
        changed(now);
    } else if (ui.mode == M_PATT) {
        if (ui.centre) { seq.pat[i] = seq.pat[seq.cur]; changed(now); } /* Commit: a copy */
        else seq_cue(i);
    } else if (ui.mode == M_SOUND && sound2_on()) {
        tile_t t;
        if (source_tile(ui.sound, i, &t) && t.action) { /* New, Mutate: Return takes the line back */
            source_line(ui.sound, t.action == ACT_MUTATE, (unsigned)now ^ engine_blocks());
            changed(now);
        }
    } else if (ui.mode == M_SOUND && i == P_LOAD) pick_open();
}

/* ---- keys ---- */
static void key(char what, int code, long long now) {
    int down = what == 'd';
    if (pick.open && (code == KEY_CENTRE || code == KEY_RETURN)) { /* Return on the release, as below */
        if (code == KEY_CENTRE && down) pick_choose(now);
        else if (code == KEY_RETURN && !down) pick_back();
        if (code == KEY_CENTRE) ui.centre = ui.centre_hold = 0; /* held when the picker opened: its release is here */
        return;
    }
    if (code == KEY_CENTRE) {
        if (down) ui.centre = 1, ui.centre_hold = 0, ui.centre_at = now, ui.turned = 0;
        else {
            if (ui.centre && !ui.centre_hold) seq_rec(!seq.rec), seq.ntake = 0;
            ui.centre = ui.centre_hold = 0;
        }
    } else if (code == KEY_RETURN) { /* on the release: a hold's never comes (the firmware's quit) */
        if (down) return;
        if (ui.mode == M_PATT && seq.nsong) seq_song_pop();
        else if (ui.mode == M_FX && !seq.ntake) seq.pat[seq.cur].nev = 0; /* nothing to undo: the bar's effects go */
        else seq_undo();
        changed(now);
    } else if (!down) return;
    else if (code == KEY_PLAY) seq_play(!seq.playing);
    else if ((code == KEY_PREV || code == KEY_NEXT) && ui.mode == M_WRITE) ui.page = clampi(ui.page + (code == KEY_NEXT ? 1 : -1), 0, PAGES - 1);
    else if (code == KEY_PREV || code == KEY_NEXT) seq_cue((seq.cue >= 0 ? seq.cue : seq.cur) + (code == KEY_NEXT ? 1 : -1));
}

/* ---- the strip's tap targets ---- */
static void strip_tap(int x, long long now) {
    if (x < 60) seq_play(!seq.playing);
    else if (x < 130) ui.wheel_to = ui.wheel_to == WT_BPM ? WT_MODE : WT_BPM;
    else if (x < 290) {
        if (ui.mode == M_WRITE && ui.layer) ui.groove = (ui.groove + 1) % G_N;
        else if (ui.mode == M_WRITE) ui.lock = (ui.lock + 1) % L_N;
        else if (ui.mode == M_KEYS) ui.keys_scale = !ui.keys_scale;
        else if (ui.mode == M_PATT && !ui.job) job_start(J_EXPORT, "Exporting", now);
        ui.wheel_to = WT_MODE;
    }
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
            if (pick.open && e->b >= STRIP && e->a < TAB_X) { /* a tap on a row picks it */
                int shown = (e->b - STRIP) / ROW_H, row = pick.top + shown;
                if (shown < PICK_ROWS && row < pick.n) pick.cur = row, pick_choose(now);
            } else if (p >= 0) pad_down(p, now);
            else if (t >= 0) {
                if (t == ui.mode && !pick.open && t == M_WRITE) ui.layer = !ui.layer;
                if (t == ui.mode && !pick.open && t == M_SOUND && sounds[ui.sound].src != SRC_SAMPLE) ui.sound2 = !ui.sound2;
                ui.mode = t; ui.wheel_to = WT_MODE; ui.pad = -1; pick.open = 0;
            }
            else if (e->b < STRIP) strip_tap(e->a, now);
        } else if (e->what == 'u') { /* one finger: whatever was down lifts, wherever it lifts */
            if (ui.pad >= 0) pad_up(ui.pad, now);
            ui.pad = -1;
        }
    }
    ui_unlock();
}

void ui_tick(long long now) {
    int drop = pick.open && pick.menu == PM_DROP;
    if (drop != pick.drop_on) { /* the server lives as long as its screen; stopping it waits, off the lock */
        pick.drop_on = drop;
        if (drop) drop_start(DROP_PORT);
        else drop_stop();
    }
    if (drop && drop_take(pick.file, sizeof pick.file)) pick_ask();
    if (pick.scan) pick.scan = 0, pick_scan(), pick_move(pick.scan_cur);
    if (ui.job && ui.job_shown) job_run(now);
    if (ui.centre && !ui.centre_hold && now - ui.centre_at >= HOLD_MS) ui.centre_hold = 1;
    if (ui.pad >= 0 && ui.mode == M_PATT && !ui.turned && !ui.centre && now - ui.pad_at >= HOLD_MS) {
        ui_lock(); /* a held pattern goes on the end of the song */
        seq_song_add(ui.pad);
        ui.turned = 1;
        changed(now);
        ui_unlock();
    }
    if (!seq.playing && !pick.drop_on && now - ui.last_input > IDLE_QUIT_MS) ui.quit = 1;
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
static void fit(char *s, int w, int big) { /* shortened from the end until it is at most w wide */
    for (size_t n = strlen(s); n && text_w(s, big) > w; --n) s[n - 1] = 0;
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

static void note_name(int semis, char *out, size_t n) { /* semitones from C3 */
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int m = ((semis % 12) + 12) % 12;
    snprintf(out, n, "%s%d", names[m], 3 + (semis - m) / 12);
}
static void step_label(int i, char *out, size_t n) { /* its number, or on the second layer what it does */
    const step_t *st = step_at(i);
    if (ui.layer && st->on && ui.groove == G_RATCHET && st->ratchet > 1) snprintf(out, n, "x%d", st->ratchet);
    else if (ui.layer && st->on && ui.groove == G_CHANCE && st->chance) snprintf(out, n, "%d%%", st->chance);
    else if (ui.layer && st->on && ui.groove == G_ACCENT && st->flags & NOTE_ACCENT) snprintf(out, n, "A");
    else if (ui.layer && st->on && ui.groove == G_SLIDE && st->flags & NOTE_SLIDE) snprintf(out, n, "S");
    else snprintf(out, n, "%d", ui.page * BAR + i + 1);
}
static void pad_label(int i, char *out, size_t n, char *val, size_t vn) { /* the pad's text and its value's, "" none */
    static const char *const tricks[3] = { "Reverse", "Half", "Stutter" };
    tile_t t;
    val[0] = 0;
    switch (ui.mode) {
    case M_PLAY: snprintf(out, n, "%s", sounds[i].smp.name[0] ? sounds[i].smp.name : "-"); break;
    case M_KEYS:
        if (sounds[ui.sound].src != SRC_BREAK) note_name(seq.key + seq_scale_note(i), out, n);
        else if (i < 8) snprintf(out, n, "Slice %d", i + 1);
        else snprintf(out, n, "%s", i < 11 ? tricks[i - 8] : "-");
        break;
    case M_WRITE: step_label(i, out, n); break;
    case M_SOUND:
        if (sound2_on()) {
            snprintf(out, n, "%s", source_tile(ui.sound, i, &t) ? t.name : "-");
            source_text(ui.sound, i, val, vn);
        } else {
            int p = sounds[ui.sound].p[i];
            snprintf(out, n, "%s", param_names[i]);
            if (i != P_LOAD) snprintf(val, vn, i == P_LOOP || i == P_REVERSE ? (p ? "On" : "Off") : "%d", p);
        }
        break;
    case M_FX:
        if (ui.fx_page == FX_PAGES) {
            x0x_bus_tile(i, &t);
            snprintf(out, n, "%s", t.name);
            if (t.names) snprintf(val, vn, "%s", t.names[x0x_bus_get(i) - t.min]);
            else snprintf(val, vn, "%d", x0x_bus_get(i));
        } else snprintf(out, n, "%s", fx_page[ui.fx_page][i] >= 0 ? fx_name(fx_page[ui.fx_page][i]) : "-");
        break;
    case M_PATT: snprintf(out, n, "P%d", i + 1); break;
    }
}
static void pick_draw(pix *c) {
    fill(c, 0, STRIP, TAB_X, H - STRIP, BG);
    if (pick.menu == PM_DROP) {
        const char *url = drop_url();
        text_in(c, 0, STRIP + 40, TAB_X, F14_H, url[0] ? "On your phone, on this Wi-Fi, open" : "Join a Wi-Fi network first", url[0] ? INK : DIM, 0);
        text_in(c, 0, STRIP + 90, TAB_X, F14_H, url, pal[4], 0);
        text_in(c, 0, STRIP + 140, TAB_X, F14_H, "Return to cancel", DIM, 0);
        return;
    }
    for (int r = 0; r < PICK_ROWS && pick.top + r < pick.n; ++r) {
        int i = pick.top + r, y = STRIP + r * ROW_H;
        char b[300];
        if (i == pick.cur) fill(c, PAD_X, y, TAB_X - 2 * PAD_X, ROW_H - 2, mix(PADBG, pal[ui.sound], 140));
        snprintf(b, sizeof b, pick.row[i].kind == 'k' ? "Kit: %s" : strchr("dS98be", pick.row[i].kind) ? "%s/" : "%s", pick.row[i].name);
        fit(b, TAB_X - 2 * PAD_X - 12, 0);
        text(c, PAD_X + 6, y + (ROW_H - 2 - F14_H) / 2, b, pick.row[i].kind == 'k' && !strcmp(pick.row[i].name, kit_name) ? pal[4] : INK, 0);
    }
    if (!pick.n) text_in(c, 0, STRIP, TAB_X, H - STRIP, "Nothing to load here", DIM, 0);
}
static void strip(pix *c, long long now) {
    fill(c, 0, 0, W, STRIP, 0xff000000);
    /* play state: a triangle or a square */
    if (seq.playing) for (int r = 0; r < 12; ++r) fill(c, 14, 8 + r, r < 6 ? r * 2 : (11 - r) * 2, 1, INK);
    else fill(c, 14, 9, 10, 10, DIM);
    if (seq.rec) for (int r = 0; r < 10; ++r) { int d = r - 4, w = d * d < 20 ? 2 * (int)(4.5f + __builtin_sqrtf(20 - d * d) - 4.5f) : 0; fill(c, 40 - w / 2, 9 + r, w, 1, 0xffff3030); }
    char b[48];
    snprintf(b, sizeof b, "%d", seq.bpm);
    text(c, 62, 3, b, ui.wheel_to == WT_BPM ? pal[0] : INK, 1);
    snprintf(b, sizeof b, "P%d%s", seq.cur + 1, seq.cue >= 0 ? ">" : "");
    if (seq.cue >= 0) snprintf(b + strlen(b), sizeof b - strlen(b), "%d", seq.cue + 1);
    text(c, 108, 3, b, DIM, 1);
    int name_x = 108 + text_w(b, 1) + 8 > 150 ? 108 + text_w(b, 1) + 8 : 150; /* a cue is wider */
    /* the wheel's name and value */
    const char *name = ""; int val = 0, show = 1, held = ui.pad >= 0 ? ui.pad : ui.tile;
    char sv[16] = "";
    tile_t t;
    if (ui.centre_hold) name = ui.mode == M_PATT ? "Swing" : "Volume", val = ui.mode == M_PATT ? seq.swing : ui.vol;
    else if (ui.wheel_to == WT_VOL) name = "Volume", val = ui.vol;
    else if (ui.wheel_to == WT_BPM) name = "BPM", val = seq.bpm;
    else if (pick.open) name = "Load", show = 0;
    else if (ui.mode == M_FX && ui.fx_page == FX_PAGES && ui.pad >= 0 && x0x_bus_tile(ui.pad, &t)) {
        name = t.name, val = x0x_bus_get(ui.pad);
        if (t.names) snprintf(sv, sizeof sv, "%s", t.names[val - t.min]);
    } else if (ui.mode == M_FX) name = "Page", val = ui.fx_page + 1;
    else if (ui.mode == M_KEYS && ui.keys_scale) name = "Scale", snprintf(sv, sizeof sv, "%s", seq_scale_name(seq.scale));
    else if (ui.mode == M_KEYS) name = "Key", note_name(seq.key, sv, sizeof sv);
    else if (ui.mode == M_PLAY && ui.pad >= 0) name = "Pitch", val = sounds[ui.pad].p[P_PITCH];
    else if (ui.mode == M_PLAY) name = "Sweep", show = 0;
    else if (ui.mode == M_SOUND && sound2_on()) {
        name = source_tile(ui.sound, held, &t) ? t.name : "-", show = 0;
        source_text(ui.sound, held, sv, sizeof sv);
    } else if (ui.mode == M_SOUND) name = param_names[held], val = sounds[ui.sound].p[held], show = held != P_LOAD;
    else if (ui.mode == M_WRITE && ui.layer) {
        const step_t *st = ui.pad >= 0 ? step_at(ui.pad) : 0;
        name = groove_names[ui.groove], show = st && ui.groove <= G_CHANCE;
        if (ui.groove == G_RATE) snprintf(sv, sizeof sv, "%s", seq_rate_name(seq.pat[seq.cur].rate[ui.sound]));
        else if (show) val = ui.groove == G_RATCHET ? (st->ratchet > 1 ? st->ratchet : 1) : (st->chance ? st->chance : 100);
    } else if (ui.mode == M_WRITE) {
        name = lock_names[ui.lock];
        if (ui.pad >= 0) {
            const step_t *st = step_at(ui.pad);
            signed char v = ui.lock == L_LEVEL ? st->level : ui.lock == L_PITCH ? st->pitch : ui.lock == L_DECAY ? st->decay : ui.lock == L_CUTOFF ? st->cutoff : st->nudge;
            int k = lock_param[ui.lock];
            val = v == UNSET && k >= 0 ? sounds[ui.sound].p[k] : v;
        } else val = lock_param[ui.lock] >= 0 ? sounds[ui.sound].p[lock_param[ui.lock]] : seq.pat[seq.cur].len[ui.sound];
        if (ui.lock == L_NUDGE && ui.pad < 0) name = "Length";
    } else if (ui.mode == M_PATT) name = "BPM", val = seq.bpm;
    if (now < ui.msg_until) snprintf(b, sizeof b, "%s", ui.msg);
    else if (sv[0]) snprintf(b, sizeof b, "%s %s", name, sv);
    else if (show) snprintf(b, sizeof b, "%s %d", name, val);
    else snprintf(b, sizeof b, "%s", name);
    fit(b, TAB_X - name_x, 1);
    text(c, name_x, 3, b, ui.centre_hold || ui.pad >= 0 ? pal[ui.sound] : INK, 1);
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
    int head = seq_playhead(ui.sound);
    for (int i = 0; i < NPAD; ++i) {
        int x = pad_x(i), y = pad_y(i);
        pix col = PADBG, ink = INK;
        char label[32], val[16];
        pad_label(i, label, sizeof label, val, sizeof val);
        if (ui.mode == M_PLAY) {
            unsigned since = blocks - engine_last_hit(i);
            if (engine_last_hit(i) && since < 12) col = mix(pal[i], 0xffffffff, (int)(256 - since * 20));
            else if (i == ui.sound) col = mix(PADBG, pal[i], 96);
            if (sounds[i].src == SRC_SAMPLE && !sounds[i].smp.pcm) ink = DIM;
        } else if (ui.mode == M_KEYS) {
            if (sounds[ui.sound].src != SRC_BREAK && seq_scale_note(i) % 12 == 0) col = mix(PADBG, pal[ui.sound], 96); /* the roots */
        } else if (ui.mode == M_WRITE) {
            const step_t *st = step_at(i);
            int in = ui.page * BAR + i < seq.pat[seq.cur].len[ui.sound];
            if (st->on) col = mix(PADBG, pal[ui.sound], in ? 200 : 70), ink = in ? 0xff000000 : DIM;
            else if (!in) ink = DIM;
        } else if (ui.mode == M_SOUND) {
            if (i == ui.tile) col = mix(PADBG, pal[ui.sound], 110);
            if (!sound2_on() && sounds[ui.sound].src != SRC_SAMPLE && i >= P_START && i <= P_REVERSE) ink = DIM; /* a sample's */
        } else if (ui.mode == M_FX) {
            int f = ui.fx_page == FX_PAGES ? 0 : fx_page[ui.fx_page][i];
            if (ui.fx_page == FX_PAGES) ;
            else if (f < 0) ink = DIM;
            else if (f == fx_sounding()) col = mix(PADBG, pal[i], 200), ink = 0xff000000;
        } else if (ui.mode == M_PATT) {
            if (i == seq.cur) col = mix(PADBG, pal[i], 160), ink = 0xff000000;
        } else ink = DIM;
        if (i == ui.pad) col = mix(col, 0xffffffff, 60);
        fill(c, x, y, PAD, PAD, col);
        if (ui.mode == M_WRITE) { /* the playhead's frame, a locked step's dot */
            const step_t *st = step_at(i);
            if (ui.page * BAR + i == head) frame(c, x, y, PAD, PAD, 3, INK);
            if (st->on && (st->level != UNSET || st->pitch != UNSET || st->decay != UNSET || st->cutoff != UNSET || st->nudge || st->ratchet > 1 || st->chance || st->flags))
                fill(c, x + PAD - 10, y + 4, 6, 6, INK);
        }
        if (ui.mode == M_PATT && i == seq.cue) frame(c, x, y, PAD, PAD, 2, INK);
        if (ui.mode == M_PATT) { /* its places in the song */
            char places[24] = "";
            for (int j = 0, l = 0; j < seq.nsong && l + 4 < (int)sizeof places; ++j)
                if (seq.song[j] == i) l += snprintf(places + l, sizeof places - (size_t)l, "%d ", j + 1);
            if (places[0]) frame(c, x - 2, y - 2, PAD + 4, PAD + 4, 2, pal[i]);
            fit(places, PAD - 4, 0);
            text_in(c, x, y + PAD - F14_H - 3, PAD, F14_H, places, ink, 0);
        }
        if (val[0]) {
            int big = text_w(val, 1) <= PAD;
            fit(label, PAD + 2, 0), fit(val, PAD + 2, big);
            text_in(c, x, y + 6, PAD, F14_H, label, ink, 0);
            text_in(c, x, y + 32, PAD, big ? F20_H : F14_H, val, ink, big);
        } else if (ui.mode == M_WRITE || ui.mode == M_PATT || (ui.mode == M_KEYS && sounds[ui.sound].src != SRC_BREAK))
            text_in(c, x, y, PAD, PAD, label, ink, 1);
        else { /* two words too wide for the pad take a line each */
            char *sp = strpbrk(label, " -"), second[32];
            if (sp && text_w(label, 0) > PAD + 2) {
                snprintf(second, sizeof second, "%s", sp + 1);
                sp[*sp == '-'] = 0; /* a hyphen stays on the first line */
                fit(label, PAD + 2, 0), fit(second, PAD + 2, 0);
                text_in(c, x, y + PAD / 2 - F14_H, PAD, F14_H, label, ink, 0);
                text_in(c, x, y + PAD / 2, PAD, F14_H, second, ink, 0);
            } else {
                fit(label, PAD + 2, 0);
                text_in(c, x, y, PAD, PAD, label, ink, 0);
            }
        }
    }
    if (pick.open) pick_draw(c);
    for (int m = 0; m < M_N; ++m) {
        int y = STRIP + m * TAB_H;
        fill(c, TAB_X, y, TAB_W, TAB_H - 2, m == ui.mode ? mix(PADBG, pal[ui.sound], 140) : 0xff1a1a1a);
        char tab[12], sub[24] = "";
        int pages = (seq.pat[seq.cur].len[ui.sound] + BAR - 1) / BAR;
        snprintf(tab, sizeof tab, "%s%s", mode_names[m], (m == M_WRITE && ui.layer) || (m == M_SOUND && sound2_on()) ? " 2" : "");
        if (m == M_WRITE && (ui.page || pages > 1)) snprintf(sub, sizeof sub, "%d/%d", ui.page + 1, pages > ui.page + 1 ? pages : ui.page + 1);
        pix tc = m == ui.mode ? 0xff000000 : DIM;
        text_in(c, TAB_X, y - (sub[0] ? 8 : 0), TAB_W, TAB_H - 2, tab, tc, 0);
        text_in(c, TAB_X, y + 10, TAB_W, TAB_H - 2, sub, tc, 0);
    }
    strip(c, now);
    if (ui.job) ui.job_shown = 1; /* the strip says it: now the job may stop the screen */
}

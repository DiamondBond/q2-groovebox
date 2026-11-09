/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The panel: what it shows is what it does. Start, Tempo and Volume across the top, four tabs along
 * the bottom, one line above them saying what the keys do now. A value is a labelled box: tap it,
 * turn the wheel. Everything drawn clears the rounded glass (check.c holds it to that). Centre
 * plays the sound or picks a row. Return goes back, or undoes where there is nowhere to go back
 * to; its hold is the firmware's quit and is never bound here. */
#include <ctype.h>
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
#define GLASS_R 80         /* the panel's corner radius, q2-pod's calibration */
#define EDGE 52            /* the top and bottom bands' side margin: they run where the corners bite */
#define SIDE 18            /* the body's */
#define BAND_H 27
#define TOP_Y 6
#define TABS_Y 287
#define BODY_Y 37
#define BODY_H 217
#define BODY_W (W - 2 * SIDE - 1)
#define HINT_Y 255
#define HINT_H 30
#define GAP 3
#define PILL_GAP 5
#define PILL_W 87
#define TAB_W 64
#define LEFT_W 94
#define GRID_X 116
#define CELL_W 58
#define CELL_H 52
#define BOX_X 117
#define BOX_W 78
#define BOX_H 70
#define BOXES 9            /* a page of them */
#define ARROW_W 33
#define ARROW_X 287
#define LIST_ROWS 5
#define LIST_ROW 30
#define SMALL_Y (BODY_Y + LIST_ROWS * LIST_ROW) /* the two buttons under the list */
#define SMALL_H ((BODY_Y + BODY_H - SMALL_Y - GAP) / 2)
#define ROW_H 31           /* the picker's rows */
#define PICK_ROWS 7
#define PICK_MAX 512       /* ponytail: the first 512 of a folder are listed */
#define DETENT 13          /* wheel units a step of a value: sixteen a turn of 200 */
#define DETENT_MOVE 28     /* a step between sounds, rows or pages: seven a turn, Q2 Pod's lists at their default */
#define WHEEL_REST_MS 250 /* the firmware sends no lift: a gap this long is a new touch */
#define HOLD_MS 300        /* a step down this long opens */
#define IDLE_QUIT_MS (5 * 60 * 1000)
#define MSG_MS 2000        /* how long a result stays on the hint line */
#define REGIONS 15         /* a file's start points, SOUND_SECONDS apart */
#define ASK_ROWS 3         /* the rows between the first start point and the rest */
#define DROP_PORT 8642
#define MACH 100           /* a box's id past this is a machine's or the bus's tile */
#define BUSY_TILE 14       /* a synth's or a 303's "how many notes" */
#define EMPTY_STEP ((step_t){ 0, UNSET, UNSET, UNSET, UNSET, 0, 0, 0, 0 })

enum { PADS_DRUMS, PADS_NOTES, PADS_FX };
enum { O_NONE, O_SOUND, O_STEP, O_TRACK, O_SET }; /* the pages of boxes: Sound's is its tab, the rest open over one */
enum { S_NOTE, S_LEVEL, S_DECAY, S_FILTER, S_ROLL, S_CHANCE, S_ACCENT, S_SLIDE, S_NUDGE, S_N };
enum { T_STEPS, T_SPEED, T_CLEAR, T_UNDO, T_MAKE, T_VARY, T_BUSY };
enum { X_SWING, X_KEY, X_SCALE };
enum { WT_MODE, WT_BPM, WT_VOL }; /* the wheel's lent to Tempo or Volume while their button is lit */
/* the slow job ui_tick is to run: a picked row's kind */
enum { J_NONE, J_KIT = 'k', J_PAD = 'r', J_CHOP = 'c', J_HITS = 'h', J_BREAK = 'B', J_BUILTIN = 'g', J_PADBREAK = 't',
       J_SYNTH = 'y', J_VOICE = 'v', J_BASS = '3', J_EXPORT = 'x' };
enum { PM_ROOT, PM_KITS, PM_MACHINE, PM_DIR, PM_ASK, PM_DROP, PM_ENGINE, PM_PRESET, PM_VOICE, PM_BREAK }; /* the picker's screens */
enum { R_KITS, R_MACHINE, R_SYNTHS, R_BASS, R_LOOPS, R_CARD, R_PHONE }; /* its first screen's rows */

static struct {
    int mode, over;     /* the tab; the page of boxes open over it */
    int pads, fx_page;  /* the Pads tab's view and its page of effects */
    int sound, page, step, list_top; /* the selected sound, its page of steps, the step open, the list's first row */
    int box, box_page;  /* the lit box and its page */
    int wheel_to, vol, acc, sweep; /* vol 0..100, the soft volume; acc wheel units since the last detent */
    int pad;            /* the cell under the finger, -1 */
    long long pad_at, touch_at, last_input, dirty_at, wheel_at;
    int dirty, quit;
    int job, job_shown, job_from;
    char job_arg[32], msg[64];
    long long msg_until;
    pthread_mutex_t lock_m;
} ui;

static struct {
    int open, n, cur, top, menu, asked_cur, scan, scan_cur, engine, voice_src, drop_on;
    char dir[640], file[900];
    struct { char kind, name[256]; } row[PICK_MAX]; /* see pick_choose */
} pick;

volatile unsigned ui_beat;
void ui_lock(void) { pthread_mutex_lock(&ui.lock_m); }
void ui_unlock(void) { pthread_mutex_unlock(&ui.lock_m); }
int ui_quit(void) { return ui.quit; }
int ui_dirty(void) { return ui.dirty && ui.dirty_at; }
void ui_saved(void) { ui.dirty = 0; }
int ui_lively(long long now) { return seq.playing || now - ui.touch_at < 500; }
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
static int boxes_on(void) { return ui.over ? ui.over : ui.mode == M_SOUND ? O_SOUND : O_NONE; }
static void set_gain(void) { engine_gain(plat_start_gain() / 100 * ui.vol * ui.vol / 100); } /* 65536 x 10000 fits an int */
static void say(const char *m, long long until) {
    snprintf(ui.msg, sizeof ui.msg, "%s", m);
    ui.msg_until = until;
}
static void hear(int pad) { if (!seq.playing) engine_note(pad, UNSET, UNSET, UNSET, UNSET, 0, 0); }
static step_t *step_cur(void) { return &seq.pat[seq.cur].s[ui.sound][ui.step]; }
static void hear_step(void) {
    const step_t *st = step_cur();
    if (!seq.playing) engine_note(ui.sound, st->level, st->pitch, st->decay, st->cutoff, st->flags, 0);
}
static int track_pages(void) { return (seq.pat[seq.cur].len[ui.sound] + BAR - 1) / BAR; }
static void transport(int on) {
    seq_play(on);
    if (!on && seq.rec) seq_rec(0); /* Record is for a running beat: Stop ends it */
}

/* ---- geometry ---- */
static int cell_x(int i) { return GRID_X + (i % 4) * (CELL_W + GAP); }
static int cell_y(int i) { return BODY_Y + (i / 4) * (CELL_H + GAP); } /* reading order, 1 top-left */
static int box_x(int i) { return BOX_X + (i % 3) * (BOX_W + GAP); }
static int box_y(int i) { return BODY_Y + (i / 3) * (BOX_H + GAP); }
static int left_h(int n) { return (BODY_H - (n - 1) * GAP) / n; }
static int left_y(int n, int k) { return BODY_Y + k * (left_h(n) + GAP); }
static int small_y(int k) { return SMALL_Y + (k - 3) * (SMALL_H + GAP); } /* slots 3 and 4 */
static int pill_x(int i) { return EDGE + i * (PILL_W + PILL_GAP); }
static int tab_x(int i) { return EDGE + i * (TAB_W + PILL_GAP); }
static int left_count(void) { return !ui.over && ui.mode == M_PADS ? 4 : 5; }
static int list_shown(void) { return !ui.over && (ui.mode == M_BEAT || ui.mode == M_SOUND); }

/* ---- the sound list ---- */
static int boxes(int *id);
static void select_sound(int i) {
    if (i != ui.sound && !seq.playing) engine_stop(ui.sound); /* the last one's tail must not cover this one */
    ui.sound = i;
    if (i < ui.list_top) ui.list_top = i;
    if (i >= ui.list_top + LIST_ROWS) ui.list_top = i - LIST_ROWS + 1;
    if (ui.mode == M_SOUND) { /* a different kind of sound has different boxes */
        int id[32];
        if (ui.box >= boxes(id)) ui.box = ui.box_page = 0;
    }
    hear(i);
}

/* ---- slow jobs: asked for under the lock, run from ui_tick off it, once the hint line has said so ---- */
static void job_start(int job, const char *m, long long now) {
    ui.job = job, ui.job_shown = 0;
    say(m, now + 3600000);
}
static void job_run(long long now) {
    int job = ui.job, r = 1, from = ui.job_from;
    char name[32] = "";
    ui.job = 0, ui_beat = 0;
    switch (job) {
    case J_KIT:
        if (kit_choose(ui.job_arg)) {
            pads_forget(project_path());
            snprintf(ui.msg, sizeof ui.msg, "Loaded %s", kit_name);
            changed(now);
            ui_lock();
            hear(ui.sound);
            ui_unlock();
        } else snprintf(ui.msg, sizeof ui.msg, "Can't read it");
        break;
    case J_EXPORT:
        if (export_pattern(name, sizeof name)) snprintf(ui.msg, sizeof ui.msg, "Saved Music/Groovebox/%s", name);
        else snprintf(ui.msg, sizeof ui.msg, "Couldn't save it");
        ui_lock(); /* the engine started over */
        seq_fx_end();
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
        if (r) {
            changed(now);
            ui_lock();
            hear(ui.sound);
            ui_unlock();
        }
    }
    ui.msg_until = plat_ms() + MSG_MS; /* a long job has outlived now */
}

/* ---- the picker: what a sound can be, the kits, and the card's folders and files ---- */
static int row_rank(char k) { return k == 'd' ? 0 : 1; }
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
static void pick_move(int n) {
    if (!pick.n) return;
    pick.cur = clampi(pick.cur + n, 0, pick.n - 1);
    if (pick.cur < pick.top) pick.top = pick.cur;
    if (pick.cur >= pick.top + PICK_ROWS) pick.top = pick.cur - PICK_ROWS + 1;
}
static void pick_scan(void) { /* off the lock: a big folder on the card must not hold up the sound */
    int first = 0, menu = pick.menu;
    pick_menu(menu);
    if (menu == PM_KITS) {
        char kits[300];
        pick_add('k', KIT_909_LIVE);
        pick_add('k', KIT_808_LIVE);
        first = pick.n;
        snprintf(kits, sizeof kits, "%s/kits", plat_dir());
        DIR *k = opendir(kits);
        for (struct dirent *e; k && (e = readdir(k));)
            if (e->d_name[0] != '.' && is_dir(kits, e)) pick_add('k', e->d_name);
        if (k) closedir(k);
    } else {
        DIR *d = opendir(pick.dir);
        for (struct dirent *e; d && pick.n < PICK_MAX && (e = readdir(d));) {
            if (e->d_name[0] == '.') continue;
            if (is_dir(pick.dir, e)) pick_add('d', e->d_name);
            else if (is_audio(e->d_name)) pick_add('f', e->d_name);
        }
        if (d) closedir(d);
    }
    qsort(pick.row + first, (size_t)(pick.n - first), sizeof pick.row[0], row_cmp);
}
static void pick_rescan(int menu, int cur) { pick.n = 0, pick.menu = menu, pick.scan = 1, pick.scan_cur = cur; } /* ui_tick does it */
static void pick_root(int cur) {
    pick_menu(PM_ROOT);
    pick_add('K', "Drum kits (all 16 sounds)");
    pick_add('M', "Single drums");
    pick_add('S', "Synths");
    pick_add('3', "303 bass");
    pick_add('b', "Drum loops");
    pick_add('m', "Music on the card");
    pick_add('p', "From your phone");
    pick_move(cur);
}
static void pick_open(void) {
    snprintf(pick.dir, sizeof pick.dir, "%s", plat_media());
    pick_root(0);
    pick.open = 1;
}
static void pick_ask(void) { /* a file picked: part of it on this sound, or all of it across the sounds */
    char b[64], nm[24];
    snprintf(nm, sizeof nm, "%s", sounds[ui.sound].smp.name[0] ? sounds[ui.sound].smp.name : "this sound");
    pick.asked_cur = pick.cur;
    pick_menu(PM_ASK);
    snprintf(b, sizeof b, "Use on %s", nm);
    pick_add('r', b);
    pick_add('h', "Slice at its hits, all 16 sounds");
    pick_add('c', "Slice evenly, all 16 sounds");
    pick_add('B', "Use as a drum loop");
    for (int i = 1; i < REGIONS; ++i) {
        int s = i * SOUND_SECONDS;
        snprintf(b, sizeof b, "Use from %d:%02d", s / 60, s % 60);
        pick_add('r', b);
    }
}
static void pick_machines(int cur) {
    pick_menu(PM_MACHINE);
    pick_add('9', "909");
    pick_add('8', "808");
    pick_move(cur);
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
    if (sounds[ui.sound].smp.pcm) pick_add('t', "This sound as a loop");
}
static void pick_back(void) {
    switch (pick.menu) {
    case PM_ROOT: pick.open = 0; break;
    case PM_KITS: pick_root(R_KITS); break;
    case PM_MACHINE: pick_root(R_MACHINE); break;
    case PM_ENGINE: pick_root(R_SYNTHS); break;
    case PM_BREAK: pick_root(R_LOOPS); break;
    case PM_DROP: pick_root(R_PHONE); break;
    case PM_PRESET: pick_engines(), pick_move(pick.engine); break;
    case PM_VOICE: pick_machines(pick.voice_src == SRC_808); break;
    case PM_ASK: pick_rescan(PM_DIR, pick.asked_cur); break;
    default:
        if (!strcmp(pick.dir, plat_media())) pick_root(R_CARD);
        else *strrchr(pick.dir, '/') = 0, pick_rescan(PM_DIR, 0);
    }
}
static void pick_choose(long long now) {
    if (!pick.n) return;
    const char *name = pick.row[pick.cur].name;
    size_t l = strlen(pick.dir);
    int kind = pick.row[pick.cur].kind;
    switch (kind) {
    case 'K': pick_rescan(PM_KITS, 0); break;
    case 'M': pick_machines(0); break;
    case 'm': snprintf(pick.dir, sizeof pick.dir, "%s", plat_media()), pick_rescan(PM_DIR, 0); break;
    case 'k':
        snprintf(ui.job_arg, sizeof ui.job_arg, "%s", name);
        pick.open = 0;
        job_start(J_KIT, "Loading", now);
        break;
    case 'd':
        if (l + 1 + strlen(name) < sizeof pick.dir) snprintf(pick.dir + l, sizeof pick.dir - l, "/%.255s", name), pick_rescan(PM_DIR, 0);
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
    default: /* r a part of the file, c and h slices, B a loop, y a preset, v a drum, 3 the bass, g and t a loop */
        ui.job_from = kind == 'r' ? (pick.cur ? pick.cur - ASK_ROWS : 0) : pick.cur;
        if (kind != 'y' && kind != 'v') pick.open = 0; /* a preset or a drum stays open, to try the next */
        job_start(kind, "Loading", now);
    }
}

/* ---- the boxes ---- */
typedef struct { char name[16], val[24]; int cur, min, max, tap; } box_t; /* tap: a tap does it, the wheel doesn't */
static const char *const param_label[P_N] = { "Volume", "Pan", "Pitch", "Fine", "Start", "End", "Loop", "Reverse",
                                              "Attack", "Decay", "Filter", "Reso", "Echo", "Reverb", "Choke", "" };
static const int step_param[4] = { P_PITCH, P_LEVEL, P_DECAY, P_CUTOFF }; /* S_NOTE to S_FILTER */
static signed char *step_field(step_t *st, int id) { return id == S_NOTE ? &st->pitch : id == S_LEVEL ? &st->level : id == S_DECAY ? &st->decay : &st->cutoff; }
static void note_name(int semis, char *out, size_t n) { /* semitones from C3 */
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int m = ((semis % 12) + 12) % 12;
    snprintf(out, n, "%s%d", names[m], 3 + (semis - m) / 12);
}
static int boxes(int *id) { /* the open page's boxes, in order: how many */
    static const int sample[] = { P_LEVEL, P_PITCH, P_PAN, P_DECAY, P_CUTOFF, P_RESO, P_ECHO, P_REVERB, P_ATTACK,
                                  P_FINE, P_START, P_END, P_LOOP, P_REVERSE, P_CHOKE };
    static const int lead[] = { P_LEVEL, P_PITCH, P_ECHO, P_REVERB, P_PAN }, tail[] = { P_DECAY, P_CUTOFF, P_RESO, P_ATTACK, P_FINE, P_CHOKE };
    const sound_t *s = &sounds[ui.sound];
    tile_t t;
    int n = 0;
    switch (boxes_on()) {
    case O_STEP:
        for (; n < S_N; ++n) id[n] = n;
        break;
    case O_TRACK:
        for (; n <= T_UNDO; ++n) id[n] = n;
        if (s->src == SRC_SYNTH || s->src == SRC_303) id[n++] = T_MAKE, id[n++] = T_VARY, id[n++] = T_BUSY;
        break;
    case O_SET:
        for (; n <= X_SCALE; ++n) id[n] = n;
        for (int i = 0; i < BUS_TILES; ++i) id[n++] = MACH + i;
        break;
    default:
        if (s->src == SRC_SAMPLE) {
            for (size_t i = 0; i < sizeof sample / sizeof *sample; ++i) id[n++] = sample[i];
            break;
        }
        if (s->src == SRC_SYNTH) id[n++] = MACH + SY_PRESET; /* a synth's first thing to change */
        for (size_t i = 0; i < sizeof lead / sizeof *lead; ++i) id[n++] = lead[i];
        for (int i = 0; i < NTILE; ++i)
            if (source_tile(ui.sound, i, &t) && !t.action && !(s->src == SRC_SYNTH && i == SY_PRESET)) id[n++] = MACH + i;
        for (size_t i = 0; i < sizeof tail / sizeof *tail; ++i) id[n++] = tail[i];
    }
    return n;
}
static void box_read(int id, box_t *b) {
    static const char *const step_names[S_N] = { "Note", "Volume", "Decay", "Filter", "Roll", "Chance", "Accent", "Slide", "Nudge" };
    static const char *const track_names[] = { "Steps", "Speed", "Clear steps", "Undo", "Make tune", "Vary tune", "Busy" };
    static const char *const set_names[] = { "Swing", "Key", "Scale" };
    const sound_t *s = &sounds[ui.sound];
    const pattern_t *p = &seq.pat[seq.cur];
    step_t *st = step_cur();
    tile_t t;
    memset(b, 0, sizeof *b);
    switch (boxes_on()) {
    case O_STEP:
        snprintf(b->name, sizeof b->name, "%s", step_names[id]);
        if (id <= S_FILTER) {
            int k = step_param[id], v = *step_field(st, id);
            b->cur = v == UNSET ? s->p[k] : v, b->min = param_min[k], b->max = param_max[k];
            if (id == S_NOTE) note_name(b->cur, b->val, sizeof b->val);
            else snprintf(b->val, sizeof b->val, "%d", b->cur);
        } else if (id == S_ROLL) {
            b->min = 1, b->max = 8, b->cur = st->ratchet > 1 ? st->ratchet : 1;
            if (b->cur > 1) snprintf(b->val, sizeof b->val, "x%d", b->cur);
            else snprintf(b->val, sizeof b->val, "Off");
        } else if (id == S_CHANCE) {
            b->min = 5, b->max = 100, b->cur = st->chance ? st->chance : 100;
            if (st->chance) snprintf(b->val, sizeof b->val, "%d%%", st->chance);
            else snprintf(b->val, sizeof b->val, "Always");
        } else if (id == S_NUDGE) {
            b->min = -127, b->max = 127, b->cur = st->nudge;
            snprintf(b->val, sizeof b->val, "%d", b->cur);
        } else {
            b->tap = 1;
            snprintf(b->val, sizeof b->val, "%s", st->flags & (id == S_ACCENT ? NOTE_ACCENT : NOTE_SLIDE) ? "On" : "Off");
        }
        break;
    case O_TRACK:
        snprintf(b->name, sizeof b->name, "%s", track_names[id]);
        if (id == T_STEPS) b->min = 1, b->max = NSTEP, b->cur = p->len[ui.sound], snprintf(b->val, sizeof b->val, "%d", b->cur);
        else if (id == T_SPEED) b->min = RATE_MIN, b->max = RATE_MAX, b->cur = p->rate[ui.sound], snprintf(b->val, sizeof b->val, "%s", seq_rate_name(b->cur));
        else if (id == T_BUSY) b->min = 5, b->max = 100, b->cur = clampi(s->e[BUSY_TILE], 5, 100), snprintf(b->val, sizeof b->val, "%d", b->cur);
        else b->tap = 1;
        break;
    case O_SET:
        if (id >= MACH) {
            x0x_bus_tile(id - MACH, &t);
            b->min = t.min, b->max = t.max, b->cur = x0x_bus_get(id - MACH);
            snprintf(b->name, sizeof b->name, "%s", t.name);
            if (t.names) snprintf(b->val, sizeof b->val, "%s", t.names[b->cur - t.min]);
            else snprintf(b->val, sizeof b->val, "%d", b->cur);
            break;
        }
        snprintf(b->name, sizeof b->name, "%s", set_names[id]);
        if (id == X_SWING) b->max = 100, b->cur = seq.swing, snprintf(b->val, sizeof b->val, "%d", b->cur);
        else if (id == X_KEY) b->min = -24, b->max = 24, b->cur = seq.key, note_name(seq.key, b->val, sizeof b->val);
        else b->max = NSCALE - 1, b->cur = seq.scale, snprintf(b->val, sizeof b->val, "%s", seq_scale_name(seq.scale));
        break;
    default:
        if (id >= MACH) {
            source_tile(ui.sound, id - MACH, &t);
            b->min = t.min, b->max = t.max, b->cur = source_get(ui.sound, id - MACH);
            snprintf(b->name, sizeof b->name, "%s", t.name);
            source_text(ui.sound, id - MACH, b->val, sizeof b->val);
            break;
        }
        b->min = param_min[id], b->max = param_max[id], b->cur = s->p[id];
        snprintf(b->name, sizeof b->name, "%s", param_label[id]);
        if (id == P_LOOP || id == P_REVERSE) snprintf(b->val, sizeof b->val, "%s", b->cur ? "On" : "Off");
        else snprintf(b->val, sizeof b->val, "%d", b->cur);
    }
}
static void turn_bus(int i, int n) {
    tile_t t;
    if (x0x_bus_tile(i, &t)) x0x_bus_set(i, clampi(x0x_bus_get(i) + n, t.min, t.max));
}
static void step_turn(int id, int n) {
    step_t *st = step_cur();
    if (id <= S_FILTER) {
        signed char *v = step_field(st, id);
        int k = step_param[id];
        *v = (signed char)clampi((*v == UNSET ? sounds[ui.sound].p[k] : *v) + n, param_min[k], param_max[k]);
    } else if (id == S_ROLL) st->ratchet = (signed char)clampi((st->ratchet > 1 ? st->ratchet : 1) + n, 1, 8);
    else if (id == S_CHANCE) {
        int v = clampi((st->chance ? st->chance : 100) + n * 5, 5, 100);
        st->chance = (signed char)(v == 100 ? 0 : v);
    } else if (id == S_NUDGE) st->nudge = (signed char)clampi(st->nudge + n * 8, -127, 127);
    else return;
    st->on = 1;
}
static void box_turn(int id, int n) {
    pattern_t *p = &seq.pat[seq.cur];
    sound_t *s = &sounds[ui.sound];
    switch (boxes_on()) {
    case O_STEP: step_turn(id, n); break;
    case O_TRACK:
        if (id == T_STEPS) p->len[ui.sound] = (unsigned char)clampi(p->len[ui.sound] + n, 1, NSTEP);
        else if (id == T_SPEED) p->rate[ui.sound] = (signed char)clampi(p->rate[ui.sound] + n, RATE_MIN, RATE_MAX);
        else if (id == T_BUSY) source_turn(ui.sound, BUSY_TILE, n);
        break;
    case O_SET:
        if (id >= MACH) turn_bus(id - MACH, n);
        else if (id == X_SWING) seq.swing = clampi(seq.swing + n * 5, 0, 100);
        else if (id == X_KEY) seq.key = clampi(seq.key + n, -24, 24);
        else seq.scale = clampi(seq.scale + n, 0, NSCALE - 1);
        break;
    default:
        if (id >= MACH) source_turn(ui.sound, id - MACH, n);
        else s->p[id] = clampi(s->p[id] + n, param_min[id], param_max[id]);
    }
}
static void undo(long long now) {
    if (seq.ntake || seq.gen.valid) seq_undo(), say("Undone", now + MSG_MS);
    else if (ui.mode == M_PADS && ui.pads == PADS_FX && seq.pat[seq.cur].nev) seq.pat[seq.cur].nev = 0, say("This beat's effects cleared", now + MSG_MS);
    else say("Nothing to undo", now + MSG_MS);
    changed(now);
}
static void track_clear(long long now) {
    pattern_t *p = &seq.pat[seq.cur];
    memcpy(seq.gen.s, p->s[ui.sound], sizeof seq.gen.s); /* Undo's way back */
    seq.gen.pat = seq.cur, seq.gen.pad = ui.sound, seq.gen.valid = 1, seq.gen.len = p->len[ui.sound];
    seq.ntake = 0;
    for (int i = 0; i < NSTEP; ++i) p->s[ui.sound][i] = EMPTY_STEP;
    say("Steps cleared. Undo brings them back", now + MSG_MS);
}
static void box_tap(int id, long long now) { /* one that does something rather than holds a value */
    if (ui.over == O_STEP) step_cur()->flags ^= id == S_ACCENT ? NOTE_ACCENT : NOTE_SLIDE, step_cur()->on = 1;
    else if (id == T_CLEAR) track_clear(now);
    else if (id == T_UNDO) undo(now);
    else { /* Make tune, Vary tune */
        seq.ntake = 0;
        source_line(ui.sound, id == T_VARY, (unsigned)now ^ engine_blocks());
        say("Tune written. Undo takes it back", now + MSG_MS);
    }
    changed(now);
}
static void box_touch(int cell, long long now) {
    int id[32], n = boxes(id), at = ui.box_page * BOXES + cell;
    box_t b;
    if (at >= n) return;
    box_read(id[at], &b);
    if (b.tap) box_tap(id[at], now); /* it has no value for the wheel, so it takes no light */
    else ui.box = at;
}
static void box_next(void) {
    int id[32], pages = (boxes(id) + BOXES - 1) / BOXES;
    ui.box_page = (ui.box_page + 1) % pages;
    ui.box = ui.box_page * BOXES;
}
static void open_over(int over) { ui.over = over, ui.box = ui.box_page = 0; }

/* ---- the wheel ---- */
static int detents(int d, int unit) { /* units to whole steps, the remainder kept */
    ui.acc += d;
    int n = 0;
    while (ui.acc >= unit) ui.acc -= unit, ++n;
    while (ui.acc <= -unit) ui.acc += unit, --n;
    return n;
}
static void wheel(int d, long long now) {
    d = -d; /* the firmware's travel rises anticlockwise; here clockwise is up */
    if (now - ui.wheel_at >= WHEEL_REST_MS || (d ^ ui.acc) < 0) ui.acc = 0; /* as the firmware's lists: a new touch or a reversal starts a whole step away */
    ui.wheel_at = now;
    if (pick.open) { pick_move(detents(d, DETENT_MOVE)); return; }
    if (ui.wheel_to == WT_MODE && !boxes_on() && ui.mode == M_PADS && ui.pads == PADS_DRUMS) { /* the sweep: continuous, a turn end to end */
        ui.sweep = clampi(ui.sweep + d, -100, 100);
        engine_sweep(abs(ui.sweep) < 4 ? 0 : ui.sweep); /* a rest at the centre */
        return;
    }
    int n = detents(d, ui.wheel_to != WT_MODE || ui.mode == M_SONG || boxes_on() ? DETENT : DETENT_MOVE);
    if (!n) return;
    if (ui.wheel_to == WT_VOL) ui.vol = clampi(ui.vol + n * 5, 0, 100), set_gain();
    else if (ui.wheel_to == WT_BPM || (ui.mode == M_SONG && !ui.over)) seq.bpm = clampi(seq.bpm + n, 40, 240), changed(now);
    else if (boxes_on()) {
        int id[32], count = boxes(id);
        if (ui.box < count) {
            box_turn(id[ui.box], n), changed(now);
            if (ui.over == O_STEP) hear_step();
        }
    } else if (ui.mode == M_PADS && ui.pads == PADS_FX) ui.fx_page = clampi(ui.fx_page + n, 0, FX_PAGES - 1);
    else if (ui.pad < 0 && ui.sound != clampi(ui.sound + n, 0, NPAD - 1)) select_sound(clampi(ui.sound + n, 0, NPAD - 1)); /* a held note keeps its sound */
}

/* ---- the grid ---- */
static void fx_stop(long long now) {
    fx_live(-1);
    if (seq_fx_end()) changed(now);
}
static void fx_down(int i, long long now) {
    int f = fx_page[ui.fx_page][i];
    if (f < 0) return;
    fx_live(f);
    seq_fx_start(f);
    if (seq.fxrec.on) changed(now);
}
static void keys_down(int i, long long now) { /* the scale, or a break's slices and tricks */
    if (sounds[ui.sound].src != SRC_BREAK) {
        seq_note(ui.sound, seq.key + seq_scale_note(i));
        if (seq.rec) changed(now);
    } else if (i < 11) x0x_break_live(ui.sound, i, 1);
}
static void keys_up(int i) {
    if (sounds[ui.sound].src != SRC_BREAK) engine_release(ui.sound);
    else if (i < 11) x0x_break_live(ui.sound, i, 0);
}
static void cell_down(int i, long long now) {
    if (ui.pad >= 0) return;
    if (ui.mode == M_BEAT && ui.page * BAR + i >= seq.pat[seq.cur].len[ui.sound]) return; /* past the track's end */
    ui.pad = i;
    ui.pad_at = now;
    if (ui.mode == M_SONG) seq_cue(i);
    else if (ui.mode == M_PADS && ui.pads == PADS_DRUMS) {
        ui.sound = i;
        if (i < ui.list_top) ui.list_top = i;
        if (i >= ui.list_top + LIST_ROWS) ui.list_top = i - LIST_ROWS + 1;
        seq_tap(i);
        if (seq.rec) changed(now);
    } else if (ui.mode == M_PADS && ui.pads == PADS_NOTES) keys_down(i, now);
    else if (ui.mode == M_PADS) fx_down(i, now);
}
static void cell_up(int i, long long now) {
    ui.pad = -1;
    if (ui.mode == M_BEAT) { /* on the lift: a hold opens the step instead */
        step_t *st = &seq.pat[seq.cur].s[ui.sound][ui.page * BAR + i];
        if (st->on) *st = EMPTY_STEP;
        else st->on = 1, hear(ui.sound);
        changed(now);
    } else if (ui.mode == M_PADS && ui.pads == PADS_DRUMS) engine_release(i);
    else if (ui.mode == M_PADS && ui.pads == PADS_NOTES) keys_up(i);
    else if (ui.mode == M_PADS) fx_stop(now);
}

/* ---- the column beside the grid ---- */
static int beat_empty(int i) {
    const pattern_t *p = &seq.pat[i];
    if (p->nev) return 0;
    for (int t = 0; t < NPAD; ++t)
        for (int s = 0; s < NSTEP; ++s)
            if (p->s[t][s].on) return 0;
    return 1;
}
static void beat_copy(long long now) {
    char m[32];
    for (int k = 1; k < NPATT; ++k) {
        int to = (seq.cur + k) % NPATT;
        if (!beat_empty(to)) continue;
        seq.pat[to] = seq.pat[seq.cur];
        seq_cue(to);
        snprintf(m, sizeof m, "Copied to Beat %d", to + 1);
        say(m, now + MSG_MS);
        changed(now);
        return;
    }
    say("No empty beat left", now + MSG_MS);
}
/* slot k's label and whether it is lit; "" none */
static int left_label(int k, char *out, size_t n) {
    static const char *const pads[4] = { "Drums", "Notes", "Effects", "Record" };
    static const char *const song[5] = { "Add to song", "Remove last", "Copy beat", "Save WAV", "Settings" };
    int id[32], pages = 0, lit = 0;
    const char *s = "";
    if (boxes_on()) pages = (boxes(id) + BOXES - 1) / BOXES;
    if (ui.over == O_STEP) s = k == 3 && step_cur()->on ? "Remove step" : k == 4 ? "Hear it" : "";
    else if (ui.over || ui.mode == M_SOUND) {
        if (k == 3 && ui.mode == M_SOUND && !ui.over) s = "Load sound";
        else if (k == 4 && pages > 1) { snprintf(out, n, "More %d/%d", ui.box_page + 1, pages); return 0; }
    }
    else if (ui.mode == M_BEAT) s = k == 3 ? "Load sound" : k == 4 ? "Options" : "";
    else if (ui.mode == M_PADS) s = pads[k], lit = k == 3 ? seq.rec : k == ui.pads;
    else s = song[k];
    snprintf(out, n, "%s", s);
    return lit;
}
static void left_tap(int k, long long now) {
    int pages = 0, id[32];
    if (boxes_on()) pages = (boxes(id) + BOXES - 1) / BOXES;
    if (ui.over == O_STEP) {
        if (k == 3 && step_cur()->on) *step_cur() = EMPTY_STEP, ui.over = 0, changed(now);
        else if (k == 4) hear_step();
    } else if (ui.over || ui.mode == M_SOUND) {
        if (k == 3 && !ui.over) pick_open();
        else if (k == 4 && pages > 1) box_next();
    } else if (ui.mode == M_BEAT) {
        if (k == 3) pick_open();
        else if (k == 4) open_over(O_TRACK);
    } else if (ui.mode == M_PADS) {
        if (k < 3) {
            if (ui.sweep) ui.sweep = 0, engine_sweep(0);
            ui.pads = k;
        } else if (seq.rec) seq_rec(0);
        else {
            seq_rec(1);
            if (!seq.playing) seq_play(1); /* a take needs a running beat to land on */
        }
    } else if (k == 0) seq_song_add(seq.cue >= 0 ? seq.cue : seq.cur), changed(now);
    else if (k == 1) seq_song_pop(), changed(now);
    else if (k == 2) beat_copy(now);
    else if (k == 3) { if (!ui.job) job_start(J_EXPORT, "Saving", now); }
    else open_over(O_SET);
}
static void left_touch(int y, long long now) {
    int n = left_count();
    if (!list_shown()) left_tap(clampi((y - BODY_Y) / (left_h(n) + GAP), 0, n - 1), now);
    else if (y < SMALL_Y) select_sound(ui.list_top + clampi((y - BODY_Y) / LIST_ROW, 0, LIST_ROWS - 1));
    else left_tap(y < small_y(4) ? 3 : 4, now);
}

/* ---- touches and keys ---- */
static void go_tab(int t) {
    ui.mode = t, ui.box = ui.box_page = 0;
    if (ui.sweep) ui.sweep = 0, engine_sweep(0); /* a sweep left on would have nothing on screen saying so */
}
static void back(long long now) {
    if (pick.open) pick_back();
    else if (ui.over) ui.over = 0;
    else undo(now);
}
static void touch_down(int x, int y, long long now) {
    if (y < BODY_Y - 1) { /* Start, Tempo, Volume */
        int i = clampi((x - EDGE) / (PILL_W + PILL_GAP), 0, 2);
        if (!i) transport(!seq.playing);
        else ui.wheel_to = ui.wheel_to == i ? WT_MODE : i;
        return;
    }
    ui.wheel_to = WT_MODE; /* anything else hands the wheel back */
    if (y >= TABS_Y - 1) {
        if (pick.open || ui.over) back(now);
        else go_tab(clampi((x - EDGE) / (TAB_W + PILL_GAP), 0, M_N - 1));
    } else if (pick.open) {
        int shown = (y - BODY_Y) / ROW_H, row = pick.top + shown;
        if (y < BODY_Y + BODY_H && shown < PICK_ROWS && row < pick.n) pick.cur = row, pick_choose(now);
    } else if (y >= BODY_Y + BODY_H) { /* the line above the tabs holds Beat's page arrows */
        if (x >= ARROW_X && ui.mode == M_BEAT && !ui.over && track_pages() > 1)
            ui.page = clampi(ui.page + (x < ARROW_X + ARROW_W + 2 ? -1 : 1), 0, track_pages() - 1);
    } else if (x < GRID_X - 2) left_touch(y, now);
    else if (boxes_on()) box_touch(clampi((y - BODY_Y) / (BOX_H + GAP), 0, 2) * 3 + clampi((x - BOX_X) / (BOX_W + GAP), 0, 2), now);
    else cell_down(clampi((y - BODY_Y) / (CELL_H + GAP), 0, 3) * 4 + clampi((x - GRID_X) / (CELL_W + GAP), 0, 3), now); /* no dead gaps */
}
static void key(char what, int code, long long now) {
    int down = what == 'd';
    if (code == KEY_RETURN) { /* on the release: a hold's never comes (the firmware's quit) */
        if (!down) back(now);
    } else if (!down || (pick.open && code != KEY_CENTRE && code != KEY_PLAY)) return;
    else if (code == KEY_PLAY) transport(!seq.playing);
    else if (code == KEY_CENTRE) {
        if (pick.open) pick_choose(now);
        else if (ui.over == O_STEP) hear_step();
        else hear(ui.sound);
    } else if (code == KEY_PREV || code == KEY_NEXT) {
        int n = code == KEY_NEXT ? 1 : -1;
        if (ui.over) return;
        if (ui.mode == M_BEAT && track_pages() > 1) ui.page = clampi(ui.page + n, 0, track_pages() - 1);
        else if (ui.mode == M_SOUND) { if (ui.sound != clampi(ui.sound + n, 0, NPAD - 1)) select_sound(clampi(ui.sound + n, 0, NPAD - 1)); }
        else seq_cue((seq.cue >= 0 ? seq.cue : seq.cur) + n);
    }
}
void ui_event(const event_t *e, long long now) {
    if (e->kind == 'q') { ui.quit = 1; return; }
    if (ui.job) return; /* a queued job reads the picker: nothing moves it till it has run */
    ui.last_input = now;
    ui_lock();
    if (e->kind == 'w') wheel(e->a, now);
    else if (e->kind == 'k') key(e->what, e->a, now);
    else if (e->kind == 't') {
        ui.touch_at = now;
        if (e->what == 'd') touch_down(e->a, e->b, now);
        else if (e->what == 'u' && ui.pad >= 0) cell_up(ui.pad, now); /* one finger: whatever was down lifts, wherever it lifts */
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
    if (ui.pad >= 0 && ui.mode == M_BEAT && !ui.over && now - ui.pad_at >= HOLD_MS) {
        ui_lock(); /* a held step opens */
        ui.step = ui.page * BAR + ui.pad;
        ui.pad = -1;
        open_over(O_STEP);
        ui_unlock();
    }
    if (!seq.playing && !pick.drop_on && now - ui.last_input > IDLE_QUIT_MS) ui.quit = 1;
}

/* ---- drawing ---- */
#define BG 0xff101010
#define PADBG 0xff282828
#define BEATBG 0xff3a3a3a  /* the first step of each beat */
#define OFFBG 0xff1a1a1a   /* past the track's end */
#define SEL 0xffeeeeec     /* lit, as the firmware's selection */
#define INK 0xffe8e8e8
#define DIM 0xff8c8c8c
#define BLACK 0xff000000
#define RED 0xffd83a3a
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
static void rrect(pix *c, int x, int y, int w, int h, int r, pix col) { /* a rounded box, its corners smoothed */
    for (int j = 0; j < h; ++j) {
        int d = j < r ? r - j : j >= h - r ? j - (h - r) + 1 : 0; /* rows into the corner */
        float in = d ? (float)r - __builtin_sqrtf((float)(r * r) - ((float)d - 0.5f) * ((float)d - 0.5f)) : 0;
        int whole = (int)in, a = (int)((1 - (in - (float)whole)) * 256);
        pix *p = c + (y + j) * W + x;
        p[whole] = mix(p[whole], col, a);
        p[w - 1 - whole] = mix(p[w - 1 - whole], col, a);
        fill(c, x + whole + 1, y + j, w - 2 * (whole + 1), 1, col);
    }
}
static void tri(pix *c, int x, int y, int s, int left, pix col) { /* s wide, 2s high, pointing sideways */
    for (int r = 0; r < 2 * s; ++r) {
        int w = r < s ? r + 1 : 2 * s - r;
        fill(c, left ? x + s - w : x, y + r, w, 1, col);
    }
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
static void label_in(pix *c, int x, int y, int w, int h, const char *s, pix col) { /* two words too wide take a line each */
    char a[40], b[40] = "", *sp;
    snprintf(a, sizeof a, "%s", s);
    if (text_w(a, 0) > w - 2 && (sp = strpbrk(a, " -"))) {
        snprintf(b, sizeof b, "%s", sp + 1);
        sp[*sp == '-'] = 0; /* a hyphen stays on the first line */
    }
    fit(a, w - 2, 0), fit(b, w - 2, 0);
    if (b[0]) {
        text_in(c, x, y + h / 2 - F14_H, w, F14_H, a, col, 0);
        text_in(c, x, y + h / 2, w, F14_H, b, col, 0);
    } else text_in(c, x, y, w, h, a, col, 0);
}
static const pix pal[NPAD] = { 0xffe05050, 0xffe08a40, 0xffe0c040, 0xffa0d040, 0xff50d060, 0xff40d0a0,
                               0xff40c0d0, 0xff4090e0, 0xff5060e0, 0xff9050e0, 0xffd050d0, 0xffe05090,
                               0xffc08060, 0xff80a080, 0xff6080b0, 0xffb0b0b0 };

static void hint_text(char *b, size_t n) {
    char nm[24];
    snprintf(nm, sizeof nm, "%s", sounds[ui.sound].smp.name[0] ? sounds[ui.sound].smp.name : "this sound");
    if (ui.wheel_to == WT_BPM) snprintf(b, n, "Wheel: tempo. Tap Tempo when done");
    else if (ui.wheel_to == WT_VOL) snprintf(b, n, "Wheel: volume. Tap Volume when done");
    else if (pick.open && pick.menu == PM_DROP) snprintf(b, n, "Waiting for your phone");
    else if (pick.open) snprintf(b, n, "For %s. Wheel and Centre, or tap", nm);
    else if (boxes_on()) snprintf(b, n, ui.over == O_SET || ui.over == O_TRACK ? "Tap a box, turn the wheel" : "Tap a box, turn the wheel. Centre: hear it");
    else if (ui.mode == M_BEAT && track_pages() > 1) {
        int len = seq.pat[seq.cur].len[ui.sound], last = (ui.page + 1) * BAR;
        snprintf(b, n, "Steps %d-%d of %d", ui.page * BAR + 1, last < len ? last : len, len);
    } else if (ui.mode == M_BEAT) snprintf(b, n, "Tap to add a beat. Hold to edit. Wheel: sound");
    else if (ui.mode == M_PADS && seq.rec) snprintf(b, n, "Recording. Return undoes it");
    else if (ui.mode == M_PADS && ui.pads == PADS_DRUMS) snprintf(b, n, "Tap to play. Wheel: filter sweep");
    else if (ui.mode == M_PADS && ui.pads == PADS_NOTES) snprintf(b, n, "%s notes. Wheel: change sound", nm);
    else if (ui.mode == M_PADS) snprintf(b, n, "Hold a pad for its effect. Wheel: page %d/%d", ui.fx_page + 1, FX_PAGES);
    else if (!seq.nsong) snprintf(b, n, "Tap a beat to play it. Wheel: tempo");
    else {
        int l = snprintf(b, n, "Song:");
        for (int i = 0; i < seq.nsong && l + 4 < (int)n; ++i) l += snprintf(b + l, n - (size_t)l, " %d", seq.song[i] + 1);
    }
}
static int hint_w(void) { return !ui.over && !pick.open && ui.mode == M_BEAT && track_pages() > 1 ? ARROW_X - SIDE - 4 : BODY_W; }
static void hint_draw(pix *c, long long now) {
    char b[120];
    int w = hint_w(), live = now < ui.msg_until;
    if (live) snprintf(b, sizeof b, "%s", ui.msg);
    else hint_text(b, sizeof b);
    fit(b, w, 0);
    text_in(c, SIDE, HINT_Y, w, HINT_H, b, live ? INK : DIM, 0);
    if (w == BODY_W) return;
    for (int i = 0; i < 2; ++i) { /* Beat's pages */
        int x = ARROW_X + i * (ARROW_W + 4), on = i ? ui.page < track_pages() - 1 : ui.page > 0;
        rrect(c, x, HINT_Y + 1, ARROW_W, BAND_H, 7, PADBG);
        tri(c, x + ARROW_W / 2 - 3, HINT_Y + BAND_H / 2 - 5, 6, !i, on ? INK : mix(PADBG, DIM, 80));
    }
}
static void button(pix *c, int x, int y, int w, int h, const char *s, int lit, pix on) {
    rrect(c, x, y, w, h, 8, lit ? on : PADBG);
    label_in(c, x, y, w, h, s, lit ? BLACK : INK);
}
static void top_draw(pix *c) {
    char b[24];
    pix bg = !seq.playing ? PADBG : seq.rec ? RED : SEL, ink = !seq.playing ? INK : BLACK;
    const char *word = seq.playing ? "Stop" : "Start";
    int tw = text_w(word, 0), x = pill_x(0) + (PILL_W - tw - 16) / 2;
    rrect(c, pill_x(0), TOP_Y, PILL_W, BAND_H, 8, bg);
    if (seq.playing) fill(c, x, TOP_Y + 8, 11, 11, ink);
    else tri(c, x, TOP_Y + 8, 11 / 2 + 1, 0, ink);
    text(c, x + 16, TOP_Y + (BAND_H - F14_H) / 2, word, ink, 0);
    snprintf(b, sizeof b, "Tempo %d", seq.bpm);
    button(c, pill_x(1), TOP_Y, PILL_W, BAND_H, b, ui.wheel_to == WT_BPM, SEL);
    snprintf(b, sizeof b, "Volume %d", ui.vol);
    button(c, pill_x(2), TOP_Y, PILL_W, BAND_H, b, ui.wheel_to == WT_VOL, SEL);
}
static void bottom_draw(pix *c) {
    if (pick.open || ui.over) {
        button(c, EDGE, TABS_Y, W - 2 * EDGE, BAND_H, pick.open ? "Back" : "Done", 0, SEL);
        return;
    }
    static const char *const names[M_N] = { "", "Pads", "Sound", "Song" };
    for (int m = 0; m < M_N; ++m) {
        char b[24];
        if (m == M_BEAT) snprintf(b, sizeof b, "Beat %d", seq.cur + 1);
        else snprintf(b, sizeof b, "%s", names[m]);
        button(c, tab_x(m), TABS_Y, TAB_W, BAND_H, b, m == ui.mode, SEL);
    }
}
static void list_draw(pix *c) {
    unsigned blocks = engine_blocks();
    const pattern_t *p = &seq.pat[seq.cur];
    for (int r = 0; r < LIST_ROWS; ++r) {
        int i = ui.list_top + r, y = BODY_Y + r * LIST_ROW, lit = i == ui.sound, any = 0, hit = engine_last_hit(i) && blocks - engine_last_hit(i) < 12;
        char b[32];
        for (int s = 0; s < NSTEP; ++s) any |= p->s[i][s].on;
        rrect(c, SIDE, y, LEFT_W, LIST_ROW - 2, 7, lit ? SEL : PADBG);
        rrect(c, SIDE + 5, y + 7, 6, LIST_ROW - 16, 3, hit ? 0xffffffff : any ? pal[i] : mix(PADBG, pal[i], 70));
        snprintf(b, sizeof b, "%s", sounds[i].smp.name[0] ? sounds[i].smp.name : "Empty");
        fit(b, LEFT_W - 22, 0);
        text(c, SIDE + 16, y + (LIST_ROW - 2 - F14_H) / 2, b, lit ? BLACK : any ? INK : DIM, 0);
    }
}
static void left_draw(pix *c) {
    int n = left_count(), h = left_h(n);
    char b[32];
    if (list_shown()) list_draw(c);
    else if (ui.over) { /* what the boxes are of */
        char title[24], sub[32];
        if (ui.over == O_STEP) snprintf(title, sizeof title, "Step %d", ui.step + 1);
        else snprintf(title, sizeof title, "%s", ui.over == O_TRACK ? "Track" : "Settings");
        if (ui.over == O_SET) snprintf(sub, sizeof sub, "Whole song");
        else snprintf(sub, sizeof sub, "%s", sounds[ui.sound].smp.name);
        fit(title, LEFT_W, 1), fit(sub, LEFT_W, 0);
        text_in(c, SIDE, BODY_Y + 28, LEFT_W, F20_H, title, INK, 1);
        text_in(c, SIDE, BODY_Y + 62, LEFT_W, F14_H, sub, DIM, 0);
    }
    for (int k = 0; k < n; ++k) {
        int lit = left_label(k, b, sizeof b);
        if (!b[0]) continue;
        if (list_shown()) button(c, SIDE, small_y(k), LEFT_W, SMALL_H, b, lit, SEL);
        else button(c, SIDE, left_y(n, k), LEFT_W, h, b, lit, k == 3 && n == 4 ? RED : SEL);
    }
}
static void boxes_draw(pix *c) {
    int id[32], n = boxes(id);
    for (int i = 0; i < BOXES && ui.box_page * BOXES + i < n; ++i) {
        int at = ui.box_page * BOXES + i, x = box_x(i), y = box_y(i), lit = at == ui.box;
        pix bg = lit ? SEL : PADBG, ink = lit ? BLACK : INK;
        box_t b;
        box_read(id[at], &b);
        rrect(c, x, y, BOX_W, BOX_H, 9, bg);
        if (!b.val[0]) { label_in(c, x, y, BOX_W, BOX_H, b.name, ink); continue; }
        fit(b.name, BOX_W - 6, 0);
        text_in(c, x, y + 6, BOX_W, F14_H, b.name, ink, 0);
        int big = text_w(b.val, 1) <= BOX_W - 6;
        fit(b.val, BOX_W - 6, big);
        text_in(c, x, y + 26, BOX_W, big ? F20_H : F14_H, b.val, ink, big);
        if (!b.tap && b.max > b.min) { /* where in its range */
            int w = BOX_W - 16;
            fill(c, x + 8, y + BOX_H - 10, w, 3, lit ? 0xffb8b8b6 : 0xff484848);
            fill(c, x + 8, y + BOX_H - 10, w * (b.cur - b.min) / (b.max - b.min), 3, lit ? BLACK : pal[ui.sound]);
        }
    }
}
static void cell_label(int i, char *out, size_t n) {
    static const char *const tricks[3] = { "Reverse", "Half", "Stutter" };
    out[0] = 0;
    if (ui.mode == M_SONG) snprintf(out, n, "%d", i + 1);
    else if (ui.pads == PADS_DRUMS) snprintf(out, n, "%s", sounds[i].smp.name[0] ? sounds[i].smp.name : "Empty");
    else if (ui.pads == PADS_FX) {
        snprintf(out, n, "%s", fx_page[ui.fx_page][i] >= 0 ? fx_name(fx_page[ui.fx_page][i]) : "");
        for (char *q = out + 1; out[0] && *q; ++q) /* LOOP 8T: Loop 8T, the way the rest of the panel writes */
            if (isalpha((unsigned char)q[-1])) *q = (char)tolower((unsigned char)*q);
    }
    else if (sounds[ui.sound].src != SRC_BREAK) note_name(seq.key + seq_scale_note(i), out, n);
    else if (i < 8) snprintf(out, n, "Slice %d", i + 1);
    else if (i < 11) snprintf(out, n, "%s", tricks[i - 8]);
}
static void grid_draw(pix *c) {
    unsigned blocks = engine_blocks();
    const pattern_t *p = &seq.pat[seq.cur];
    int head = seq_playhead(ui.sound);
    for (int i = 0; i < NPAD; ++i) {
        int x = cell_x(i), y = cell_y(i), big = 0, frame = 0, detail = 0;
        pix col = PADBG, ink = INK;
        char label[32];
        if (ui.mode == M_BEAT) {
            int at = ui.page * BAR + i;
            const step_t *st = &p->s[ui.sound][at];
            if (at >= p->len[ui.sound]) { rrect(c, x, y, CELL_W, CELL_H, 8, OFFBG); continue; }
            col = st->on ? pal[ui.sound] : i % 4 ? PADBG : BEATBG;
            ink = st->on ? BLACK : DIM;
            if (st->on && st->pitch != UNSET) note_name(st->pitch, label, sizeof label);
            else snprintf(label, sizeof label, "%d", at + 1);
            frame = at == head;
            detail = st->on && (st->level != UNSET || st->decay != UNSET || st->cutoff != UNSET || st->nudge || st->ratchet > 1 || st->chance || st->flags);
        } else {
            cell_label(i, label, sizeof label);
            if (ui.mode == M_SONG) {
                big = 1;
                ink = beat_empty(i) ? DIM : INK;
                if (i == seq.cur) col = SEL, ink = BLACK;
                frame = i == seq.cue;
            } else if (ui.pads == PADS_DRUMS) {
                unsigned since = blocks - engine_last_hit(i);
                col = mix(PADBG, pal[i], 64);
                if (engine_last_hit(i) && since < 12) col = mix(pal[i], 0xffffffff, (int)(256 - since * 20)), ink = BLACK;
                else if (!sounds[i].smp.name[0]) ink = DIM;
            } else if (ui.pads == PADS_FX) {
                int f = fx_page[ui.fx_page][i];
                if (f >= 0 && f == fx_sounding()) col = SEL, ink = BLACK;
            } else if (sounds[ui.sound].src != SRC_BREAK && seq_scale_note(i) % 12 == 0) col = mix(PADBG, pal[ui.sound], 110); /* the roots */
        }
        if (i == ui.pad) col = mix(col, 0xffffffff, 60);
        if (frame) rrect(c, x, y, CELL_W, CELL_H, 8, INK), rrect(c, x + 3, y + 3, CELL_W - 6, CELL_H - 6, 6, col);
        else rrect(c, x, y, CELL_W, CELL_H, 8, col);
        if (detail) fill(c, x + CELL_W - 12, y + 7, 5, 5, BLACK); /* more than on or off */
        if (big) text_in(c, x, y, CELL_W, CELL_H, label, ink, 1);
        else label_in(c, x, y, CELL_W, CELL_H, label, ink);
    }
}
static void pick_draw(pix *c) {
    if (pick.menu == PM_DROP) {
        const char *url = drop_url();
        text_in(c, SIDE, BODY_Y + 40, BODY_W, F14_H, url[0] ? "On your phone, on this Wi-Fi, open" : "Join a Wi-Fi network first", url[0] ? INK : DIM, 0);
        text_in(c, SIDE, BODY_Y + 90, BODY_W, F14_H, url, pal[4], 0);
        return;
    }
    for (int r = 0; r < PICK_ROWS && pick.top + r < pick.n; ++r) {
        int i = pick.top + r, y = BODY_Y + r * ROW_H, lit = i == pick.cur, more = !!strchr("KMSmpbde98f", pick.row[i].kind);
        char b[300];
        rrect(c, SIDE, y, BODY_W, ROW_H - 2, 8, lit ? SEL : PADBG);
        snprintf(b, sizeof b, "%s", pick.row[i].name);
        if (pick.row[i].kind == 'k') b[0] = (char)toupper((unsigned char)b[0]); /* a kit's folder name */
        fit(b, BODY_W - 40, 0);
        text(c, SIDE + 12, y + (ROW_H - 2 - F14_H) / 2, b, lit ? BLACK : pick.row[i].kind == 'k' && !strcmp(pick.row[i].name, kit_name) ? pal[4] : INK, 0);
        if (more) tri(c, SIDE + BODY_W - 18, y + (ROW_H - 2) / 2 - 5, 5, 0, lit ? BLACK : DIM);
    }
    if (!pick.n && !pick.scan) text_in(c, SIDE, BODY_Y, BODY_W, BODY_H, "Nothing to load here", DIM, 0);
}
void ui_render(pix *c, long long now) {
    ui.page = clampi(ui.page, 0, track_pages() - 1); /* a shorter track or the next beat may have taken this page away */
    for (int i = 0; i < W * H; ++i) c[i] = BG;
    top_draw(c);
    if (pick.open) pick_draw(c);
    else {
        if (boxes_on()) boxes_draw(c);
        else grid_draw(c);
        left_draw(c);
    }
    hint_draw(c, now);
    bottom_draw(c);
    if (ui.job) ui.job_shown = 1; /* the hint line says it: now the job may stop the screen */
}

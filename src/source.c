/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The pads' sources: the sample a pad held goes (or stays, for a break), the machine starts on the
 * defaults or the project's values. SOUND 2 reads and turns them through here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "project.h"
#include "seq.h"
#include "source.h"
#include "synth.h"
#include "ui.h"
#include "x0x.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void source_assign(int pad, int src, int sub, int preset, const int *e, const char *name) {
    int v[NTILE];
    if (e) memcpy(v, e, sizeof v);
    else x0x_defaults(src, sub, v);
    sound_swap(pad, 0, 0, name, 0);
    ui_lock();
    sound_t *s = &sounds[pad];
    s->src = src, s->sub = sub, s->preset = preset;
    memcpy(s->e, v, sizeof s->e);
    if (src == SRC_SYNTH) synth_kill(pad), synth_apply(pad, v);
    else x0x_assign(pad, src, sub, v);
    ui_unlock();
}
void source_synth(int pad, int engine, int preset) {
    int v[NTILE] = { 0 }, echo, reverb;
    char name[32];
    synth_preset(engine, preset, v, &echo, &reverb);
    v[SY_NEW] = 70;
    snprintf(name, sizeof name, "%s", synth_preset_name(engine, preset));
    source_assign(pad, SRC_SYNTH, engine, preset, v, name);
    ui_lock();
    sounds[pad].p[P_ECHO] = echo, sounds[pad].p[P_REVERB] = reverb;
    ui_unlock();
}
int source_break(int pad, int bars) {
    sound_t *s = &sounds[pad];
    int e[NTILE];
    if (!s->smp.pcm) return 0;
    x0x_defaults(SRC_BREAK, 0, e);
    e[14] = bars ? bars : x0x_break_guess(s->smp.n);
    if (!x0x_break_load(pad, s->smp.pcm, s->smp.n, e[14], e)) return 0;
    ui_lock();
    s->src = SRC_BREAK, s->sub = s->preset = 0;
    memcpy(s->e, e, sizeof s->e);
    pattern_t *p = &seq.pat[seq.cur]; /* an empty track starts playing it: a loop that sits silent looks broken */
    int any = 0;
    for (int i = 0; i < NSTEP; ++i) any |= p->s[pad][i].on;
    if (!any)
        for (int i = 0; i < p->len[pad]; ++i) p->s[pad][i].on = 1;
    ui_unlock();
    return 1;
}
void source_kit(int src) {
    for (int i = 0; i < NPAD; ++i) {
        int sub, e[NTILE], choke;
        const char *name;
        x0x_kit(src, i, &sub, e, &name, &choke);
        source_assign(i, src, sub, 0, e, name);
        ui_lock();
        sounds[i].p[P_CHOKE] = choke;
        ui_unlock();
    }
}
/* out of a card file: anything out of range and the pad stays as it is */
void source_restore(int pad, int src, int sub, int preset, const int *e) {
    int v[NTILE] = { 0 };
    tile_t t;
    char name[32] = "";
    if (src == SRC_SYNTH) {
        if (sub < 0 || sub >= SYNTH_ENGINES || preset < 0 || preset >= synth_presets(sub)) return;
        snprintf(name, sizeof name, "%s", synth_preset_name(sub, preset));
    } else if (src == SRC_909 || src == SRC_808) {
        if (sub < 0 || sub > 10) return;
        for (int i = 0; i < NPAD; ++i) { /* the kit's name for it */
            int s, c, tmp[NTILE];
            const char *nm;
            x0x_kit(src, i, &s, tmp, &nm, &c);
            if (s == sub && !name[0]) snprintf(name, sizeof name, "%s", nm);
            if (i == pad && s == sub) { snprintf(name, sizeof name, "%s", nm); break; }
        }
    } else if (src == SRC_303) snprintf(name, sizeof name, "303");
    else if (src != SRC_BREAK) return;
    for (int i = 0; i < NTILE; ++i) {
        v[i] = e[i];
        int ok = src == SRC_SYNTH ? synth_tile(pad, sub, i, &t) : x0x_tile(src, sub, i, &t);
        if (ok) v[i] = clampi(v[i], t.min, t.max);
        else v[i] = 0;
    }
    if (src == SRC_BREAK) {
        if (!source_break(pad, v[14])) return;
        ui_lock();
        memcpy(sounds[pad].e, v, sizeof v);
        for (int i = 0; i < NTILE; ++i) if (x0x_tile(src, 0, i, &t)) x0x_set(pad, src, 0, i, v[i]);
        ui_unlock();
        return;
    }
    source_assign(pad, src, sub, preset, v, name);
}
void source_restart_all(void) {
    for (int i = 0; i < NPAD; ++i) {
        sound_t *s = &sounds[i];
        if (s->src == SRC_SYNTH) synth_kill(i);
        else if (s->src == SRC_BREAK) x0x_break_stop(i);
        else if (s->src != SRC_SAMPLE) x0x_assign(i, s->src, s->sub, s->e);
    }
}

int source_tile(int pad, int i, tile_t *t) {
    const sound_t *s = &sounds[pad];
    if (s->src == SRC_SYNTH) return synth_tile(pad, s->sub, i, t);
    if (s->src != SRC_SAMPLE) return x0x_tile(s->src, s->sub, i, t);
    return 0;
}
int source_get(int pad, int i) {
    const sound_t *s = &sounds[pad];
    return s->src == SRC_SYNTH && i == SY_PRESET ? s->preset : s->e[i];
}
void source_turn(int pad, int i, int n) {
    sound_t *s = &sounds[pad];
    tile_t t;
    if (!source_tile(pad, i, &t) || (t.action == ACT_MUTATE)) return;
    if (t.action == ACT_NEW) n *= 5;
    int v = clampi(source_get(pad, i) + n, t.min, t.max);
    if (s->src == SRC_SYNTH && i == SY_PRESET) { /* the wheel walks the presets, their sends with them */
        int echo, reverb, keep = s->e[SY_NEW];
        s->preset = v;
        synth_preset(s->sub, v, s->e, &echo, &reverb);
        s->e[SY_NEW] = keep;
        s->p[P_ECHO] = echo, s->p[P_REVERB] = reverb;
        synth_apply(pad, s->e);
        snprintf(s->smp.name, sizeof s->smp.name, "%s", synth_preset_name(s->sub, v));
        return;
    }
    s->e[i] = v;
    if (t.action) return;
    if (s->src == SRC_SYNTH) synth_apply(pad, s->e);
    else x0x_set(pad, s->src, s->sub, i, v);
}
void source_text(int pad, int i, char *out, size_t n) {
    tile_t t;
    const sound_t *s = &sounds[pad];
    out[0] = 0;
    if (!source_tile(pad, i, &t) || t.action == ACT_MUTATE) return;
    int v = source_get(pad, i);
    if (s->src == SRC_SYNTH && i == SY_PRESET) snprintf(out, n, "%s", synth_preset_name(s->sub, v));
    else if (t.names && !t.action) snprintf(out, n, "%s", t.names[clampi(v, t.min, t.max) - t.min]);
    else snprintf(out, n, "%d", v);
}

void source_line(int pad, int mutate, unsigned entropy) {
    static unsigned rng;
    pattern_t *p = &seq.pat[seq.cur];
    const sound_t *s = &sounds[pad];
    x0x_step_t st[NSTEP];
    int len = p->len[pad], tile = s->src == SRC_SYNTH ? SY_NEW : 14;
    x0x_gen_t g = { clampi(s->e[tile], 5, 100), ((seq.key % 12) + 12) % 12, seq.scale, s->src == SRC_303 ? 1 : 2, x0x_seed(entropy) };
    memcpy(seq.gen.s, p->s[pad], sizeof seq.gen.s);
    seq.gen.pat = seq.cur, seq.gen.pad = pad, seq.gen.valid = 1, seq.gen.len = len;
    memset(st, 0, sizeof st);
    for (int i = 0; i < len; ++i) {
        const step_t *q = &p->s[pad][i];
        st[i].on = q->on, st[i].note = (unsigned char)(48 + (q->pitch == UNSET ? seq.key : q->pitch));
        st[i].accent = (q->flags & NOTE_ACCENT) != 0, st[i].slide = (q->flags & NOTE_SLIDE) != 0;
    }
    if (mutate) { if (!rng) rng = g.seed; x0x_mutate(st, len, &g, &rng); }
    else rng = g.seed, x0x_line(st, len, &g);
    for (int i = 0; i < len; ++i) {
        step_t *q = &p->s[pad][i];
        q->on = st[i].on;
        q->pitch = (signed char)clampi(st[i].note - 48, -24, 24);
        q->flags = (signed char)((st[i].accent ? NOTE_ACCENT : 0) | (st[i].slide ? NOTE_SLIDE : 0));
    }
}

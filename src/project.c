/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The project file, key=value lines: the tempo, the sounds' tiles, each pattern's steps as sixteen
 * characters a track, and a locked step's values. Written whole to .new and renamed, as the card
 * can drop out mid-write. The kit's WAVs are named by pad number; a loaded sound lives beside the
 * project as pad%02d.wav, read back after the kit. */
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "fx.h"
#include "platform.h"
#include "seq.h"
#include "ui.h"
#include "wav.h"
#include "project.h"

char kit_name[32] = "default";
static char pad_names[NPAD][32]; /* a loaded sound's label, from project.txt until its WAV is read */

/* Decoded with no lock held, swapped under it, the old buffer freed after, so a running voice never
 * reads freed memory and the audio thread never waits on a file. */
void sound_swap(int pad, short *pcm, int n, const char *name, int loaded) {
    sample_t *s = &sounds[pad].smp;
    ui_lock();
    engine_stop(pad);
    short *old = s->pcm;
    s->pcm = pcm, s->n = n;
    ui_unlock();
    free(old);
    s->loaded = loaded;
    snprintf(s->name, sizeof s->name, "%.31s", name);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int kit_load(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *names[64];
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < 64;) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".wav") && e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name);
    if (!n) return 0;
    int loaded = 0;
    for (int i = 0; i < NPAD; ++i) { /* a kit of fewer leaves the rest empty, not the old kit's */
        char path[300], label[32] = "";
        short *pcm = 0;
        int got = 0;
        if (i < n) {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            got = wav_read(path, &pcm);
            const char *nm = names[i];
            while (*nm >= '0' && *nm <= '9') ++nm; /* the pad number */
            while (*nm == ' ') ++nm;
            snprintf(label, sizeof label, "%.*s", (int)(strlen(nm) - 4), nm);
        }
        sound_swap(i, pcm, got, label, 0);
        loaded += got > 0;
    }
    for (int i = 0; i < n; ++i) free(names[i]);
    return loaded;
}

/* the name comes from project.txt or the kits folder: a plain name, never a path */
static int kit_exists(const char *name) {
    if (!name[0] || name[0] == '.' || strchr(name, '/')) return 0;
    char dir[300];
    snprintf(dir, sizeof dir, "%s/kits/%s", plat_dir(), name);
    return kit_load(dir) > 0;
}
int kit_choose(const char *name) {
    if (!kit_exists(name)) return 0;
    if (name != kit_name) snprintf(kit_name, sizeof kit_name, "%s", name);
    return 1;
}
void kit_open(const char *name) {
    if (!kit_choose(name) && !kit_choose("default")) snprintf(kit_name, sizeof kit_name, "default");
}
const char *project_path(void) {
    static char dir[300];
    snprintf(dir, sizeof dir, "%s/projects/1", plat_dir());
    return dir;
}
void pad_path(char *out, size_t n, const char *dir, int pad) { snprintf(out, n, "%s/pad%02d.wav", dir, pad + 1); }
void pads_restore(const char *dir) {
    for (int i = 0; i < NPAD; ++i) {
        char path[320];
        short *pcm;
        pad_path(path, sizeof path, dir, i);
        int n = wav_read(path, &pcm);
        if (n) sound_swap(i, pcm, n, pad_names[i][0] ? pad_names[i] : "Sample", 1);
    }
}
void pads_forget(const char *dir) {
    for (int i = 0; i < NPAD; ++i) {
        char path[320];
        pad_path(path, sizeof path, dir, i);
        unlink(path);
    }
}

static const char *const keys[P_N] = { "level", "pan", "pitch", "fine", "start", "end", "loop", "reverse",
                                       "attack", "decay", "cutoff", "reso", "echo", "reverb", "choke", "load" };
int project_save(const char *dir) {
    char path[256], tmp[260];
    snprintf(path, sizeof path, "%s/project.txt", dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "bpm=%d\nswing=%d\npattern=%d\nkit=%s\n", seq.bpm, seq.swing, seq.cur, kit_name);
    for (int i = 0; i < NPAD; ++i) {
        for (int k = 0; k < P_LOAD; ++k)
            if (sounds[i].p[k] != param_def[k]) fprintf(f, "pad%d.%s=%d\n", i + 1, keys[k], sounds[i].p[k]);
        if (sounds[i].smp.loaded) fprintf(f, "pad%d.name=%s\n", i + 1, sounds[i].smp.name);
    }
    for (int p = 0; p < NPATT; ++p) {
        if (seq.pat[p].nev) {
            fprintf(f, "p%d.fx=", p + 1);
            for (int e = 0; e < seq.pat[p].nev; ++e)
                fprintf(f, "%d:%d:%d ", seq.pat[p].ev[e].step, seq.pat[p].ev[e].len, seq.pat[p].ev[e].fx);
            fputc('\n', f);
        }
        for (int t = 0; t < NPAD; ++t) {
            const pattern_t *P = &seq.pat[p];
            int any = P->len[t] != NSTEP;
            char row[NSTEP + 1];
            for (int s = 0; s < NSTEP; ++s) row[s] = P->s[t][s].on ? 'x' : '.', any |= P->s[t][s].on;
            row[NSTEP] = 0;
            if (!any) continue;
            fprintf(f, "p%d.t%d=%s len=%d\n", p + 1, t + 1, row, P->len[t]);
            for (int s = 0; s < NSTEP; ++s) {
                const step_t *st = &P->s[t][s];
                if (st->level != UNSET || st->pitch != UNSET || st->decay != UNSET || st->cutoff != UNSET || st->nudge)
                    fprintf(f, "p%d.t%d.s%d=%d %d %d %d %d\n", p + 1, t + 1, s + 1, st->level, st->pitch, st->decay,
                            st->cutoff, st->nudge);
            }
        }
    }
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0; /* the card can drop out */
    return ok && rename(tmp, path) == 0;
}

int project_load(const char *dir) {
    char path[256], line[512]; /* a pattern's FX events on one line */
    snprintf(path, sizeof path, "%s/project.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        int a, b, c, n[5];
        if (!strcmp(k, "bpm")) { a = atoi(v); seq.bpm = a < 40 ? 40 : a > 240 ? 240 : a; }
        else if (!strcmp(k, "swing")) { a = atoi(v); seq.swing = a < 0 ? 0 : a > 100 ? 100 : a; }
        else if (!strcmp(k, "pattern")) { a = atoi(v); seq.cur = a < 0 || a >= NPATT ? 0 : a; }
        else if (!strcmp(k, "kit")) snprintf(kit_name, sizeof kit_name, "%.*s", (int)strcspn(v, "\r\n"), v);
        else if (sscanf(k, "pad%d.", &a) == 1 && a >= 1 && a <= NPAD) {
            const char *key = strchr(k, '.');
            if (!key++) continue;
            if (!strcmp(key, "name")) snprintf(pad_names[a - 1], sizeof pad_names[0], "%.*s", (int)strcspn(v, "\r\n"), v);
            for (int i = 0; i < P_LOAD; ++i)
                if (!strcmp(key, keys[i])) {
                    int x = atoi(v);
                    sounds[a - 1].p[i] = x < param_min[i] ? param_min[i] : x > param_max[i] ? param_max[i] : x;
                }
        } else if (sscanf(k, "p%d.fx", &a) == 1 && strstr(k, ".fx") && a >= 1 && a <= NPATT) {
            pattern_t *P = &seq.pat[a - 1];
            P->nev = 0;
            for (int used; P->nev < NFXEV && sscanf(v, "%d:%d:%d %n", n, n + 1, n + 2, &used) == 3; v += used)
                if (n[0] >= 0 && n[0] < NSTEP && n[1] >= 1 && n[1] <= NSTEP - n[0] && n[2] >= 0 && n[2] < NFX) {
                    P->ev[P->nev].step = (signed char)n[0], P->ev[P->nev].len = (signed char)n[1];
                    P->ev[P->nev++].fx = (signed char)n[2];
                }
        } else if (sscanf(k, "p%d.t%d.s%d", &a, &b, &c) == 3 && a >= 1 && a <= NPATT && b >= 1 && b <= NPAD &&
                   c >= 1 && c <= NSTEP && sscanf(v, "%d %d %d %d %d", n, n + 1, n + 2, n + 3, n + 4) == 5) {
            step_t *st = &seq.pat[a - 1].s[b - 1][c - 1];
            st->level = (signed char)n[0], st->pitch = (signed char)n[1], st->decay = (signed char)n[2];
            st->cutoff = (signed char)n[3], st->nudge = (signed char)n[4];
        } else if (sscanf(k, "p%d.t%d", &a, &b) == 2 && a >= 1 && a <= NPATT && b >= 1 && b <= NPAD) {
            pattern_t *P = &seq.pat[a - 1];
            for (int s = 0; s < NSTEP && v[s] && v[s] != ' ' && v[s] != '\n'; ++s) P->s[b - 1][s].on = v[s] == 'x';
            const char *len = strstr(v, "len=");
            if (len) { c = atoi(len + 4); P->len[b - 1] = (unsigned char)(c < 1 ? 1 : c > NSTEP ? NSTEP : c); }
        }
    }
    fclose(f);
    return 1;
}

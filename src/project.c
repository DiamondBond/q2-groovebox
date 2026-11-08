/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The project file, key=value lines: the tempo, the sounds' tiles, each pattern's steps as sixteen
 * characters a track, and a locked step's values. Written whole to .new and renamed, as the card
 * can drop out mid-write. The kit's WAVs are named by pad number. */
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "seq.h"
#include "wav.h"
#include "project.h"

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
    int loaded = 0;
    for (int i = 0; i < n; ++i) {
        if (i < NPAD) {
            char path[256];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            sample_t *s = &sounds[i].smp;
            free(s->pcm);
            s->n = wav_read(path, &s->pcm);
            if (s->n) ++loaded;
            snprintf(s->file, sizeof s->file, "%s", path);
            const char *nm = names[i];
            while (*nm >= '0' && *nm <= '9') ++nm; /* the pad number */
            while (*nm == ' ') ++nm;
            snprintf(s->name, sizeof s->name, "%.*s", (int)(strlen(nm) - 4), nm);
        }
        free(names[i]);
    }
    return loaded;
}

static const char *const keys[P_N] = { "level", "pan", "pitch", "fine", "start", "end", "loop", "reverse",
                                       "attack", "decay", "cutoff", "reso", "echo", "reverb", "choke", "load" };
int project_save(const char *dir) {
    char path[256], tmp[260];
    snprintf(path, sizeof path, "%s/project.txt", dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "bpm=%d\nswing=%d\npattern=%d\n", seq.bpm, seq.swing, seq.cur);
    for (int i = 0; i < NPAD; ++i)
        for (int k = 0; k < P_LOAD; ++k)
            if (sounds[i].p[k] != param_def[k]) fprintf(f, "pad%d.%s=%d\n", i + 1, keys[k], sounds[i].p[k]);
    for (int p = 0; p < NPATT; ++p)
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
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0; /* the card can drop out */
    return ok && rename(tmp, path) == 0;
}

int project_load(const char *dir) {
    char path[256], line[256];
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
        else if (sscanf(k, "pad%d.", &a) == 1 && a >= 1 && a <= NPAD) {
            const char *key = strchr(k, '.');
            if (!key++) continue;
            for (int i = 0; i < P_LOAD; ++i)
                if (!strcmp(key, keys[i])) {
                    int x = atoi(v);
                    sounds[a - 1].p[i] = x < param_min[i] ? param_min[i] : x > param_max[i] ? param_max[i] : x;
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

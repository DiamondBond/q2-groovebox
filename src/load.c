/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The slow jobs, all on the UI thread with no lock held: a sound from a card file through the
 * rootfs's ffmpeg (no shell: card names hold quotes and $), one region to a pad or sixteen slices
 * across the kit, and the pattern or chain rendered to a WAV. ponytail: a job stops the screen until
 * it ends and the sound carries on, save an export, which has the engine to itself and silences it;
 * a worker thread if that grates. */
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "engine.h"
#include "load.h"
#include "platform.h"
#include "project.h"
#include "seq.h"
#include "source.h"
#include "ui.h"
#include "wav.h"
#include "x0x.h"

#define FFMPEG "/usr/bin/ffmpeg"
#define STALL_MS 30000 /* no output for this long and ffmpeg is given up on */
#define HOP 512        /* the transient finder's window, 12 ms */
#define MIN_GAP 8      /* hops between two hits: 90 ms */
#define MIN_SLICE (FS / 20)

/* <seconds> of file from <from> as mono FS: frames in *out, 0 and none on any failure */
static int decode(const char *file, int from, int seconds, short **out) {
    size_t cap = (size_t)seconds * FS * sizeof **out, got = 0;
    short *b = malloc(cap); /* untouched pages cost nothing */
    int fd[2];
    *out = 0;
    if (!b || pipe(fd)) return free(b), 0;
    char ss[16], t[16];
    snprintf(ss, sizeof ss, "%d", from);
    snprintf(t, sizeof t, "%d", seconds);
    const char *argv[] = { FFMPEG, "-nostdin", "-v", "quiet", "-ss", ss, "-t", t, "-i", file,
                           "-f", "s16le", "-ar", "44100", "-ac", "1", "-", 0 };
    pid_t pid = fork();
    if (pid < 0) return close(fd[0]), close(fd[1]), free(b), 0;
    if (!pid) { /* only async-signal-safe calls until the exec */
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(fd[1], 1), dup2(null, 2);
        close(fd[0]), close(fd[1]);
        execv(FFMPEG, (char *const *)argv);
        _exit(127);
    }
    close(fd[1]);
    struct pollfd p = { fd[0], POLLIN, 0 };
    while (got < cap && poll(&p, 1, STALL_MS) > 0) {
        ssize_t n = read(fd[0], (char *)b + got, cap - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd[0]);
    kill(pid, SIGKILL); /* full, stalled or finished, either way it is done */
    waitpid(pid, 0, 0);
    int frames = (int)(got / sizeof *b);
    if (frames < 2) return free(b), 0;
    short *fit = realloc(b, (size_t)frames * sizeof *b);
    *out = fit ? fit : b;
    return frames;
}

/* into the pad and onto the card: 1 once saved. The Start and End of the last sound mean nothing to
 * this one. Not saved, it lasts this session, and no older WAV may come back under its name. */
static int place(int pad, short *pcm, int n, const char *name) {
    char path[320];
    sound_swap(pad, pcm, n, name, 1);
    ui_lock();
    sounds[pad].p[P_START] = param_def[P_START];
    sounds[pad].p[P_END] = param_def[P_END];
    ui_unlock();
    pad_path(path, sizeof path, project_path(), pad);
    if (wav_write(path, pcm, n, 1)) return 1;
    unlink(path);
    sounds[pad].smp.loaded = 0;
    return 0;
}

/* the pad's label: the file's name, its folders and extension off, what the font can draw */
static void label_of(const char *file, char *out, size_t n) {
    const char *base = strrchr(file, '/'), *dot;
    base = base ? base + 1 : file;
    dot = strrchr(base, '.');
    size_t len = dot > base ? (size_t)(dot - base) : strlen(base), k = 0;
    for (size_t i = 0; i < len && k + 1 < n; ++i)
        if (base[i] >= 32 && base[i] < 127) out[k++] = base[i];
    out[k] = 0;
}

int load_pad(int pad, const char *file, int from) {
    short *pcm;
    char label[32];
    int n = decode(file, from, SOUND_SECONDS, &pcm);
    if (!n) return 0;
    label_of(file, label, sizeof label);
    return place(pad, pcm, n, label[0] ? label : "Sample") ? 1 : -1;
}

/* ponytail: the whole file is in memory while it is cut up, 28 MB at most, next to whatever the pads
 * still hold; the pads swap one at a time so the two never add up to more than that. */
typedef struct { float rise; int hop; } hit_t;
static int by_rise(const void *a, const void *b) { float d = ((const hit_t *)b)->rise - ((const hit_t *)a)->rise; return d > 0 ? 1 : d < 0 ? -1 : 0; }
static int by_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
/* where the sound jumps in level: the sixteen biggest jumps in time order, far enough apart, always
 * starting from the top. A long stretch with no jump gets halved so all sixteen pads have something. */
static int slice_points(const short *pcm, int n, int *at) {
    int hops = n / HOP, count = 1, nh = 0;
    float *e = malloc((size_t)(hops + 1) * sizeof *e);
    hit_t *h = malloc((size_t)(hops + 1) * sizeof *h);
    at[0] = 0;
    if (e && h && hops > 3) {
        float sum = 0, sq = 0;
        for (int k = 0; k < hops; ++k) {
            long long a = 0;
            for (int i = 0; i < HOP; ++i) a += abs(pcm[k * HOP + i]);
            e[k] = logf(1 + (float)a / HOP);
        }
        for (int k = 1; k < hops; ++k) { float d = e[k] - e[k - 1]; if (d > 0) sum += d, sq += d * d; }
        float mean = sum / (float)hops, dev = sqrtf(sq / (float)hops - mean * mean);
        for (int k = 1; k + 1 < hops; ++k) {
            float d = e[k] - e[k - 1];
            if (d > mean + dev && d >= e[k + 1] - e[k]) h[nh].rise = d, h[nh++].hop = k;
        }
        qsort(h, (size_t)nh, sizeof *h, by_rise);
        for (int i = 0; i < nh && count < NPAD; ++i) { /* strongest first, none too near one kept */
            int near = h[i].hop < MIN_GAP, frame = h[i].hop * HOP;
            for (int k = 1; k < count; ++k) if (abs(at[k] - frame) < MIN_GAP * HOP) near = 1;
            if (!near) at[count++] = frame;
        }
        qsort(at, (size_t)count, sizeof *at, by_int);
        for (int k = 1; k < count; ++k) { /* back to where the hit starts, inside its window */
            int peak = 0, end = at[k] + HOP < n ? at[k] + HOP : n, from = at[k] - HOP;
            for (int i = from; i < end; ++i) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
            for (int i = from; i < end; ++i) if (abs(pcm[i]) * 4 > peak) { at[k] = i > 64 ? i - 64 : i; break; }
        }
    }
    free(e), free(h);
    while (count < NPAD) {
        int best = 0, len = 0;
        for (int k = 0; k < count; ++k) {
            int l = (k + 1 < count ? at[k + 1] : n) - at[k];
            if (l > len) len = l, best = k;
        }
        if (len < 2 * MIN_SLICE) break;
        memmove(at + best + 2, at + best + 1, (size_t)(count - best - 1) * sizeof *at);
        at[best + 1] = at[best] + len / 2;
        ++count;
    }
    return count;
}
int load_chop(const char *file, int hits) {
    short *pcm;
    int n = decode(file, 0, NPAD * SOUND_SECONDS, &pcm), at[NPAD + 1], count = NPAD, saved = 1;
    if (n / NPAD < 2) return free(pcm), 0;
    if (hits) count = slice_points(pcm, n, at), at[count] = n;
    else for (int i = 0; i <= NPAD; ++i) at[i] = i * (n / NPAD);
    for (int i = 0; i < count; ++i) {
        int len = at[i + 1] - at[i];
        short *s = malloc((size_t)len * sizeof *s);
        char label[32];
        if (!s) return free(pcm), -1;
        memcpy(s, pcm + at[i], (size_t)len * sizeof *s);
        if (hits) for (int k = 1; k < 64 && k < len; ++k) s[len - k] = (short)(s[len - k] * k / 64); /* no click where the next hit was */
        snprintf(label, sizeof label, "Slice %d", i + 1);
        saved &= place(i, s, len, label);
    }
    for (int i = count; i < NPAD; ++i) {
        char path[320];
        sound_swap(i, 0, 0, "", 0);
        pad_path(path, sizeof path, project_path(), i);
        unlink(path);
    }
    free(pcm);
    return saved ? 1 : -1;
}
int load_break(int pad, const char *file) {
    int r = load_pad(pad, file, 0);
    return r && source_break(pad, 0) ? r : 0;
}
int load_break_builtin(int pad, int which) {
    short *pcm;
    int n = x0x_break_render(which, &pcm);
    if (!n) return 0;
    int saved = place(pad, pcm, n, which ? "Break Fill" : "Break Groove");
    return source_break(pad, 2) ? (saved ? 1 : -1) : 0;
}

int export_pattern(char *name, size_t len) {
    char path[400];
    int n = 1;
    snprintf(path, sizeof path, "%s/Music", plat_media());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Music/Groovebox", plat_media());
    mkdir(path, 0755);
    do {
        snprintf(name, len, "1-%d.wav", n);
        snprintf(path, sizeof path, "%s/Music/Groovebox/%s", plat_media(), name);
    } while (!access(path, F_OK) && ++n < 10000);

    ui_lock(); /* the engine is this thread's from here: render() outputs silence behind the flag */
    int cur = seq.cur, cue = seq.cue, playing = seq.playing, at = seq.song_at;
    int frames = seq_song_frames();
    engine_offline(1);
    if (seq.nsong > 1) seq.cur = seq.song[0];
    seq.song_at = 0, seq.cue = -1, seq.pos = 0, seq.bar = seq.pbar = 0, seq.playing = 1;
    ui_unlock();
    FILE *f = wav_begin(path, frames, 2); /* straight to the card: a song can be longer than the memory */
    int ok = f != 0;
    for (int done = 0; ok && done < frames;) {
        static short buf[CTL * 2 * 256];
        int acc[CTL * 2], n = 0;
        for (; n < CTL * 256 && done + n < frames; n += CTL) {
            seq_block();
            engine_block(acc);
            for (int i = 0; i < CTL * 2; ++i) buf[2 * n + i] = (short)acc[i];
        }
        int w = done + n > frames ? frames - done : n;
        ok = fwrite(buf, 4, (size_t)w, f) == (size_t)w;
        done += w;
    }
    ui_lock();
    seq.cur = cur, seq.cue = cue, seq.song_at = at, seq.pos = 0, seq.bar = seq.pbar = 0, seq.playing = playing;
    engine_offline(0);
    ui_unlock();
    return f ? wav_end(f, path, ok) : 0;
}

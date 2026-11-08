/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The slow jobs, all on the UI thread with no lock held: a sound from a card file through the
 * rootfs's ffmpeg (no shell: card names hold quotes and $), one region to a pad or sixteen slices
 * across the kit, and the pattern or chain rendered to a WAV. ponytail: a job stops the screen until
 * it ends and the sound carries on, save an export, which has the engine to itself and silences it;
 * a worker thread if that grates. */
#include <fcntl.h>
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
#include "ui.h"
#include "wav.h"

#define FFMPEG "/usr/bin/ffmpeg"
#define STALL_MS 30000 /* no output for this long and ffmpeg is given up on */

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
int load_chop(const char *file) {
    short *pcm;
    int n = decode(file, 0, NPAD * SOUND_SECONDS, &pcm), slice = n / NPAD, saved = 1;
    if (slice < 2) return free(pcm), 0;
    for (int i = 0; i < NPAD; ++i) {
        short *s = malloc((size_t)slice * sizeof *s);
        char label[32];
        if (!s) return free(pcm), -1;
        memcpy(s, pcm + i * slice, (size_t)slice * sizeof *s);
        snprintf(label, sizeof label, "Slice %d", i + 1);
        saved &= place(i, s, slice, label);
    }
    free(pcm);
    return saved ? 1 : -1;
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
    int cur = seq.cur, cue = seq.cue, playing = seq.playing, chained = seq.chain[0] >= 0 && seq.chain[1] >= 0;
    int frames = seq_step_frames() * NSTEP * (chained ? 2 : 1);
    engine_offline(1);
    if (chained) seq.cur = seq.chain[0];
    seq.cue = -1, seq.pos = 0, seq.bar = 0, seq.playing = 1;
    ui_unlock();
    int ok = 0;
    short *pcm = malloc((size_t)frames * 4);
    if (pcm) {
        int acc[CTL * 2];
        for (int f = 0; f < frames; f += CTL) {
            seq_block();
            engine_block(acc);
            for (int i = 0; i < CTL && f + i < frames; ++i) {
                pcm[2 * (f + i)] = (short)acc[2 * i];
                pcm[2 * (f + i) + 1] = (short)acc[2 * i + 1];
            }
        }
    }
    ui_lock();
    seq.cur = cur, seq.cue = cue, seq.pos = 0, seq.bar = 0, seq.playing = playing;
    engine_offline(0);
    ui_unlock();
    if (pcm) ok = wav_write(path, pcm, frames, 2);
    free(pcm);
    return ok;
}

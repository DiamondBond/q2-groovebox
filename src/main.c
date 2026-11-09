/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * Groovebox for the Shanling Q2: the loop. The audio thread renders in CTL-frame blocks under the
 * UI's lock; the UI thread waits on input, draws at 60 fps while touched or playing and 30 idle,
 * autosaves two seconds after a change and saves on the way out. Slow work (a decode, a kit, an
 * export) runs in ui_tick on this thread, off the lock, so the sound carries on; only an export
 * silences it, having the engine to itself. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"
#include "platform.h"
#include "project.h"
#include "seq.h"
#include "ui.h"

#define FREEZE_MS 8000u /* longer than a card's worst fsync */
#define FREEZE_GRACE_MS 2000 /* the first record found the UI thread running again 200 ms later */
static pix canvas[W * H];
static char project_dir[300];
static const char *volatile stage = "start"; /* what the UI thread is in, for the watchdog */

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void render(short *out, int frames) {
    int acc[CTL * 2];
    long long t0 = now_ns();
    ui_lock();
    if (engine_is_offline()) { /* an export owns the engine */
        ui_unlock();
        memset(out, 0, (size_t)frames * 4);
        return;
    }
    for (int f = 0; f < frames; f += CTL) {
        seq_block();
        engine_block(acc);
        for (int i = 0; i < CTL * 2; ++i) out[(f * 2) + i] = (short)acc[i];
    }
    int load = (int)((now_ns() - t0) * 100 * FS / frames / 1000000000LL);
    engine_load(load);
    ui_unlock();
    if (load >= 100) usleep(1000); /* late, so ALSA won't put it to sleep: it would retake the lock before the UI got it, and hog its core */
}

/* ponytail: one freeze on a device, never reproduced off it. This ends any that leaves the UI thread stuck and says
 * what each thread was doing, and only records a stall the thread comes back from; a spinning thread's pc would need
 * a signal handler, if freeze.txt ever shows one */
static void *watchdog(void *arg) {
    (void)arg;
    struct sched_param sp = { .sched_priority = 60 }; /* above the audio thread, which may be what is spinning */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    static char threads[8192];
    for (;;) {
        usleep(1000000);
        unsigned beat = ui_beat;
        if (!beat || (unsigned)plat_ms() - beat <= FREEZE_MS) continue;
        const char *in = stage;
        char head[64];
        project_freeze_take(threads, sizeof threads); /* now, while it is stuck */
        int stuck = project_freeze_stuck(beat, FREEZE_GRACE_MS);
        unsigned back = ui_beat; /* 0 if it went on into a long job */
        snprintf(head, sizeof head, "stalled_ms=%u in=%s recovered=%d\n", (!stuck && back ? back : (unsigned)plat_ms()) - beat, in, !stuck);
        alarm(stuck ? 3 : 20); /* a card that has gone must not keep the app from quitting */
        project_freeze_note(plat_dir(), head, threads);
        if (stuck) _exit(1);
        alarm(0);
    }
}

int main(int argc, char **argv) {
    if (!plat_open(argc, argv)) return fprintf(stderr, "groovebox: no screen\n"), 1;
    engine_init();
    seq_init();
    ui_init();
    snprintf(project_dir, sizeof project_dir, "%s/projects", plat_dir());
    mkdir(project_dir, 0755);
    snprintf(project_dir, sizeof project_dir, "%s", project_path());
    mkdir(project_dir, 0755);
    project_load(project_dir);
    seq_starter();
    kit_open(kit_name);
    pads_restore(project_dir);
    if (!plat_audio_start(render)) fprintf(stderr, "groovebox: no audio\n");
    long long next = plat_ms(), dirty_since = 0;
    pthread_t dog;
    pthread_create(&dog, 0, watchdog, 0);
    while (!ui_quit()) {
        long long t = plat_ms();
        event_t e;
        ui_beat = (unsigned)t | 1;
        stage = "input";
        int wait = next > t ? (int)(next - t) : 0;
        while (plat_wait(wait, &e) && e.kind != 'q') ui_event(&e, plat_ms()), wait = 0;
        if (e.kind == 'q') break;
        t = plat_ms();
        stage = "tick";
        ui_tick(t);
        if (t < next) continue;
        stage = "draw";
        ui_render(canvas, t);
        stage = "show";
        plat_show(canvas);
        next = t + (ui_lively(t) ? 16 : 33);
        if (ui_dirty()) {
            if (!dirty_since) dirty_since = t;
            if (t - dirty_since > 2000) {
                stage = "save";
                if (project_save(project_dir)) ui_saved(), dirty_since = 0;
            }
        } else dirty_since = 0;
    }
    ui_beat = 0;
    plat_audio_stop();
    if (ui_dirty()) project_save(project_dir);
    plat_close();
    return 0;
}

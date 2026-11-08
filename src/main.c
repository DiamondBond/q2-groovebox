/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * Groovebox for the Shanling Q2: the loop. The audio thread renders in CTL-frame blocks under the
 * UI's lock; the UI thread waits on input, draws at 60 fps while touched or playing and 30 idle,
 * autosaves two seconds after a change and saves on the way out. Slow work (a decode, a kit, an
 * export) runs in ui_tick on this thread, off the lock, so the sound carries on; only an export
 * silences it, having the engine to itself. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "engine.h"
#include "platform.h"
#include "project.h"
#include "seq.h"
#include "ui.h"

static pix canvas[W * H];
static char project_dir[300];

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
    engine_load((int)((now_ns() - t0) * 100 * FS / frames / 1000000000LL));
    ui_unlock();
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
    while (!ui_quit()) {
        long long t = plat_ms();
        event_t e;
        int wait = next > t ? (int)(next - t) : 0;
        while (plat_wait(wait, &e) && e.kind != 'q') ui_event(&e, plat_ms()), wait = 0;
        if (e.kind == 'q') break;
        t = plat_ms();
        ui_tick(t);
        if (t < next) continue;
        ui_render(canvas, t);
        plat_show(canvas);
        next = t + (ui_lively(t) ? 16 : 33);
        if (ui_dirty()) {
            if (!dirty_since) dirty_since = t;
            if (t - dirty_since > 2000 && project_save(project_dir)) ui_saved(), dirty_since = 0;
        } else dirty_since = 0;
    }
    plat_audio_stop();
    if (ui_dirty()) project_save(project_dir);
    plat_close();
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * One ALSA thread at FS, S16LE stereo, period 256 frames, buffer 1024 (four periods, Rockbox's
 * rule). alsa-lib 1.1.6 deadlocks unless LIBASOUND_THREAD_SAFE=0, as q2video found. The MIPS FPU
 * traps on denormals, so the thread flushes them to zero. */
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include "groovebox.h"
#include "platform.h"

#define PERIOD 256
static snd_pcm_t *pcm;
static pthread_t thread;
static volatile int run, underruns;
static void (*render_cb)(short *, int);

int plat_underruns(void) { return underruns; }

static void *audio_thread(void *arg) {
    (void)arg;
#if defined(__mips__)
    unsigned fcsr; /* FS bit 24: flush denormals */
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
    fcsr |= 1u << 24;
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcsr));
#endif
    struct sched_param sp = { .sched_priority = 50 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    short buf[PERIOD * 2];
    while (run) {
        render_cb(buf, PERIOD);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, PERIOD);
        if (n < 0) {
            ++underruns;
            if (snd_pcm_recover(pcm, (int)n, 1) < 0) break;
        }
    }
    return 0;
}

int plat_audio_start(void (*render)(short *, int)) {
    extern const char *audio_device; /* platform_q2.c's argv[1] */
    setenv("LIBASOUND_THREAD_SAFE", "0", 1);
    if (snd_pcm_open(&pcm, audio_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) return 0;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, FS, 1,
                           4 * PERIOD * 1000000 / FS) < 0)
        return snd_pcm_close(pcm), pcm = 0, 0;
    render_cb = render;
    run = 1;
    if (pthread_create(&thread, 0, audio_thread, 0)) return run = 0, snd_pcm_close(pcm), pcm = 0, 0;
    return 1;
}
void plat_audio_stop(void) {
    if (!pcm) return;
    run = 0;
    pthread_join(thread, 0);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = 0;
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * One ALSA thread at FS, S16LE stereo, period 256 frames, buffer 1024 (four periods, Rockbox's
 * rule). alsa-lib 1.1.6 deadlocks unless LIBASOUND_THREAD_SAFE=0, as q2video found. The MIPS FPU
 * traps on denormals, so the thread flushes them to zero.
 * The firmware hands the output over as it does to its q2video (q2-pod patch/video.c sound_open,
 * docs/librespot.md), so this does what q2video does with it: "usb" is the USB DAC's card in
 * /proc/asound/cards; hciplayer lets go of the PCM, muting the DAC, a moment after the firmware
 * stopped it, so the open is retried (2 s on the jack, 5 s on bluealsa, which claims its PCM at
 * set_params) and the jack's DAC is unmuted after it, and muted again at the end; Mono Audio and
 * Swap L/R are the firmware's /tmp/q2mono and /tmp/q2swap, an int each. */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "groovebox.h"
#include "platform.h"

#define PERIOD 256
#define DAC "/dev/shanling_dac"
#define DAC_PCM 0xc0044d1bu  /* hciplayer sets it to 0 for each PCM track: undoes a DSD one */
#define DAC_MUTE 0xc0044d1fu /* hciplayer sets it on pause and close, clears it on start */
#define MONO_FILE "/tmp/q2mono"
#define SWAP_FILE "/tmp/q2swap"
static snd_pcm_t *pcm;
static int jack; /* the headphone DAC, plughw:0,0: its mute is this program's while it runs */
static pthread_t thread;
static volatile int run, underruns;
static void (*render_cb)(short *, int);

int plat_underruns(void) { return underruns; }

/* "usb", the USB DAC, as plughw:N,0: the card /proc/asound/cards lists as "N [id]: USB-Audio - name". */
static const char *usb_device(char *out) {
    char b[1024], *u;
    int fd = open("/proc/asound/cards", O_RDONLY | O_CLOEXEC), n = fd >= 0 ? (int)read(fd, b, sizeof b - 1) : 0;
    if (fd >= 0) close(fd);
    b[n > 0 ? n : 0] = 0;
    if (!(u = strstr(b, ": USB-Audio - "))) return "plughw:1,0";
    while (u > b && u[-1] != '\n') --u;
    snprintf(out, 16, "plughw:%d,0", atoi(u));
    return out;
}
static void dac_set(unsigned long req, int v) {
    int fd = open(DAC, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, req, &v);
    close(fd);
}
/* the firmware's int flag file, shared read-only, 0 until it exists */
static volatile int *flag_map(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    void *p = mmap(0, sizeof(int), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? 0 : p;
}

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
    volatile int *mono = 0, *swap = 0; /* the firmware's switches, looked for till they appear */
    for (unsigned tries = 0; run; ++tries) {
        if (!(tries % 100)) {
            if (!mono) mono = flag_map(MONO_FILE);
            if (!swap) swap = flag_map(SWAP_FILE);
        }
        render_cb(buf, PERIOD);
        if (mono && *mono > 0)
            for (int i = 0; i < PERIOD * 2; i += 2) buf[i] = buf[i + 1] = (short)((buf[i] + buf[i + 1]) / 2);
        if (swap && *swap > 0)
            for (int i = 0; i < PERIOD * 2; i += 2) {
                short t = buf[i];
                buf[i] = buf[i + 1], buf[i + 1] = t;
            }
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
    char usb[16];
    const char *dev = strcmp(audio_device, "usb") ? audio_device : usb_device(usb);
    int err;
    jack = !strcmp(dev, "plughw:0,0");
    setenv("LIBASOUND_THREAD_SAFE", "0", 1);
    for (int i = 0; i < (jack ? 20 : 50) && (err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)); ++i)
        pcm = 0, usleep(100000);
    if (err) return 0;
    for (int i = 0; (err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, FS, 1,
                                              4 * PERIOD * 1000000 / FS)) == -EBUSY && !jack && i < 50; ++i)
        usleep(100000);
    if (err) return snd_pcm_close(pcm), pcm = 0, 0;
    if (jack) dac_set(DAC_PCM, 0), dac_set(DAC_MUTE, 0); /* hciplayer's close muted it */
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
    if (jack) dac_set(DAC_MUTE, 1); /* as q2video's quit: no noise till the firmware powers it off */
}

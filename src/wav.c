/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * WAV in and out, the plain PCM kinds. The reader downmixes and resamples (linear) to the engine's
 * mono at FS, since every sound is read once and plays many times. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groovebox.h"
#include "wav.h"

static unsigned le(const unsigned char *p, int n) {
    unsigned v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | p[i];
    return v;
}
#define WAV_MAX (40 * 1024 * 1024) /* ponytail: a sound is seconds long; this stops a 2 GB file */
int wav_read(const char *path, short **pcm) {
    *pcm = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > WAV_MAX) return fclose(f), 0;
    unsigned char *b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) return free(b), fclose(f), 0;
    fclose(f);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return free(b), 0;
    unsigned fmt = 0, ch = 0, rate = 0, bits = 0, dpos = 0, dlen = 0;
    for (unsigned p = 12; p + 8 <= (unsigned)size;) {
        unsigned n = le(b + p + 4, 4);
        if (n > (unsigned)size - p - 8) n = (unsigned)size - p - 8;
        if (!memcmp(b + p, "fmt ", 4) && n >= 16)
            fmt = le(b + p + 8, 2), ch = le(b + p + 10, 2), rate = le(b + p + 12, 4), bits = le(b + p + 22, 2);
        else if (!memcmp(b + p, "data", 4))
            dpos = p + 8, dlen = n;
        p += 8 + n + (n & 1);
    }
    if ((fmt != 1 && fmt != 0xfffe) || !ch || !rate || (bits != 8 && bits != 16 && bits != 24) || !dlen)
        return free(b), 0;
    unsigned bps = bits / 8, frames = dlen / (bps * ch);
    unsigned out_n = (unsigned)((unsigned long long)frames * FS / rate);
    if (out_n < 2) return free(b), 0;
    short *out = malloc(out_n * sizeof *out);
    if (!out) return free(b), 0;
    for (unsigned i = 0; i < out_n; ++i) {
        unsigned long long src = (unsigned long long)i * rate / FS; /* nearest-lower frame, then blend */
        unsigned f0 = (unsigned)src, f1 = f0 + 1 < frames ? f0 + 1 : f0;
        unsigned frac = (unsigned)(((unsigned long long)i * rate % FS) * 65536 / FS);
        int s[2] = { 0, 0 };
        for (int k = 0; k < 2; ++k) {
            unsigned fr = k ? f1 : f0;
            long sum = 0;
            for (unsigned c = 0; c < ch; ++c) {
                const unsigned char *q = b + dpos + (fr * ch + c) * bps;
                int v = bps == 1 ? ((int)q[0] - 128) << 8 : bps == 2 ? (short)le(q, 2) : (int)(le(q, 3) << 8) >> 16;
                sum += v;
            }
            s[k] = (int)(sum / (long)ch);
        }
        out[i] = (short)(s[0] + (((s[1] - s[0]) * (int)frac) >> 16));
    }
    free(b);
    *pcm = out;
    return (int)out_n;
}

int wav_write(const char *path, const short *stereo, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    unsigned dlen = (unsigned)frames * 4, hdr[11] = { 0x46464952, 36 + dlen, 0x45564157, 0x20746d66, 16,
                                                         1 | 2 << 16, FS, FS * 4, 4 | 16 << 16, 0x61746164, dlen };
    int ok = fwrite(hdr, 1, 44, f) == 44 && fwrite(stereo, 4, (size_t)frames, f) == (size_t)frames;
    return fclose(f) == 0 && ok;
}

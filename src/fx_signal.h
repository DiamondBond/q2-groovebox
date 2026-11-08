/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The input the lifted chain is compared on, here and against SLOOP itself (tools/sloop_golden.c):
 * a falling triangle a beat and noise bursts a sixteenth, into the mix and both sends. FNV-1a. */
#define GOLDEN_BLOCKS 3000u /* 2.2 s */
#define GOLDEN_ON 300u      /* the effect runs from this block */
#define GOLDEN_OFF 2600u    /* to this one, then its release */
static void golden_signal(unsigned b, int *l, int *r, int *dly, int *rev) {
    static unsigned seed;
    if (!b) seed = 1;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned t = b * 32 + i, q = t % 22050, s = t % 5512, p = t % 400;
        int tri = (p < 200 ? (int)p - 100 : 300 - (int)p) * 160 * (int)(22050 - q) / 22050;
        int nz = s < 2000 ? (int)((seed = seed * 1664525u + 1013904223u) >> 18) - 8192 : 0;
        l[i] = (tri + nz) * 3, r[i] = (tri - nz) * 3;
        dly[i] = tri + nz, rev[i] = tri - nz;
    }
}
static unsigned golden_hash(unsigned h, const int *a) {
    for (int i = 0; i < 32; ++i)
        for (int k = 0; k < 4; ++k) h = (h ^ ((unsigned)a[i] >> (k * 8) & 255)) * 16777619u;
    return h;
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The lift: what punch.c and the buses read of SLOOP's core.h, then SLOOP's own code. Its own
 * translation unit, as SLOOP's names are short and many. Its level is ours, Q15 in int32 with
 * headroom; the mix is held inside what its filters take, far above anything the limiter lets out. */
#include <stdint.h>
#include <string.h>
#include "groovebox.h"
#include "fx.h"
#include "sloop_tables.h"
#undef W /* punch.c has a table of that name */
#undef H
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized" /* punch.c's c1, set for the effects that use it */
#endif

enum { G_BPM, G_FXFADE, G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX, G_RSIZE, G_RDAMP, G_COUNT };
static struct {
    int16_t g[G_COUNT];
    uint8_t playing;
} song;
static uint32_t clk_pos;
/* SLOOP's TIME default is an eighth */
static uint32_t dly_samples(uint32_t d) { return (void)d, (uint32_t)FS * 30u / (uint32_t)song.g[G_BPM]; }

#include "sloop_dsp.h"
#include "sloop_buses.c"
#include "punch.c"

#define MIX_MAX 280000 /* punch.c's clamp on its kills: x >> 1 stays inside the SVFs' 150000 */
#define REV_MAX 165000 /* fx_buses' mulq15(rev_in, 13000) fits 32 bits */

const signed char fx_page[FX_PAGES][16] = {
    { PX_LOOP4, PX_LOOP8, PX_LOOP16, PX_LOOP32, PX_STUT, PX_REV, PX_STOP, PX_HALF, PX_LPF, PX_HPF, PX_CRUSH, PX_GATE,
      PX_ECHO, PX_KLOW, PX_KMID, PX_KHIGH },
    { PX_TEL, PX_DOWN, PX_WOBBLE, PX_RWD, PX_OCT, PX_FLANGE, PX_PAN, PX_FREEZE, PX_LOOP8T, PX_MONO, PX_FADE, -1, -1, -1,
      -1, -1 },
};
static int live = -1, recorded = -1;
const char *fx_name(int fx) { return PUNCH_NAME[fx]; }

void fx_init(void) {
    memset(dly_buf, 0, sizeof dly_buf);
    memset(rev_line, 0, sizeof rev_line);
    memset(rev_ap, 0, sizeof rev_ap);
    memset(&fx, 0, sizeof fx);
    memset(rev_l, 0, sizeof rev_l);
    memset(rev_r, 0, sizeof rev_r);
    memset(punch_ring, 0, sizeof punch_ring);
    memset((void *)&punch, 0, sizeof punch);
    punch.req = punch.cur = -1;
    punch.fg = 8192;
    rev_frz = 0;
    live = recorded = -1;
    song.g[G_BPM] = 120, song.g[G_FXFADE] = 0, song.g[G_DTIME] = 1, song.g[G_DFDBK] = 60; /* SLOOP's defaults */
    song.g[G_DCOLOR] = 70, song.g[G_DMIX] = 90, song.g[G_RSIZE] = 90, song.g[G_RDAMP] = 60;
}
void fx_clock(int playing, int bpm, int pos) {
    song.playing = (uint8_t)playing;
    song.g[G_BPM] = (int16_t)bpm;
    clk_pos = (uint32_t)pos * (uint32_t)bpm;
}
static void want(void) { punch.req = (int8_t)(live >= 0 ? live : recorded); }
void fx_live(int f) { live = f, want(); }
void fx_seq(int f) { recorded = f, want(); }
int fx_sounding(void) { return punch.req; }

void fx_buses_block(int *l, int *r, int *dly, int *rev) {
    int wet_l[CTL], wet_r[CTL];
    for (int i = 0; i < CTL; ++i) rev[i] = clamp(rev[i], -REV_MAX, REV_MAX);
    punch_freeze_feed(rev, l, r, CTL);
    fx_buses(dly, rev, wet_l, wet_r, CTL);
    for (int i = 0; i < CTL; ++i) l[i] += wet_l[i], r[i] += wet_r[i];
}
void fx_punch_block(int *l, int *r) {
    for (int i = 0; i < CTL; ++i) l[i] = clamp(l[i], -MIX_MAX, MIX_MAX), r[i] = clamp(r[i], -MIX_MAX, MIX_MAX);
    punch_process(l, r, CTL);
}

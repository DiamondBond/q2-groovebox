/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * SLOOP's punch-in effects and its delay and reverb buses, on blocks of CTL frames. */
#ifndef FX_H
#define FX_H
#define NFX 27
#define FX_PAGES 2
extern const signed char fx_page[FX_PAGES][16]; /* a page's effect on each pad, -1 none */
const char *fx_name(int fx);
void fx_init(void);
void fx_clock(int playing, int bpm, int pos); /* the transport, at the block's start: pos frames into the beat */
void fx_live(int fx);                         /* the finger's effect, -1 none; it beats a recorded one */
void fx_seq(int fx);                          /* the pattern's recorded effect now, -1 none */
int fx_sounding(void);                        /* the effect running now, -1 none */
void fx_buses_block(int *l, int *r, int *dly, int *rev); /* the sends through the buses, the wet into l and r */
void fx_feed_block(int *l, int *r, int *rev);             /* the sends bound for another bus: bounded the same */
void fx_punch_block(int *l, int *r);
#endif

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * X0X's machines, one to a pad, its generator and its bus. */
#ifndef X0X_H
#define X0X_H
#include "engine.h"
void x0x_init(void);
void x0x_defaults(int src, int sub, int *e);
void x0x_kit(int src, int pad, int *sub, int *e, const char **name, int *choke); /* a live kit's pad */
void x0x_assign(int pad, int src, int sub, const int *e); /* under ui_lock */
void x0x_set(int pad, int src, int sub, int tile, int v);
int x0x_tile(int src, int sub, int i, tile_t *t);
void x0x_on(int pad, int src, int sub, int note, int semis, int accent, int slide);
void x0x_off(int pad, int src);
void x0x_kill(int pad, int src);
int x0x_fill(int pad, int src, float *s); /* CTL frames, Q15; 0 once silent */
/* the break: a pad's sample as the loop, built off the lock */
int x0x_break_load(int pad, const short *pcm, int n, int bars, const int *e);
void x0x_break_free(int pad);
void x0x_break_step(int pad, int step, unsigned bar, int frames, int on);
void x0x_break_stop(int pad);
void x0x_break_live(int pad, int key, int down); /* 0-7 a slice, 8 reverse, 9 half, 10 stutter */
int x0x_break_guess(int frames);                 /* bars, from a loop's length */
int x0x_break_render(int which, short **out);    /* the two built-in loops, played by the 909: frames */
/* TB-3PO */
typedef struct { signed char on, accent, slide; unsigned char note; } x0x_step_t;
typedef struct { int density, root, scale, base_oct; unsigned seed; } x0x_gen_t;
void x0x_line(x0x_step_t *s, int len, const x0x_gen_t *g);
void x0x_mutate(x0x_step_t *s, int len, const x0x_gen_t *g, unsigned *rng);
unsigned x0x_seed(unsigned entropy);
/* the bus: FX's third page */
#define BUS_TILES 16
void x0x_bus_reset(void);
int x0x_bus_tile(int i, tile_t *t);
int x0x_bus_get(int i);
void x0x_bus_set(int i, int v);
int x0x_bus_sends(void);  /* the sends go to X0X's reverb and delay rather than SLOOP's */
void x0x_bus_block(int *l, int *r, int *dly, int *rev, int bpm);
#endif

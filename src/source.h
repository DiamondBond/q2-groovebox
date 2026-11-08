/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * What a pad plays when it isn't a plain sample: assigning one, the machine's boxes, the generator. */
#ifndef SOURCE_H
#define SOURCE_H
#include <stddef.h>
#include "engine.h"
/* with no lock held */
void source_assign(int pad, int src, int sub, int preset, const int *e, const char *name); /* e 0: the defaults */
void source_synth(int pad, int engine, int preset);
int source_break(int pad, int bars);  /* the pad's sample as a break; bars 0: guessed from its length. 1 if it took */
void source_kit(int src);             /* SRC_909 or SRC_808 live across the pads */
void source_restore(int pad, int src, int sub, int preset, const int *e); /* from a project file: every value checked */
/* under ui_lock */
void source_restart_all(void);        /* every machine back to its first state, settings kept: an export starts here */
int source_tile(int pad, int i, tile_t *t);
int source_get(int pad, int i);
void source_turn(int pad, int i, int n);
void source_text(int pad, int i, char *out, size_t n);
void source_line(int pad, int mutate, unsigned entropy); /* TB-3PO writes this pad's track of the current pattern */
#endif

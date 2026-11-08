/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * Groovebox for the Shanling Q2: what every file shares. */
#ifndef GROOVEBOX_H
#define GROOVEBOX_H
#define W 375 /* the view, landscape */
#define H 320
#define FS 44100 /* the engine's rate, the wired DAC's own */
#define CTL 32   /* frames a block, SLOOP's control rate: triggers land on a block */
#define NPAD 16
#define NSTEP 64 /* a track's most steps: four pages */
#define BAR 16   /* sixteenths a bar, and steps a page */
#define NPATT 16
#define SOUND_SECONDS 20 /* the longest sound a pad holds */
#define UNSET (-128) /* a step parameter that isn't locked: the sound's own */
typedef unsigned pix;  /* 0xffRRGGBB, the panel's word */
#endif

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * What the device and the host window both provide. */
#ifndef PLATFORM_H
#define PLATFORM_H
#include "groovebox.h"
typedef struct {
    char kind, what; /* 'k' d|u, 'w', 't' d|m|u, or 0 for none; 'q' = quit asked (TERM) */
    int a, b;        /* key or wheel delta or x, y */
} event_t;
int plat_open(int argc, char **argv);       /* screen + input; 0 on failure */
int plat_wait(int ms, event_t *e);          /* next event, or 0 after ms */
void plat_show(const pix *canvas);          /* W x H to the screen */
void plat_close(void);
long long plat_ms(void);                    /* monotonic */
const char *plat_dir(void);                 /* the binary's folder, for kits and projects */
int plat_audio_start(void (*render)(short *stereo, int frames)); /* argv-chosen output; 0 on failure */
void plat_audio_stop(void);
int plat_underruns(void);
int plat_start_gain(void);                  /* unity, or hciplayer's curve for argv[2] on BT/USB */
#endif

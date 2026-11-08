/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef PROJECT_H
#define PROJECT_H
#include <stddef.h>
extern char kit_name[32];                    /* the project's kit */
void sound_swap(int pad, short *pcm, int n, const char *name, int loaded); /* takes pcm; safe while playing */
int kit_load(const char *dir);               /* sixteen WAVs by name into the pads: how many */
#define KIT_909_LIVE "909 Live"
#define KIT_808_LIVE "808 Live"
int kit_choose(const char *name);            /* this kit, 1 if it opened; else the sounds stay as far as they can */
void kit_open(const char *name);             /* at start: by name, or default if it is missing */
const char *project_path(void);
void pad_path(char *out, size_t n, const char *dir, int pad); /* a loaded sound's WAV */
void pads_restore(const char *dir);          /* the loaded sounds back, after the kit */
void pads_forget(const char *dir);           /* a kit change: their WAVs go */
int project_load(const char *dir);           /* <dir>/project.txt into seq and the sounds: 1 if found */
int project_save(const char *dir);           /* whole, .new then renamed: 1 on success */
#endif

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef WAV_H
#define WAV_H
/* A PCM WAV (8, 16 or 24-bit, any channels, any rate) as mono 16-bit at FS: frames, or 0 and no buffer */
int wav_read(const char *path, short **pcm);
/* 16-bit at FS, ch channels, through .new and an fsync: 0 on failure */
#include <stdio.h>
FILE *wav_begin(const char *path, int frames, int ch); /* then the frames, s16: an export too long to hold */
int wav_end(FILE *f, const char *path, int ok);        /* fsync and rename, or nothing left behind */
int wav_write(const char *path, const short *pcm, int frames, int ch);
#endif

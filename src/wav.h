/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef WAV_H
#define WAV_H
/* A PCM WAV (8, 16 or 24-bit, any channels, any rate) as mono 16-bit at FS: frames, or 0 and no buffer */
int wav_read(const char *path, short **pcm);
/* 16-bit stereo at FS: 0 on failure */
int wav_write(const char *path, const short *stereo, int frames);
#endif

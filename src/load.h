/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef LOAD_H
#define LOAD_H
#include <stddef.h>
/* 1 loaded and saved, -1 loaded but not saved to the card, 0 the file could not be read */
int load_pad(int pad, const char *file, int from); /* up to 20 s from second <from> */
int load_chop(const char *file, int hits);         /* sixteen slices across the pads: at its transients, or equal */
int load_break(int pad, const char *file);         /* the file to the pad as a break */
int load_break_builtin(int pad, int which);        /* X0X's two loops: groove, fill */
int export_pattern(char *name, size_t len);        /* the current pattern or song to Music/Groovebox: 1 on success */
#endif

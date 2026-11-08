/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef PROJECT_H
#define PROJECT_H
int kit_load(const char *dir);               /* sixteen WAVs by name into the pads: how many */
int project_load(const char *dir);           /* <dir>/project.txt into seq and the sounds: 1 if found */
int project_save(const char *dir);           /* whole, .new then renamed: 1 on success */
#endif

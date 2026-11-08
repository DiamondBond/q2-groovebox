/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef DROP_H
#define DROP_H
#include <stddef.h>
int drop_start(int port);                /* 1 if listening */
void drop_stop(void);                    /* waits for its thread: no lock held */
const char *drop_url(void);              /* what the phone opens, "" with no network */
int drop_take(char *path, size_t n);     /* 1, once, when a file has arrived */
#endif

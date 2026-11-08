/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond */
#ifndef UI_H
#define UI_H
#include "groovebox.h"
#include "platform.h"
enum { M_BEAT, M_PADS, M_SOUND, M_SONG, M_N };
void ui_init(void);
void ui_event(const event_t *e, long long now);
void ui_tick(long long now);      /* holds that became long, the autosave, the self-close */
void ui_render(pix *canvas, long long now);
int ui_quit(void);
int ui_lively(long long now);     /* touched lately or playing: 60 fps, else 30 */
int ui_dirty(void);               /* the project changed since the last save (cleared by ui_saved) */
void ui_saved(void);
void ui_lock(void);               /* seq and engine state: the audio thread takes it per period */
void ui_unlock(void);
#endif

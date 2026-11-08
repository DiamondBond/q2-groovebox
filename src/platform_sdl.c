/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The host: an SDL2 window the size of the view, the mouse as the finger, the scroll wheel as the
 * wheel, Enter Centre, Backspace Return, Space Play, [ ] Prev/Next, Escape quits. With
 * --script FILE OUTDIR it replays datagram lines ("MS t d X Y", "MS k d 218", "MS w -13",
 * "MS shot NAME") without a window and writes NAME.ppm frames, so the stage can be eyeballed. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "platform.h"

const char *audio_device = "sdl";
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID dev;
static void (*render_cb)(short *, int);
static char dir[256];
static FILE *script;
static char outdir[256];
static long long fake_now;
static int quit_asked;
static char shot[64];

long long plat_ms(void) { return script ? fake_now : (long long)SDL_GetTicks64(); }
const char *plat_dir(void) { return dir; }
int plat_start_gain(void) { return 65536; }
int plat_underruns(void) { return 0; }

int plat_open(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", argc > 0 ? argv[0] : "");
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    for (int i = 1; i + 2 < argc; ++i)
        if (!strcmp(argv[i], "--script")) {
            script = fopen(argv[i + 1], "r");
            snprintf(outdir, sizeof outdir, "%s", argv[i + 2]);
            if (!script) return 0;
        }
    if (script) return 1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 0;
    win = SDL_CreateWindow("Groovebox", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    return win && ren && tex;
}

static int key_code(SDL_Keycode k) {
    return k == SDLK_RETURN ? 218 : k == SDLK_BACKSPACE ? 170 : k == SDLK_SPACE ? 171 : k == SDLK_LEFTBRACKET ? 222
         : k == SDLK_RIGHTBRACKET ? 223 : 0;
}
static int parse(const char *s, event_t *e) { /* "k d 218" / "w -13" / "t d X Y" */
    char what[4] = "";
    memset(e, 0, sizeof *e);
    if (sscanf(s, "k %1s %d", what, &e->a) == 2) return e->kind = 'k', e->what = what[0], 1;
    if (sscanf(s, "w %d", &e->a) == 1) return e->kind = 'w', 1;
    if (sscanf(s, "t %1s %d %d", what, &e->a, &e->b) == 3) return e->kind = 't', e->what = what[0], 1;
    return 0;
}
int plat_wait(int ms, event_t *e) {
    memset(e, 0, sizeof *e);
    if (script) { /* the next line, if its time has come within ms */
        static char line[128];
        static int have;
        if (!have && !fgets(line, sizeof line, script)) { /* the end, once a pending shot is drawn */
            if (shot[0]) return fake_now += ms, 0;
            return quit_asked = 1, e->kind = 'q', 1;
        }
        have = 1;
        long long t; int n;
        if (sscanf(line, "%lld %n", &t, &n) < 1) return have = 0, 0;
        if (t > fake_now + ms) return fake_now += ms, 0;
        fake_now = t;
        have = 0;
        if (sscanf(line + n, "shot %63s", shot) == 1) return e->kind = 's', 1;
        return parse(line + n, e);
    }
    SDL_Event ev;
    int got = ms > 0 ? SDL_WaitEventTimeout(&ev, ms) : SDL_PollEvent(&ev);
    if (!got) return 0;
    switch (ev.type) {
    case SDL_QUIT: e->kind = 'q'; return 1;
    case SDL_KEYDOWN: case SDL_KEYUP:
        if (ev.key.repeat) return 0;
        if (ev.key.keysym.sym == SDLK_ESCAPE) { e->kind = 'q'; return 1; }
        e->a = key_code(ev.key.keysym.sym);
        if (!e->a) return 0;
        e->kind = 'k', e->what = ev.type == SDL_KEYDOWN ? 'd' : 'u';
        return 1;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION:
        if (ev.type == SDL_MOUSEMOTION && !(ev.motion.state & SDL_BUTTON_LMASK)) return 0;
        e->kind = 't';
        e->what = ev.type == SDL_MOUSEBUTTONDOWN ? 'd' : ev.type == SDL_MOUSEBUTTONUP ? 'u' : 'm';
        e->a = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x;
        e->b = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
        return 1;
    case SDL_MOUSEWHEEL: e->kind = 'w', e->a = -ev.wheel.y * 13; return 1; /* a notch a detent, anticlockwise-positive as the Q2 */
    }
    return 0;
}

void plat_show(const pix *canvas) {
    if (script) {
        if (!shot[0]) return;
        char path[400];
        snprintf(path, sizeof path, "%s/%s.ppm", outdir, shot);
        FILE *f = fopen(path, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; ++i) { unsigned char rgb[3] = { canvas[i] >> 16, canvas[i] >> 8, canvas[i] }; fwrite(rgb, 1, 3, f); }
        fclose(f);
        shot[0] = 0;
        return;
    }
    SDL_UpdateTexture(tex, 0, canvas, W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, 0, 0);
    SDL_RenderPresent(ren);
}

static void sdl_audio(void *u, Uint8 *stream, int len) { (void)u; render_cb((short *)stream, len / 4); }
int plat_audio_start(void (*render)(short *, int)) {
    render_cb = render;
    if (script) return 1;
    SDL_AudioSpec want = { .freq = FS, .format = AUDIO_S16SYS, .channels = 2, .samples = 256, .callback = sdl_audio }, have;
    dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
    if (!dev) return 0;
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}
void plat_audio_stop(void) { if (dev) SDL_CloseAudioDevice(dev), dev = 0; }
void plat_close(void) { if (script) fclose(script); else SDL_Quit(); }

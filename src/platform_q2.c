/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The device: the framebuffer and the firmware's input socket, after Buddy's (q2-fidget, MIT),
 * and the firmware's argv: the ALSA device, then on Bluetooth or USB its volume (NN, or hNN). */
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"
#include "platform.h"

#define INPUT_SOCK "/tmp/q2-groovebox.input.sock"
const char *audio_device = "plughw:0,0";
static int start_gain = 65536;
static int fb = -1, sock = -1;
static struct fb_var_screeninfo var;
static unsigned *mem, line, y0, back, mem_len;
static char dir[256];
static volatile sig_atomic_t quit;

static void on_term(int sig) { (void)sig; quit = 1; }

long long plat_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
const char *plat_dir(void) { return dir; }
int plat_start_gain(void) { return start_gain; }

int plat_open(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) audio_device = argv[1];
    if (argc > 2 && argv[2][0]) start_gain = bt_gain(atoi(argv[2] + (argv[2][0] == 'h')));
    snprintf(dir, sizeof dir, "%s", argc > 0 ? argv[0] : "");
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else strcpy(dir, ".");

    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    struct fb_fix_screeninfo fix;
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) return 0;
    line = fix.line_length / 4, y0 = var.yoffset, back = y0 ? 0 : W;
    /* the Q2's panel only: 320 x 375 portrait, 32 bpp, red at 16, two pages */
    if (var.xres != H || var.yres != W || var.yres_virtual < 2 * W || var.bits_per_pixel != 32 ||
        var.red.offset != 16 || line < H || 2 * W * line * 4 > fix.smem_len)
        return 0;
    mem_len = fix.smem_len;
    mem = mmap(0, mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (mem == MAP_FAILED) return mem = 0, 0;

    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = INPUT_SOCK };
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    unlink(INPUT_SOCK);
    if (sock >= 0 && bind(sock, (struct sockaddr *)&addr, sizeof addr)) close(sock), sock = -1;
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    return 1;
}

static int num(const char **s) {
    int n = 0, neg = 0;
    while (**s == ' ') ++*s;
    if (**s == '-') neg = 1, ++*s;
    while (**s >= '0' && **s <= '9') n = n * 10 + *(*s)++ - '0';
    return neg ? -n : n;
}
int plat_wait(int ms, event_t *e) {
    memset(e, 0, sizeof *e);
    if (quit) return e->kind = 'q', 1;
    struct pollfd p = { sock, POLLIN, 0 };
    if (sock < 0 || poll(&p, 1, ms) <= 0) return quit ? (e->kind = 'q', 1) : 0;
    char msg[64];
    long n = recv(sock, msg, sizeof msg - 1, MSG_DONTWAIT);
    if (n <= 0) return 0;
    msg[n] = 0;
    e->kind = msg[0];
    e->what = msg[0] && msg[1] == ' ' ? msg[2] : 0;
    const char *r = e->kind == 'w' || !e->what || !msg[3] ? msg + 1 : msg + 3;
    e->a = num(&r);
    e->b = num(&r);
    if (e->kind != 'k' && e->kind != 'w' && e->kind != 't') e->kind = 0;
    return e->kind != 0;
}

void plat_show(const pix *canvas) {
    /* turned clockwise onto the portrait panel: view (x, y) is panel (H-1-y, x) */
    unsigned *page = mem + back * line;
    for (int py = 0; py < W; ++py) {
        unsigned *row = page + py * line;
        const unsigned *src = canvas + (H - 1) * W + py;
        for (int px = 0; px < H; ++px, src -= W) row[px] = *src;
    }
    var.yoffset = back;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    back = back ? 0 : W;
}

void plat_close(void) {
    if (mem && var.yoffset != y0) var.yoffset = y0, ioctl(fb, FBIOPAN_DISPLAY, &var);
    if (mem) munmap(mem, mem_len);
    if (fb >= 0) close(fb);
    if (sock >= 0) close(sock), unlink(INPUT_SOCK);
}

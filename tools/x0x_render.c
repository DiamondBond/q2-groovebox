/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * kits/909 and kits/808: X0X's drum models (ER-99 / 9W9, 8W8) rendered to one-shot WAVs, mono 44.1 kHz
 * 16-bit, sixteen a kit, a sound's tail cut where it falls silent or at its limit, peaks balanced by
 * hand. Built and run by tools/upstream.sh against the pinned commit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drum909.h"
#include "drum808.h"

#define FS 44100
#define BLOCK 256
#define MAX (FS * 2)
static float buf[MAX];

static void save(const char *dir, int n, const char *name, int len, float peak)
{
    char path[512];
    float top = 1e-6f;
    int fade = FS / 200, last = 0;
    for (int i = 0; i < len; i++) {
        float a = fabsf(buf[i]);
        if (a > top) top = a;
    }
    for (int i = 0; i < len; i++)
        if (fabsf(buf[i]) > top * 0.001f) last = i + 1; /* -60 dB */
    len = last + fade < len ? last + fade : len;
    snprintf(path, sizeof path, "%s/%02d %s.wav", dir, n, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    unsigned d = (unsigned)len * 2, h[11] = { 0x46464952, 36 + d, 0x45564157, 0x20746d66, 16, 1 | 1 << 16, FS, FS * 2,
                                              2 | 16 << 16, 0x61746164, d };
    fwrite(h, 1, 44, f);
    for (int i = 0; i < len; i++) {
        float e = len - i < fade ? (float)(len - i) / (float)fade : 1;
        short s = (short)lrintf(buf[i] / top * peak * e);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static void set909(drum909_t *d, int voice, const char *param, int value)
{
    for (int i = 0; i < drum909_nparams(voice); i++)
        if (!strcmp(drum909_param(voice, i)->name, param)) { drum909_set(d, voice, i, value); return; }
    fprintf(stderr, "909: no %s on %d\n", param, voice);
    exit(1);
}
static int render909(drum909_t *d, int voice, float secs)
{
    float rev[BLOCK], dly[BLOCK];
    int len = 0, max = (int)(secs * FS);
    drum909_trigger(d, voice, 0.8f);
    while (len + BLOCK <= max) {
        drum909_render(d, buf + len, rev, dly, BLOCK);
        len += BLOCK;
        if (!drum909_active(d)) break;
    }
    return len;
}
static int render808(drum808_t *d, int track, float secs)
{
    float rev[BLOCK], dly[BLOCK];
    int len = 0, max = (int)(secs * FS);
    drum808_trigger(d, track, 0.8f);
    while (len + BLOCK <= max) {
        drum808_render(d, buf + len, rev, dly, BLOCK);
        len += BLOCK;
        if (!drum808_active(d)) break;
    }
    return len;
}

static drum909_t d9;
static drum808_t d8;
static void kit909(const char *dir)
{
    static const struct { int v; const char *name; float secs, peak; const char *param; int value; } S[16] = {
        { DR_BD, "Kick", 0.7f, 30000, 0, 0 },       { DR_SD, "Snare", 0.45f, 28000, 0, 0 },
        { DR_CH, "Hat", 0.2f, 18000, 0, 0 },        { DR_OH, "Open Hat", 0.7f, 18000, 0, 0 },
        { DR_CP, "Clap", 0.5f, 26000, 0, 0 },       { DR_LT, "Tom Lo", 0.6f, 26000, 0, 0 },
        { DR_MT, "Tom Mid", 0.5f, 26000, 0, 0 },    { DR_HT, "Tom Hi", 0.45f, 26000, 0, 0 },
        { DR_RS, "Rim", 0.1f, 22000, 0, 0 },        { DR_CR, "Crash", 1.2f, 20000, 0, 0 },
        { DR_RD, "Ride", 1.2f, 18000, 0, 0 },       { DR_BD, "Kick Long", 1.2f, 30000, "Decay", 118 },
        { DR_BD, "Kick Tight", 0.25f, 30000, "Decay", 40 }, { DR_SD, "Snare Hi", 0.4f, 28000, "Tune", 110 },
        { DR_CP, "Clap Tail", 0.9f, 26000, "Tail", 120 }, { DR_CH, "Hat Tight", 0.1f, 18000, "Decay", 30 },
    };
    for (int i = 0; i < 16; i++) {
        drum909_init(&d9);
        if (S[i].param) set909(&d9, S[i].v, S[i].param, S[i].value);
        save(dir, i + 1, S[i].name, render909(&d9, S[i].v, S[i].secs), S[i].peak);
    }
}
static void kit808(const char *dir)
{
    static const struct { int t; const char *name; float secs, peak; int sw; } S[16] = {
        { D8_BD, "Kick", 1.0f, 30000, 0 },     { D8_SD, "Snare", 0.45f, 28000, 0 }, { D8_CH, "Hat", 0.15f, 18000, 0 },
        { D8_OH, "Open Hat", 0.6f, 18000, 0 }, { D8_CP, "Clap", 0.5f, 26000, 0 },   { D8_LT, "Tom Lo", 0.6f, 26000, 0 },
        { D8_MT, "Tom Mid", 0.5f, 26000, 0 },  { D8_HT, "Tom Hi", 0.45f, 26000, 0 }, { D8_RS, "Rim", 0.1f, 22000, 0 },
        { D8_CY, "Cymbal", 1.2f, 20000, 0 },   { D8_CB, "Cowbell", 0.5f, 22000, 0 }, { D8_RS, "Clave", 0.15f, 22000, 1 },
        { D8_CP, "Maracas", 0.15f, 20000, 1 }, { D8_LT, "Conga Lo", 0.4f, 26000, 1 }, { D8_MT, "Conga Mid", 0.4f, 26000, 1 },
        { D8_HT, "Conga Hi", 0.4f, 26000, 1 },
    };
    for (int i = 0; i < 16; i++) {
        drum808_init(&d8);
        if (S[i].sw)
            for (int k = 0; k < drum808_nparams(S[i].t); k++)
                if (!strcmp(drum808_param(S[i].t, k)->name, "Sound")) drum808_set(&d8, S[i].t, k, 1);
        save(dir, i + 1, S[i].name, render808(&d8, S[i].t, S[i].secs), S[i].peak);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    kit909(argv[1]);
    kit808(argv[2]);
    return 0;
}

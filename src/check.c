/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * make check: the engine, the sequencer and the wheel on the host, asserts only. One compilation
 * unit so the statics are reachable. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "engine.c"
#include "seq.c"
#include "wav.c"
#include "project.c"
#define main sdl_main_unused
#include "platform_sdl.c"
#undef main
#include "ui.c"

static int triggered, trig_pad;
static void count_hits(void) { /* a one-step pattern on pad 3 fires once a bar */
    seq_init(); engine_init();
    static short tone[2000];
    for (int i = 0; i < 2000; ++i) tone[i] = (short)(10000 * sin(i * 0.1));
    sounds[3].smp.pcm = tone; sounds[3].smp.n = 2000;
    seq.pat[0].s[3][4].on = 1; /* step 5 */
    seq_play(1);
    int acc[CTL * 2], barf = seq_step_frames() * NSTEP, first = -1;
    for (int f = 0; f < barf * 2; f += CTL) {
        unsigned before = engine_last_hit(3);
        seq_block(); engine_block(acc);
        if (engine_last_hit(3) != before && first < 0) first = f;
    }
    int want = 4 * seq_step_frames();
    assert(first >= want - CTL && first <= want); /* on its block */
    (void)triggered; (void)trig_pad;
}

int main(void) {
    script = tmpfile(); /* platform_sdl's replay clock: plat_ms() is fake_now */
    /* a sixteenth at 120 is 5512 frames; quantise rounds to the nearest step */
    seq_init();
    assert(seq_step_frames() == 5512);
    assert(seq_quantise(0) == 0 && seq_quantise(2755) == 0 && seq_quantise(2757) == 1 && seq_quantise(5512 * 15 + 2800) == 16);
    /* swing delays the odd steps up to a third of a step */
    seq.swing = 100;
    assert(onset(1, 5512, 0) == 5512 + 5512 / 3 && onset(2, 5512, 0) == 2 * 5512);
    assert(onset(2, 5512, 128) == 2 * 5512 + 5512 / 2); /* a nudge of half a step */
    count_hits();
    /* a cue lands on the bar, a chain alternates */
    seq_init(); seq_play(1); seq_cue(3);
    int barf = seq_step_frames() * NSTEP;
    for (int f = 0; f < barf - CTL; f += CTL) seq_block();
    assert(seq.cur == 0 && seq.cue == 3);
    seq_block(); assert(seq.cur == 3 && seq.cue == -1 && seq.bar == 1);
    seq.chain[0] = 3; seq.chain[1] = 7;
    for (int f = 0; f < barf; f += CTL) seq_block();
    assert(seq.cur == 7);
    /* REC: a tap just after step 2's onset lands on step 2 and sounds; just before, on step 3, silent until it */
    seq_init(); engine_init(); seq_play(1); seq_rec(1);
    static short click[100]; for (int i = 0; i < 100; ++i) click[i] = 3000;
    sounds[0].smp.pcm = click; sounds[0].smp.n = 100;
    seq.pos = 2 * 5512 + 100; seq_tap(0);
    assert(seq.pat[0].s[0][2].on && seq.ntake == 1 && engine_last_hit(0) == 0 && voices[0].on);
    seq.pos = 3 * 5512 - 100; seq_tap(0);
    assert(seq.pat[0].s[0][3].on && seq.ntake == 2);
    seq_undo(); assert(!seq.pat[0].s[0][2].on && !seq.pat[0].s[0][3].on && seq.ntake == 0);
    /* the wheel: 200 units a turn are 15 detents and a remainder; a jitter of +-3 is none */
    ui_init(); int n = 0;
    for (int i = 0; i < 20; ++i) n += detents(10);
    assert(n == 15 && ui.acc == 5);
    ui.acc = 0; n = 0;
    for (int i = 0; i < 50; ++i) n += detents(i & 1 ? 3 : -3);
    assert(n == 0);
    /* the limiter: a +12 dB sum never clips */
    engine_init(); static short loud[4000]; for (int i = 0; i < 4000; ++i) loud[i] = (short)(i & 1 ? 32767 : -32767);
    for (int p = 0; p < 4; ++p) sounds[p].smp.pcm = loud, sounds[p].smp.n = 4000, sounds[p].p[P_LEVEL] = 127, engine_trigger(p, UNSET, UNSET, UNSET, UNSET);
    int acc[CTL * 2];
    for (int b = 0; b < 40; ++b) { engine_block(acc); for (int i = 0; i < CTL * 2; ++i) assert(acc[i] <= 32767 && acc[i] >= -32768); }
    assert(engine_peak() > 20000);
    /* WAV: a stereo write reads back as its mono mean at FS, and a 22050 Hz file is stretched */
    static short st[200]; for (int i = 0; i < 100; ++i) st[2 * i] = (short)(i * 100), st[2 * i + 1] = (short)(i * 100 + 50);
    char tmp[] = "/tmp/gbcheckXXXXXX"; close(mkstemp(tmp));
    assert(wav_write(tmp, st, 100));
    short *rd; int rn = wav_read(tmp, &rd);
    assert(rn == 100 && rd[10] == 1025 && rd[50] == 5025); free(rd);
    unsigned char hdr[44]; FILE *f = fopen(tmp, "r+b"); fread(hdr, 1, 44, f); unsigned r = 22050; memcpy(hdr + 24, &r, 4); r = 22050 * 4; memcpy(hdr + 28, &r, 4); fseek(f, 0, SEEK_SET); fwrite(hdr, 1, 44, f); fclose(f);
    rn = wav_read(tmp, &rd); assert(rn == 200 && rd[20] == 1025); free(rd);
    unlink(tmp);
    /* a project round-trips: steps, a lock, the tempo, a sound's tile */
    seq_init(); engine_init(); seq.bpm = 133; seq.swing = 20; seq.pat[2].s[5][7].on = 1; seq.pat[2].s[5][7].pitch = -5; seq.pat[2].len[5] = 12; sounds[5].p[P_CUTOFF] = 60;
    char d[] = "/tmp/gbprojXXXXXX"; assert(mkdtemp(d)); assert(project_save(d));
    seq_init(); engine_init(); assert(project_load(d));
    assert(seq.bpm == 133 && seq.swing == 20 && seq.pat[2].s[5][7].on && seq.pat[2].s[5][7].pitch == -5 && seq.pat[2].s[5][7].level == UNSET && seq.pat[2].len[5] == 12 && sounds[5].p[P_CUTOFF] == 60 && !seq.pat[2].s[5][6].on);
    char p[300]; snprintf(p, sizeof p, "%s/project.txt", d); unlink(p); rmdir(d);
    /* the stage: a tap on pad 1 (bottom-left) in PLAY selects it; WRITE toggles step 1; a turn while held is no tap */
    seq_init(); engine_init(); ui_init();
    event_t e = { 't', 'd', PAD_X + 5, STRIP + 3 * 72 + 5 }; ui_event(&e, 0); e.what = 'u'; ui_event(&e, 10);
    assert(ui.sound == 0 && pad_at(PAD_X + 5, STRIP + 5) == 12);
    e = (event_t){ 't', 'd', TAB_X + 10, STRIP + TAB_H + 5 }; ui_event(&e, 20); e.what = 'u'; ui_event(&e, 25);
    assert(ui.mode == M_WRITE);
    e = (event_t){ 't', 'd', PAD_X + 5, STRIP + 3 * 72 + 5 }; ui_event(&e, 30); e.what = 'u'; ui_event(&e, 40);
    assert(seq.pat[0].s[0][0].on);
    e.what = 'd'; ui_event(&e, 50); event_t w = { 'w', 0, -13, 0 }; ui_event(&w, 55); e.what = 'u'; ui_event(&e, 60);
    assert(seq.pat[0].s[0][0].on && seq.pat[0].s[0][0].level == 101); /* held + a clockwise detent: a lock, still on */
    /* Centre: a tap toggles REC, a hold with the wheel is the volume */
    event_t k = { 'k', 'd', KEY_CENTRE, 0 }; ui_event(&k, 100); k.what = 'u'; ui_event(&k, 150); assert(seq.rec == 1);
    k.what = 'd'; ui_event(&k, 200); ui_tick(600); ui_event(&w, 610); k.what = 'u'; ui_event(&k, 620);
    assert(seq.rec == 1 && ui.vol == 100); /* at the top already */
    w.a = 13; k.what = 'd'; ui_event(&k, 700); ui_event(&w, 710); k.what = 'u'; ui_event(&k, 720); assert(ui.vol == 95 && seq.rec == 1);
    /* Return undoes the take; Prev/Next cue; idle quits after 5 minutes stopped */
    k = (event_t){ 'k', 'd', KEY_NEXT, 0 }; ui_event(&k, 800); assert(seq.cur == 1);
    seq_rec(1); seq_tap(1); k = (event_t){ 'k', 'd', KEY_RETURN, 0 }; ui_event(&k, 810); k.what = 'u'; ui_event(&k, 820);
    ui_tick(820 + IDLE_QUIT_MS + 1); assert(ui_quit());
    /* the review's cases: the idle clock starts at the app's start, not the device's boot */
    seq_init(); engine_init(); fake_now = 3600 * 1000; ui_init(); ui_tick(3600 * 1000 + 1000); assert(!ui_quit()); fake_now = 0;
    /* Start 127 with Loop on: the voice keeps to the sample (ASan watches); a sample past 65535 frames plays whole */
    engine_init(); static short big[70560]; for (int i = 0; i < 70560; ++i) big[i] = (short)(i & 1 ? 3000 : -3000);
    sounds[0].smp.pcm = big; sounds[0].smp.n = 70560; sounds[0].p[P_START] = 127; sounds[0].p[P_LOOP] = 1;
    engine_trigger(0, UNSET, UNSET, UNSET, UNSET); for (int b = 0; b < 200; ++b) engine_block(acc); assert(voices[0].on);
    sounds[0].p[P_START] = 0; sounds[0].p[P_LOOP] = 0; engine_trigger(0, UNSET, UNSET, UNSET, UNSET);
    int blocks_on = 0; for (int b = 0; b < 3000 && voices[0].on; ++b) engine_block(acc), ++blocks_on;
    assert(blocks_on >= 70560 / CTL - 1 && blocks_on <= 70560 / CTL + 1);
    /* an attack rises to full, then the decay falls: the voice ends, and not on its first frame */
    sounds[0].p[P_ATTACK] = 60; sounds[0].p[P_DECAY] = 40; sounds[0].p[P_LOOP] = 1; engine_trigger(0, UNSET, UNSET, UNSET, UNSET);
    int peak_b = 0, pk = 0; blocks_on = 0;
    for (int b = 0; b < 20000 && voices[0].on; ++b) { engine_block(acc); ++blocks_on; if (engine_peak() > pk) pk = engine_peak(), peak_b = b; }
    assert(blocks_on > 100 && !voices[0].on && pk > 800 && peak_b > 10); /* 3000 x level 100's 0.31 a side */
    /* a pitch lock is the pitch itself: +5 on the sound, a lock of 6 plays 6 semitones */
    sounds[0].p[P_ATTACK] = 0; sounds[0].p[P_DECAY] = 127; sounds[0].p[P_LOOP] = 0; sounds[0].p[P_PITCH] = 5;
    engine_trigger(0, UNSET, 6, UNSET, UNSET); assert(voices[0].inc == (int)(65536 * exp2f(6 / 12.0f)));
    /* undo clears the take in the pattern it was recorded in, after a cue moved on */
    seq_init(); engine_init(); seq_play(1); seq_rec(1); sounds[0].smp.pcm = click; sounds[0].smp.n = 100;
    seq.pos = 100; seq_tap(0); assert(seq.pat[0].s[0][0].on); seq_cue(2); for (int f = 0; f < 5512 * NSTEP; f += CTL) seq_block();
    assert(seq.cur == 2); seq_undo(); assert(!seq.pat[0].s[0][0].on);
    /* a REC tap just past an odd step's straight grid, with swing, waits for the swung onset: one hit */
    seq_init(); engine_init(); seq_play(1); seq_rec(1); seq.swing = 100; sounds[0].smp.pcm = click; sounds[0].smp.n = 100;
    seq.pos = 5512 + 100; seq_tap(0); assert(seq.pat[0].s[0][1].on && !voices[0].on);
    for (int f = 5512 + 100; f < 5512 * 2; f += CTL) { seq.pos = f; seq_block(); } assert(voices[0].on);
    /* PATT: a held pattern chains with the current one; Return releases it */
    seq_init(); engine_init(); ui_init(); ui.mode = M_PATT;
    e = (event_t){ 't', 'd', PAD_X + 5 + 72, STRIP + 3 * 72 + 5 }; ui_event(&e, 10); ui_tick(10 + HOLD_MS); e.what = 'u'; ui_event(&e, 400);
    assert(seq.chain[0] == 0 && seq.chain[1] == 1 && seq.cur == 0);
    k = (event_t){ 'k', 'd', KEY_RETURN, 0 }; ui_event(&k, 500); assert(seq.chain[0] == 0); k.what = 'u'; ui_event(&k, 550); assert(seq.chain[0] == -1 && seq.chain[1] == -1);
    puts("check: ok");
    return 0;
}

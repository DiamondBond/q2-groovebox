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
#include "load.c"
#include "fx_signal.h"
#include "fx_golden.h"
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
    assert(wav_write(tmp, st, 100, 2));
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
    /* End 0 with Loop on stays inside the sample (ASan watches) */
    seq_init(); engine_init(); static short two[2] = { 100, 200 }; sounds[0].smp.pcm = two; sounds[0].smp.n = 2; sounds[0].p[P_END] = 0; sounds[0].p[P_LOOP] = 1;
    engine_trigger(0, UNSET, UNSET, UNSET, UNSET); assert(voices[0].start == 0 && voices[0].end == 1 << 16); for (int b = 0; b < 4; ++b) engine_block(acc); sounds[0].smp.pcm = 0; sounds[0].smp.n = 0;
    /* Reverse keeps to the region: Start 0, End 64 plays the first half backwards, from its end to the start */
    seq_init(); engine_init(); static short ramp[1270]; for (int i = 0; i < 1270; ++i) ramp[i] = (short)(i * 10);
    sounds[0].smp.pcm = ramp; sounds[0].smp.n = 1270; sounds[0].p[P_END] = 64; sounds[0].p[P_REVERSE] = 1; sounds[0].p[P_LEVEL] = 127;
    engine_trigger(0, UNSET, UNSET, UNSET, UNSET); engine_block(acc); assert(acc[0] > 3100 && acc[0] < 3250 && acc[2 * 31] < acc[0]);
    for (int b = 0; b < 18; ++b) engine_block(acc);
    assert(voices[0].on && acc[2 * 31] > 0 && acc[2 * 31] < 200);
    /* every lifted effect gives SLOOP's own output on the same input (tools/sloop_golden.c), and ends bounded */
    for (unsigned fx = 0; fx < NFX; ++fx) {
        int l[CTL], r[CTL], d_[CTL], v_[CTL]; unsigned h = 2166136261u; fx_init();
        for (unsigned b = 0; b < GOLDEN_BLOCKS; ++b) {
            fx_clock(1, 120, (int)(b * CTL % 22050u)); fx_live(b >= GOLDEN_ON && b < GOLDEN_OFF ? (int)fx : -1);
            golden_signal(b, l, r, d_, v_); fx_buses_block(l, r, d_, v_); fx_punch_block(l, r);
            h = golden_hash(golden_hash(h, l), r);
        }
        assert(h == fx_golden[fx]);
    }
    /* sixteen full-scale voices with both sends up, through every effect: no overflow (UBSan), the limiter holds */
    { static short hot[8000]; for (int i = 0; i < 8000; ++i) hot[i] = (short)(i & 8 ? 32767 : -32768);
      for (int fx = 0; fx < NFX; fx += 3) { engine_init(); fx_live(fx);
          for (int p = 0; p < NPAD; ++p) { sounds[p].smp.pcm = hot; sounds[p].smp.n = 8000; sounds[p].p[P_LEVEL] = 127; sounds[p].p[P_ECHO] = 127; sounds[p].p[P_REVERB] = 127; sounds[p].p[P_PAN] = -64; sounds[p].p[P_START] = 0; sounds[p].p[P_END] = 127; sounds[p].p[P_REVERSE] = 0; engine_trigger(p, UNSET, UNSET, UNSET, UNSET); }
          for (int b = 0; b < 300; ++b) { engine_block(acc); for (int i = 0; i < CTL * 2; ++i) assert(acc[i] <= 32767 && acc[i] >= -32768); } }
      for (int p = 0; p < NPAD; ++p) sounds[p].smp.pcm = 0, sounds[p].smp.n = 0; }
    /* the echo send sounds on after the sample has ended */
    engine_init(); static short blip[200]; for (int i = 0; i < 200; ++i) blip[i] = (short)(i & 4 ? 20000 : -20000);
    sounds[1].smp.pcm = blip; sounds[1].smp.n = 200; sounds[1].p[P_ECHO] = 127; engine_trigger(1, UNSET, UNSET, UNSET, UNSET);
    { int heard = 0; for (int b = 0; b < 800; ++b) { engine_block(acc); if (!voices[1].on) for (int i = 0; i < CTL * 2; ++i) heard |= acc[i] != 0; } assert(heard); }
    sounds[1].smp.pcm = 0; sounds[1].smp.n = 0;
    /* a swap cuts the pad's running voice and frees the old sound (ASan watches the next block) */
    { for (int p = 0; p < NPAD; ++p) sounds[p].smp = (sample_t){ 0 };
      short *a = calloc(4000, 2), *b = calloc(4000, 2); sound_swap(0, a, 4000, "A", 0); engine_trigger(0, UNSET, UNSET, UNSET, UNSET); engine_block(acc); assert(voices[0].on);
      sound_swap(0, b, 4000, "B", 1); assert(!voices[0].on && sounds[0].smp.pcm == b && !strcmp(sounds[0].smp.name, "B") && sounds[0].smp.loaded); engine_block(acc);
      sound_swap(0, 0, 0, "", 0); assert(!sounds[0].smp.pcm); }
    /* a project round-trips its kit, a loaded sound's name and FX events; a bad event is dropped */
    { seq_init(); engine_init(); strcpy(kit_name, "909"); seq.pat[1].ev[0] = (fxev_t){ 4, 2, 7 }; seq.pat[1].nev = 1; sounds[3].smp.loaded = 1; strcpy(sounds[3].smp.name, "Foo");
      char pd[] = "/tmp/gbp2XXXXXX", pf[300]; assert(mkdtemp(pd)); assert(project_save(pd));
      sounds[3].smp = (sample_t){ 0 }; seq_init(); strcpy(kit_name, "x"); assert(project_load(pd));
      assert(!strcmp(kit_name, "909") && seq.pat[1].nev == 1 && seq.pat[1].ev[0].step == 4 && seq.pat[1].ev[0].len == 2 && seq.pat[1].ev[0].fx == 7 && !strcmp(pad_names[3], "Foo"));
      snprintf(pf, sizeof pf, "%s/project.txt", pd); FILE *t = fopen(pf, "w"); fputs("p1.fx=99:1:0 3:99:1 3:2:50 2:2:5 \n", t); fclose(t); seq_init(); project_load(pd);
      assert(seq.pat[0].nev == 1 && seq.pat[0].ev[0].step == 2 && seq.pat[0].ev[0].fx == 5); unlink(pf); rmdir(pd); memset(pad_names, 0, sizeof pad_names); }
    /* a WAV write leaves no .new and mono reads back; a longer file stops at twenty seconds */
    { char w1[] = "/tmp/gbw1XXXXXX", nw[40]; close(mkstemp(w1)); short *lng = calloc(FS * 21, 2); assert(wav_write(w1, lng, FS * 21, 1)); snprintf(nw, sizeof nw, "%s.new", w1); assert(access(nw, F_OK));
      short *rd2; assert(wav_read(w1, &rd2) == FS * SOUND_SECONDS); free(rd2); free(lng); unlink(w1); }
    /* a kit: fewer than sixteen files empties the rest; a missing or path-like name falls back to default */
    char root[] = "/tmp/gbrootXXXXXX", pth[400]; assert(mkdtemp(root));
    snprintf(dir, sizeof dir, "%s", root);
    { static const char *const mk[] = { "kits", "kits/default", "kits/other", "projects", "projects/1", "media", "media/sub", "media/Music", 0 };
      for (int i = 0; mk[i]; ++i) { snprintf(pth, sizeof pth, "%s/%s", root, mk[i]); assert(!mkdir(pth, 0755)); } }
    static short w4[1600]; for (int i = 0; i < 1600; ++i) w4[i] = (short)(i * 7);
    snprintf(pth, sizeof pth, "%s/kits/default/01 Kick.wav", root); assert(wav_write(pth, w4, 1600, 1));
    snprintf(pth, sizeof pth, "%s/kits/other/01 Bee.wav", root); assert(wav_write(pth, w4, 1600, 1));
    for (int p = 0; p < NPAD; ++p) sounds[p].smp = (sample_t){ 0 };
    short *pre = calloc(100, 2); sound_swap(5, pre, 100, "Old", 1);
    kit_open("other"); assert(!strcmp(kit_name, "other") && !strcmp(sounds[0].smp.name, "Bee") && sounds[0].smp.n == 1600 && !sounds[5].smp.pcm && !sounds[5].smp.loaded);
    kit_open("../other"); assert(!strcmp(kit_name, "default") && !strcmp(sounds[0].smp.name, "Kick"));
    kit_open("nope"); assert(!strcmp(kit_name, "default"));
    /* the picker: the kits lead, then folders, then sounds; Centre and Return are its own while open */
    snprintf(pth, sizeof pth, "%s/media", root); setenv("GROOVEBOX_MEDIA", pth, 1);
    snprintf(pth, sizeof pth, "%s/media/it's a $test.wav", root); assert(wav_write(pth, w4, 1600, 1));
    snprintf(pth, sizeof pth, "%s/media/notes.txt", root); FILE *nt = fopen(pth, "w"); fclose(nt);
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.mode = M_SOUND; ui.sound = 2;
    e = (event_t){ 't', 'd', pad_x(P_LOAD) + 5, pad_y(P_LOAD) + 5 }; ui_event(&e, 10); e.what = 'u'; ui_event(&e, 20); ui_tick(25);
    assert(pick.open && pick.n == 5 && pick.row[0].kind == 'k' && !strcmp(pick.row[0].name, "default") && pick.row[1].kind == 'k' && pick.row[2].kind == 'd' && !strcmp(pick.row[2].name, "Music") && !strcmp(pick.row[3].name, "sub") && pick.row[4].kind == 'f');
    { static pix cv[W * H]; ui_render(cv, 30); }
    w = (event_t){ 'w', 0, -13 * 2, 0 }; ui_event(&w, 40); assert(pick.cur == 2);
    k = (event_t){ 'k', 'd', KEY_CENTRE, 0 }; ui_event(&k, 50); k.what = 'u'; ui_event(&k, 60); ui_tick(65); assert(!strcmp(strrchr(pick.dir, '/'), "/Music") && pick.n == 0 && !seq.rec);
    k = (event_t){ 'k', 'd', KEY_RETURN, 0 }; ui_event(&k, 70); k.what = 'u'; ui_event(&k, 80); ui_tick(85); assert(pick.open && pick.n == 5 && !seq.ntake);
    w.a = -13 * 4; ui_event(&w, 90); assert(pick.cur == 4);
    k = (event_t){ 'k', 'd', KEY_CENTRE, 0 }; ui_event(&k, 100); k.what = 'u'; ui_event(&k, 110); assert(pick.asking && pick.n == 1 + REGIONS && pick.row[0].kind == 'c' && !strncmp(pick.row[1].name, "To pad 3", 8));
    k = (event_t){ 'k', 'd', KEY_RETURN, 0 }; ui_event(&k, 120); k.what = 'u'; ui_event(&k, 130); ui_tick(135); assert(!pick.asking && pick.cur == 4);
    /* a tap below the last drawn row picks nothing; a stray file in kits/ is no kit; a Centre held as the picker opens lets go */
    { int was = pick.n; pick.n = 40; e = (event_t){ 't', 'd', 20, H - 1 }; int cur = pick.cur; ui_event(&e, 136); assert(pick.cur == cur && !pick.asking); pick.n = was; e.what = 'u'; ui_event(&e, 137);
      snprintf(pth, sizeof pth, "%s/kits/stray.wav", root); assert(wav_write(pth, w4, 1600, 1)); pick_back(); ui_tick(138); pick_open(); ui_tick(139); assert(pick.n == 5 && !kit_choose("stray.wav")); unlink(pth);
      ui.centre = ui.centre_hold = 1; k = (event_t){ 'k', 'u', KEY_CENTRE, 0 }; ui_event(&k, 140); assert(pick.open && !ui.centre && !ui.centre_hold); }
    /* a card file through ffmpeg, a name with a quote and a $ (no shell); one region to the pad, then a chop */
    if (!access(FFMPEG, X_OK)) {
        snprintf(pth, sizeof pth, "%s/media/it's a $test.wav", root);
        assert(load_pad(2, pth, 0) == 1 && sounds[2].smp.n == 1600 && sounds[2].smp.pcm[100] == 700 && !strcmp(sounds[2].smp.name, "it's a $test") && sounds[2].smp.loaded);
        snprintf(pth, sizeof pth, "%s/projects/1/pad03.wav", root); assert(!access(pth, F_OK));
        char miss[300]; snprintf(miss, sizeof miss, "%s/media/gone.mp3", root); assert(load_pad(2, miss, 0) == 0 && sounds[2].smp.n == 1600);
        /* unsaved (the card refuses): the pad plays on, but its older WAV must not come back under the new name */
        { char pd2[300]; snprintf(pd2, sizeof pd2, "%s/projects/1", root); chmod(pd2, 0555); short *u = calloc(10, 2); int ro = access(pd2, W_OK);
          if (ro) { assert(!place(4, u, 10, "U") && !sounds[4].smp.loaded); } else sound_swap(4, u, 10, "U", 0);
          chmod(pd2, 0755); }
        sounds[2].p[P_START] = 40; snprintf(pth, sizeof pth, "%s/media/it's a $test.wav", root); assert(load_chop(pth) == 1);
        assert(sounds[0].smp.n == 100 && sounds[15].smp.n == 100 && sounds[1].smp.pcm[0] == 700 && !strcmp(sounds[15].smp.name, "Slice 16") && sounds[2].p[P_START] == 0);
        /* they come back after the kit, under their names, and a kit change forgets them */
        strcpy(kit_name, "default"); snprintf(pth, sizeof pth, "%s", project_path()); assert(project_save(pth));
        kit_open("default"); assert(!strcmp(sounds[0].smp.name, "Kick")); memset(pad_names, 0, sizeof pad_names); assert(project_load(pth)); pads_restore(pth);
        assert(sounds[3].smp.n == 100 && !strcmp(sounds[3].smp.name, "Slice 4") && sounds[3].smp.loaded);
        pads_forget(pth); snprintf(pth, sizeof pth, "%s/projects/1/pad03.wav", root); assert(access(pth, F_OK));
        snprintf(pth, sizeof pth, "%s", project_path()); strcat(pth, "/project.txt"); unlink(pth);
    }
    /* FX: a held pad runs its effect, a Centre-held one latches until the next tap, which does only that */
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.mode = M_FX;
    event_t f0 = { 't', 'd', pad_x(1) + 5, pad_y(1) + 5 }, f1 = { 't', 'd', pad_x(4) + 5, pad_y(4) + 5 };
    ui_event(&f0, 10); assert(fx_sounding() == fx_page[0][1]); f0.what = 'u'; ui_event(&f0, 20); assert(fx_sounding() == -1);
    k = (event_t){ 'k', 'd', KEY_CENTRE, 0 }; ui_event(&k, 30); f1.what = 'd'; ui_event(&f1, 40); f1.what = 'u'; ui_event(&f1, 50); k.what = 'u'; ui_event(&k, 60); assert(fx_sounding() == fx_page[0][4] && ui.fx_latched && !seq.rec);
    f0.what = 'd'; ui_event(&f0, 70); assert(fx_sounding() == -1 && !ui.fx_latched); f0.what = 'u'; ui_event(&f0, 80); assert(fx_sounding() == -1);
    w = (event_t){ 'w', 0, -13, 0 }; ui_event(&w, 90); assert(ui.fx_page == 1); w.a = 13; ui_event(&w, 95); assert(ui.fx_page == 0);
    /* with REC on it is written quantised, played back, undone, and Return with nothing to undo clears the bar */
    seq_play(1); seq_rec(1); seq.pos = 2 * 5512 + 100; f0.what = 'd'; ui_event(&f0, 100); seq.pos = 6 * 5512 + 50; f0.what = 'u'; ui_event(&f0, 110);
    assert(seq.pat[0].nev == 1 && seq.pat[0].ev[0].step == 2 && seq.pat[0].ev[0].len == 4 && seq.pat[0].ev[0].fx == fx_page[0][1] && fx_sounding() == -1);
    seq.pos = 3 * 5512; seq_block(); assert(fx_sounding() == fx_page[0][1]); seq.pos = 7 * 5512; seq_block(); assert(fx_sounding() == -1);
    seq_undo(); assert(seq.pat[0].nev == 0 && seq.ntake == 0);
    seq.pat[0].nev = 3; k = (event_t){ 'k', 'd', KEY_RETURN, 0 }; ui_event(&k, 200); k.what = 'u'; ui_event(&k, 210); assert(seq.pat[0].nev == 0);
    seq_play(0); seq_rec(0); assert(fx_sounding() == -1);
    /* export: the bar, or both of a chain, to a numbered file; the transport comes back as it was */
    { seq_init(); engine_init(); for (int p = 0; p < NPAD; ++p) if (p != 2) sound_swap(p, 0, 0, "", 0);
      sound_swap(2, calloc(1600, 2), 1600, "A", 0);
      for (int i = 0; i < 1600; ++i) sounds[2].smp.pcm[i] = (short)(i & 8 ? 9000 : -9000);
      seq.pat[0].s[2][0].on = 1; seq.pat[1].s[2][8].on = 1; seq.cur = 1; engine_gain(1000); engine_sweep(50);
      char nm[32]; assert(export_pattern(nm, sizeof nm) && !strcmp(nm, "1-1.wav") && seq.cur == 1 && !seq.playing && !engine_is_offline() && eg.gain == 1000 && eg.sweep == 50);
      engine_gain(65536); engine_sweep(0); snprintf(pth, sizeof pth, "%s/media/Music/Groovebox/1-1.wav", root); short *x; int xn = wav_read(pth, &x);
      assert(xn == seq_step_frames() * NSTEP); { int q = 0; for (int i = 0; i < xn; ++i) q |= x[i]; assert(q); } free(x);
      seq.chain[0] = 0; seq.chain[1] = 1; assert(export_pattern(nm, sizeof nm) && !strcmp(nm, "1-2.wav") && seq.cur == 1);
      snprintf(pth, sizeof pth, "%s/media/Music/Groovebox/1-2.wav", root); xn = wav_read(pth, &x); assert(xn == 2 * seq_step_frames() * NSTEP); free(x); }
    /* a strip tap in PATT asks for one, run off the lock once the strip has said so */
    seq_init(); engine_init(); ui_init(); ui.mode = M_PATT; e = (event_t){ 't', 'd', 200, 5 }; ui_event(&e, 10); assert(ui.job == J_EXPORT && !ui.job_shown);
    ui_tick(11); assert(ui.job == J_EXPORT); { static pix cv[W * H]; ui_render(cv, 12); } assert(ui.job_shown); ui_tick(13); assert(!ui.job && !strncmp(ui.msg, "Saved 1-3", 9));
    { char cmd[64]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); assert(!system(cmd)); }
    for (int p = 0; p < NPAD; ++p) sound_swap(p, 0, 0, "", 0);
    puts("check: ok");
    return 0;
}

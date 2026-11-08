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
#include "drop.c"
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
    int acc[CTL * 2], barf = seq_step_frames() * BAR, first = -1;
    for (int f = 0; f < barf * 2; f += CTL) {
        unsigned before = engine_last_hit(3);
        seq_block(); engine_block(acc);
        if (engine_last_hit(3) != before && first < 0) first = f;
    }
    int want = 4 * seq_step_frames();
    assert(first >= want - CTL && first <= want); /* on its block */
    (void)triggered; (void)trig_pad;
}
static int hits_in(int pad, int frames) { /* the blocks the pad was struck in, the transport running */
    int acc[CTL * 2], n = 0;
    engine_block(acc); /* a hit in block 0 would look like none */
    for (int f = 0; f < frames; f += CTL) {
        unsigned was = engine_last_hit(pad);
        seq_block();
        n += engine_last_hit(pad) != was;
        engine_block(acc);
    }
    return n;
}
static int louder(int blocks, int floor) { /* the transport stopped: any sample past floor */
    int acc[CTL * 2], heard = 0;
    for (int b = 0; b < blocks; ++b) {
        engine_block(acc);
        for (int i = 0; i < CTL * 2; ++i) {
            assert(acc[i] <= 32767 && acc[i] >= -32768);
            heard |= abs(acc[i]) > floor;
        }
    }
    return heard;
}
static void clear_pads(void) { for (int p = 0; p < NPAD; ++p) sound_swap(p, 0, 0, "", 0); }
static short *tone_pcm(int n) {
    short *t = malloc((size_t)n * sizeof *t);
    for (int i = 0; i < n; ++i) t[i] = (short)(i & 8 ? 12000 : -12000);
    return t;
}
static event_t tap_of(int x, int y, char what) { return (event_t){ 't', what, x, y }; }
static void tap_at(int x, int y, long long at) {
    event_t t = tap_of(x, y, 'd');
    ui_event(&t, at);
    t.what = 'u';
    ui_event(&t, at + 1);
}
static void tap_pad(int i, long long at) { tap_at(cell_x(i) + 5, cell_y(i) + 5, at); }
static void tap_tab(int m, long long at) { tap_at(tab_x(m) + 10, TABS_Y + 5, at); }
static void press(int code, long long at) {
    event_t k = { 'k', 'd', code, 0 };
    ui_event(&k, at);
    k.what = 'u';
    ui_event(&k, at + 1);
}
static void centre_on_row(int row, long long at) { pick.cur = row, press(KEY_CENTRE, at); }
static void run_job(long long at) { static pix cv[W * H]; ui_render(cv, at); ui_tick(at + 1); }
static void tap_box(int i, long long at) { tap_at(box_x(i) + 5, box_y(i) + 5, at); }
static void tap_left(int n, int k, long long at) { tap_at(SIDE + 5, left_y(n, k) + 5, at); }
static void hold_step(int i, long long at) { event_t h = tap_of(cell_x(i) + 5, cell_y(i) + 5, 'd'); ui_event(&h, at); ui_tick(at + HOLD_MS); h.what = 'u'; ui_event(&h, at + HOLD_MS + 1); }
static int glass_x(int y) { /* the width the glass hides on row y */
    int d = y < GLASS_R ? GLASS_R - y : y >= H - GLASS_R ? y - (H - GLASS_R) + 1 : 0;
    return d ? (int)ceil(GLASS_R - sqrt(GLASS_R * GLASS_R - d * d)) : 0;
}
static void on_glass(int x, int y, int w, int h) {
    assert(y >= 0 && y + h <= H);
    assert(x >= glass_x(y) && x >= glass_x(y + h - 1) && x + w <= W - glass_x(y) && x + w <= W - glass_x(y + h - 1));
}
static void all_screens(long long at) { /* every screen on this sound, ASan and UBSan watching, each hint fitting its line */
    static pix cv[W * H];
    char b[120];
    int id[32];
#define SHOW (ui_render(cv, at), hint_text(b, sizeof b), assert(text_w(b, 0) <= hint_w()))
    for (ui.mode = 0; ui.mode < M_N; ++ui.mode) {
        ui.over = O_NONE;
        for (ui.pads = 0; ui.pads < 3; ++ui.pads) SHOW;
        ui.pads = 0;
        for (ui.over = O_NONE; ui.over <= O_SET; ++ui.over) {
            if (ui.over == O_SOUND) continue;
            for (ui.box_page = 0; ui.box_page * BOXES < (boxes_on() ? boxes(id) : 1); ++ui.box_page) SHOW;
        }
    }
    ui.mode = ui.over = ui.box_page = ui.box = 0;
#undef SHOW
}
static const char *drop_code(void) { return d.code; }
static int kit_sub(int src, int slot) { int sub, e[NTILE], choke; const char *nm; x0x_kit(src, slot, &sub, e, &nm, &choke); return sub; }
static int http(const char *head, const char *body) { /* one request to the drop server: the status */
    int c = socket(AF_INET, SOCK_STREAM, 0), st = 0;
    struct sockaddr_in a;
    char r[64] = "";
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET, a.sin_port = htons(DROP_PORT), a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!connect(c, (struct sockaddr *)&a, sizeof a));
    assert(send(c, head, strlen(head), 0) == (ssize_t)strlen(head));
    if (body) assert(send(c, body, strlen(body), 0) == (ssize_t)strlen(body));
    (void)!recv(c, r, sizeof r - 1, 0);
    close(c);
    sscanf(r, "HTTP/1.1 %d", &st);
    return st;
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
    int barf = seq_step_frames() * BAR;
    for (int f = 0; f < barf - CTL; f += CTL) seq_block();
    assert(seq.cur == 0 && seq.cue == 3);
    seq_block(); assert(seq.cur == 3 && seq.cue == -1 && seq.bar == 1);
    seq_song_add(3); seq_song_add(7);
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
    /* Beat: step 1 is top-left, a tap turns it on and off; a hold opens it instead, the wheel there writes it, Return comes back */
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick);
    event_t w = { 'w', 0, -13, 0 }; /* a clockwise detent */
    { const step_t *s5 = &seq.pat[0].s[0][5];
      assert(ui.mode == M_BEAT); tap_pad(0, 30); assert(seq.pat[0].s[0][0].on); tap_pad(0, 40); assert(!seq.pat[0].s[0][0].on);
      event_t h = tap_of(cell_x(5) + 5, cell_y(5) + 5, 'd'); ui_event(&h, 50); ui_tick(50 + HOLD_MS - 1); assert(!ui.over); ui_tick(50 + HOLD_MS); assert(ui.over == O_STEP && ui.step == 5);
      h.what = 'u'; ui_event(&h, 400); assert(!s5->on); /* the lift isn't a tap */
      tap_box(S_LEVEL, 410); ui_event(&w, 420); assert(s5->on && s5->level == 101);
      tap_box(S_NOTE, 421); ui_event(&w, 422); assert(s5->pitch == 1);
      tap_box(S_ROLL, 430); ui_event(&w, 431); assert(s5->ratchet == 2);
      tap_box(S_CHANCE, 432); w.a = 13; ui_event(&w, 433); w.a = -13; assert(s5->chance == 95);
      tap_box(S_ACCENT, 434); tap_box(S_SLIDE, 435); assert(s5->flags == (NOTE_ACCENT | NOTE_SLIDE));
      tap_box(S_NUDGE, 436); ui_event(&w, 437); assert(s5->nudge == 8);
      press(KEY_RETURN, 440); assert(!ui.over && s5->on && !seq.ntake);
      hold_step(5, 450); assert(ui.over == O_STEP); tap_left(5, 3, 800); assert(!ui.over && !s5->on && s5->level == UNSET && !s5->flags); }
    /* the wheel walks the sounds, the list's window following; tapping a row picks it */
    for (int i = 0; i < 5; ++i) ui_event(&w, 810);
    assert(ui.sound == 5 && ui.list_top == 2);
    tap_at(SIDE + 5, BODY_Y + 1 * LIST_ROW + 5, 811); assert(ui.sound == 3);
    w.a = 13; for (int i = 0; i < 20; ++i) ui_event(&w, 812);
    w.a = -13; assert(!ui.sound && !ui.list_top);
    /* the top row: Start runs; Tempo lends the wheel until anything else is touched; so does Volume */
    tap_at(pill_x(0) + 5, TOP_Y + 5, 820); assert(seq.playing); tap_at(pill_x(0) + 5, TOP_Y + 5, 821); assert(!seq.playing);
    tap_at(pill_x(1) + 5, TOP_Y + 5, 822); assert(ui.wheel_to == WT_BPM); ui_event(&w, 823); assert(seq.bpm == 121 && !ui.sound);
    tap_pad(3, 824); assert(ui.wheel_to == WT_MODE); ui_event(&w, 825); assert(seq.bpm == 121 && ui.sound == 1);
    tap_at(pill_x(2) + 5, TOP_Y + 5, 826); w.a = 13; ui_event(&w, 827); w.a = -13; assert(ui.vol == 95); tap_at(pill_x(2) + 5, TOP_Y + 5, 828); assert(ui.wheel_to == WT_MODE);
    /* Record starts the beat; Stop ends the take and Return still undoes it; then there is nothing left to undo */
    seq_init(); engine_init(); ui_init(); tap_tab(M_PADS, 830); assert(ui.mode == M_PADS);
    tap_left(4, 3, 831); assert(seq.rec && seq.playing); seq.pos = 100; tap_pad(1, 832); assert(seq.pat[0].s[1][0].on && ui.sound == 1);
    tap_at(pill_x(0) + 5, TOP_Y + 5, 833); assert(!seq.playing && !seq.rec);
    press(KEY_RETURN, 834); assert(!seq.pat[0].s[1][0].on && !strcmp(ui.msg, "Undone")); press(KEY_RETURN, 835); assert(!strcmp(ui.msg, "Nothing to undo"));
    /* the sweep leaves with its view */
    { event_t s3 = { 'w', 0, -39, 0 }; tap_left(4, 0, 836); ui_event(&s3, 837); assert(eg.sweep == 39); tap_left(4, 1, 838); assert(!eg.sweep && !ui.sweep); tap_left(4, 0, 839); ui_event(&s3, 840); assert(eg.sweep == 39); tap_tab(M_BEAT, 841); assert(!eg.sweep && !ui.sweep); }
    /* Next cues the next beat where there is no page to turn; on Sound it is the next sound; idle quits after 5 minutes stopped */
    tap_tab(M_SONG, 842); press(KEY_NEXT, 843); assert(seq.cur == 1); tap_tab(M_SOUND, 844); press(KEY_NEXT, 845); assert(ui.sound == 2 && seq.cur == 1);
    ui_tick(845 + IDLE_QUIT_MS + 2); assert(ui_quit());
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
    seq.pos = 100; seq_tap(0); assert(seq.pat[0].s[0][0].on); seq_cue(2); for (int f = 0; f < 5512 * BAR; f += CTL) seq_block();
    assert(seq.cur == 2); seq_undo(); assert(!seq.pat[0].s[0][0].on);
    /* a REC tap just past an odd step's straight grid, with swing, waits for the swung onset: one hit */
    seq_init(); engine_init(); seq_play(1); seq_rec(1); seq.swing = 100; sounds[0].smp.pcm = click; sounds[0].smp.n = 100;
    seq.pos = 5512 + 100; seq_tap(0); assert(seq.pat[0].s[0][1].on && !voices[0].on);
    for (int f = 5512 + 100; f < 5512 * 2; f += CTL) { seq.pos = f; seq_block(); } assert(voices[0].on);
    /* Song: a beat tapped is played; Add to song starts the list; Remove last takes one off; Copy beat goes to the next empty one */
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.mode = M_SONG;
    tap_pad(1, 10); assert(seq.cur == 1); tap_left(5, 0, 20); assert(seq.nsong == 1 && seq.song[0] == 1);
    tap_pad(2, 30); tap_left(5, 0, 40); assert(seq.nsong == 2 && seq.song[1] == 2 && seq.cur == 2); tap_left(5, 1, 50); assert(seq.nsong == 1);
    seq.pat[2].s[3][4].on = 1; seq.pat[4].s[1][1].on = 1; tap_left(5, 2, 60); assert(seq.pat[3].s[3][4].on && seq.cur == 3 && !strcmp(ui.msg, "Copied to Beat 4"));
    tap_left(5, 2, 62); assert(seq.pat[5].s[3][4].on && seq.cur == 5); /* Beat 5 had steps: over it */
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
    /* the picker: what a sound can be first, then the kits or the card, folders before files; Back walks up */
    snprintf(pth, sizeof pth, "%s/media", root); setenv("GROOVEBOX_MEDIA", pth, 1);
    snprintf(pth, sizeof pth, "%s/media/it's a $test.wav", root); assert(wav_write(pth, w4, 1600, 1));
    snprintf(pth, sizeof pth, "%s/media/notes.txt", root); FILE *nt = fopen(pth, "w"); fclose(nt);
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.sound = 2;
    tap_left(5, 3, 10); assert(pick.open && pick.menu == PM_ROOT && pick.n == R_PHONE + 1 && pick.row[R_KITS].kind == 'K' && pick.row[R_CARD].kind == 'm' && pick.row[R_PHONE].kind == 'p');
    { static pix cv[W * H]; ui_render(cv, 30); }
    w.a = -13 * 10; ui_event(&w, 40); w.a = -13; assert(pick.cur == R_PHONE);
    centre_on_row(R_CARD, 50); ui_tick(51); assert(pick.menu == PM_DIR && pick.n == 3 && pick.row[0].kind == 'd' && !strcmp(pick.row[0].name, "Music") && !strcmp(pick.row[1].name, "sub") && pick.row[2].kind == 'f');
    centre_on_row(0, 52); ui_tick(53); assert(!strcmp(strrchr(pick.dir, '/'), "/Music") && pick.n == 0);
    press(KEY_RETURN, 54); ui_tick(55); assert(pick.open && pick.n == 3);
    pick.cur = 2; press(KEY_CENTRE, 56); assert(pick.menu == PM_ASK && pick.n == ASK_ROWS + REGIONS && pick.row[0].kind == 'r' && !strcmp(pick.row[0].name, "Use on this sound") && pick.row[1].kind == 'h' && pick.row[2].kind == 'c' && pick.row[3].kind == 'B' && !strcmp(pick.row[4].name, "Use from 0:20"));
    press(KEY_RETURN, 57); ui_tick(58); assert(pick.menu == PM_DIR && pick.cur == 2);
    /* the file's start points: the first row is 0:00, the row after the slices is the next */
    pick_ask(); centre_on_row(4, 59); assert(ui.job == J_PAD && ui.job_from == 1 && !pick.open); ui.job = 0; ui.msg_until = 0; pick.open = 1; pick.menu = PM_DIR; pick_rescan(PM_DIR, 2); ui_tick(60);
    pick_ask(); centre_on_row(0, 61); assert(ui.job == J_PAD && !ui.job_from); ui.job = 0; ui.msg_until = 0; pick.open = 1; pick_rescan(PM_DIR, 2); ui_tick(62);
    /* a tap below the last row picks nothing; a stray file in kits/ is no kit; Back at the top closes */
    { int was = pick.n, cur = pick.cur; pick.n = 2; tap_at(40, BODY_Y + 5 * ROW_H, 63); assert(pick.cur == cur && pick.menu == PM_DIR); pick.n = was;
      snprintf(pth, sizeof pth, "%s/kits/stray.wav", root); assert(wav_write(pth, w4, 1600, 1)); pick_back(); assert(pick.menu == PM_ROOT && pick.cur == R_CARD); centre_on_row(R_KITS, 64); ui_tick(65); assert(pick.menu == PM_KITS && pick.n == 4 && !kit_choose("stray.wav")); unlink(pth);
      press(KEY_RETURN, 66); assert(pick.menu == PM_ROOT && pick.cur == R_KITS); press(KEY_RETURN, 67); assert(!pick.open); }
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
        sounds[2].p[P_START] = 40; snprintf(pth, sizeof pth, "%s/media/it's a $test.wav", root); assert(load_chop(pth, 0) == 1);
        assert(sounds[0].smp.n == 100 && sounds[15].smp.n == 100 && sounds[1].smp.pcm[0] == 700 && !strcmp(sounds[15].smp.name, "Slice 16") && sounds[2].p[P_START] == 0);
        /* they come back after the kit, under their names, and a kit change forgets them */
        strcpy(kit_name, "default"); snprintf(pth, sizeof pth, "%s", project_path()); assert(project_save(pth));
        kit_open("default"); assert(!strcmp(sounds[0].smp.name, "Kick")); memset(pad_names, 0, sizeof pad_names); assert(project_load(pth)); pads_restore(pth);
        assert(sounds[3].smp.n == 100 && !strcmp(sounds[3].smp.name, "Slice 4") && sounds[3].smp.loaded);
        pads_forget(pth); snprintf(pth, sizeof pth, "%s/projects/1/pad03.wav", root); assert(access(pth, F_OK));
        snprintf(pth, sizeof pth, "%s", project_path()); strcat(pth, "/project.txt"); unlink(pth);
    }
    /* Effects: a held pad runs its effect, lifted it stops; the wheel turns the page and stops at the last */
    seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.mode = M_PADS; ui.pads = PADS_FX;
    event_t f0 = tap_of(cell_x(1) + 5, cell_y(1) + 5, 'd');
    ui_event(&f0, 10); assert(fx_sounding() == fx_page[0][1]); f0.what = 'u'; ui_event(&f0, 20); assert(fx_sounding() == -1);
    ui_event(&w, 90); ui_event(&w, 91); assert(ui.fx_page == FX_PAGES - 1); w.a = 13; ui_event(&w, 95); w.a = -13; assert(!ui.fx_page);
    /* with Record on it is written quantised, played back, undone, and Return with nothing to undo clears the beat's effects */
    seq_play(1); seq_rec(1); seq.pos = 2 * 5512 + 100; f0.what = 'd'; ui_event(&f0, 100); seq.pos = 6 * 5512 + 50; f0.what = 'u'; ui_event(&f0, 110);
    assert(seq.pat[0].nev == 1 && seq.pat[0].ev[0].step == 2 && seq.pat[0].ev[0].len == 4 && seq.pat[0].ev[0].fx == fx_page[0][1] && fx_sounding() == -1);
    seq.pos = 3 * 5512; seq_block(); assert(fx_sounding() == fx_page[0][1]); seq.pos = 7 * 5512; seq_block(); assert(fx_sounding() == -1);
    seq_undo(); assert(seq.pat[0].nev == 0 && seq.ntake == 0);
    seq.pat[0].nev = 3; press(KEY_RETURN, 200); assert(seq.pat[0].nev == 0);
    seq_play(0); seq_rec(0); assert(fx_sounding() == -1);
    /* export: the bar, or both of a chain, to a numbered file; the transport comes back as it was */
    { seq_init(); engine_init(); for (int p = 0; p < NPAD; ++p) if (p != 2) sound_swap(p, 0, 0, "", 0);
      sound_swap(2, calloc(1600, 2), 1600, "A", 0);
      for (int i = 0; i < 1600; ++i) sounds[2].smp.pcm[i] = (short)(i & 8 ? 9000 : -9000);
      seq.pat[0].s[2][0].on = 1; seq.pat[1].s[2][8].on = 1; seq.cur = 1; engine_gain(1000); engine_sweep(50);
      char nm[32]; assert(export_pattern(nm, sizeof nm) && !strcmp(nm, "1-1.wav") && seq.cur == 1 && !seq.playing && !engine_is_offline() && eg.gain == 1000 && eg.sweep == 50);
      engine_gain(65536); engine_sweep(0); snprintf(pth, sizeof pth, "%s/media/Music/Groovebox/1-1.wav", root); short *x; int xn = wav_read(pth, &x);
      assert(xn == seq_step_frames() * BAR); { int q = 0; for (int i = 0; i < xn; ++i) q |= x[i]; assert(q); } free(x);
      seq.song[0] = 0, seq.song[1] = 1, seq.nsong = 2; assert(export_pattern(nm, sizeof nm) && !strcmp(nm, "1-2.wav") && seq.cur == 1);
      snprintf(pth, sizeof pth, "%s/media/Music/Groovebox/1-2.wav", root); xn = wav_read(pth, &x); assert(xn == 2 * seq_step_frames() * BAR); free(x); }
    /* Save as WAV asks for one, run off the lock once the hint line has said so */
    seq_init(); engine_init(); ui_init(); ui.mode = M_SONG; tap_left(5, 3, 10); assert(ui.job == J_EXPORT && !ui.job_shown);
    ui_tick(11); assert(ui.job == J_EXPORT); { static pix cv[W * H]; ui_render(cv, 12); } assert(ui.job_shown); ui_tick(13); assert(!ui.job && !strncmp(ui.msg, "Saved Music/Groovebox/1-3", 25));

    /* ---- the sequencer's depth ---- */
    int bar = seq_step_frames() * BAR;
    /* a track's rate: 1/32 1/16T 1/16 1/8T 1/8 1/4, every step on */
    { static const int want[] = { 32, 24, 16, 12, 8, 4 };
      for (int rate = RATE_MIN; rate <= RATE_MAX; ++rate) {
          clear_pads(); seq_init(); engine_init(); sound_swap(0, tone_pcm(200), 200, "T", 0);
          seq.pat[0].rate[0] = (signed char)rate, seq.pat[0].len[0] = NSTEP;
          for (int s = 0; s < NSTEP; ++s) seq.pat[0].s[0][s].on = 1;
          seq_play(1); assert(hits_in(0, bar) == want[rate - RATE_MIN]);
      } }
    /* 64 steps: step 21 sounds in the second bar only, and a cue waits for all four */
    clear_pads(); seq_init(); engine_init(); sound_swap(5, tone_pcm(200), 200, "T", 0);
    seq.pat[0].len[5] = 64, seq.pat[0].s[5][20].on = 1; assert(seq_bars(&seq.pat[0]) == 4); seq_play(1); seq_cue(2);
    assert(hits_in(5, bar) == 0 && hits_in(5, bar) == 1 && seq.cur == 0 && seq.cue == 2);
    hits_in(5, bar * 2); assert(seq.cur == 2 && seq.cue == -1);
    /* a five-step track drifts against the bar: steps 1, 6, 11, 16, then 5, 10, 15 */
    seq_init(); seq.pat[0].len[5] = 5, seq.pat[0].s[5][0].on = 1; seq_play(1);
    assert(hits_in(5, bar) == 4 && hits_in(5, bar) == 3);
    /* a ratchet divides its step; a nudge moves it without losing it */
    seq_init(); seq.pat[0].s[5][2].on = 1, seq.pat[0].s[5][2].ratchet = 4, seq.pat[0].s[5][8].on = 1, seq.pat[0].s[5][8].nudge = 64;
    seq_play(1); assert(hits_in(5, bar) == 5);
    /* chance: about half at 50, the same answer for a step within its bar, always at 0 */
    { int on = 0; seq_init();
      for (unsigned b = 0; b < 2000; ++b) { seq.bar = b; int a = chance_ok(3, 7, 50); assert(a == chance_ok(3, 7, 50)); on += a; assert(chance_ok(3, 7, 0)); }
      assert(on > 800 && on < 1200); }
    /* the song: patterns in turn from the current one, in the export's length, popped back to none */
    seq_init(); seq_song_add(2); seq_song_add(5); assert(seq.nsong == 3 && !seq.song[0] && seq.song[1] == 2 && seq.song[2] == 5 && seq_song_frames() == 3 * bar);
    seq_play(1); hits_in(5, bar); assert(seq.cur == 2); hits_in(5, bar); assert(seq.cur == 5); hits_in(5, bar); assert(!seq.cur);
    seq_play(0); seq.cur = 5; seq_play(1); assert(seq.song_at == 2 && seq.cur == 5); seq_play(0);
    seq_song_pop(); assert(seq.nsong == 2); seq_song_pop(); assert(seq.nsong == 1); seq_song_pop(); assert(!seq.nsong);
    { seq_init(); seq_song_add(0); assert(seq.nsong == 1); seq_play(1); assert(!seq.cur); seq_play(0); } /* one entry is no song yet */
    /* a transient chop finds the hits: eight of them, inside a window, in order, the rest halved to fill the pads */
    { int n = FS * 4, at[NPAD + 1], c; short *p = calloc((size_t)n, sizeof *p); static const int hit[8] = { 17640, 35280, 52920, 70560, 88200, 105840, 123480, 141120 };
      for (int h = 0; h < 8; ++h) for (int i = 0; i < 4000; ++i) p[hit[h] + i] = (short)((i & 4 ? 1 : -1) * (24000 - 6 * i));
      c = slice_points(p, n, at); assert(c == NPAD && !at[0]);
      for (int k = 1; k < c; ++k) assert(at[k] > at[k - 1]);
      for (int h = 0; h < 8; ++h) { int near = 0; for (int k = 0; k < c; ++k) near |= abs(at[k] - hit[h]) <= 2 * HOP; assert(near); }
      free(p); }

    /* ---- what a pad can be ---- */
    /* every preset of every engine sounds and stays bounded; the release ends it */
    { clear_pads(); seq_init(); engine_init();
      for (int en = 0; en < SYNTH_ENGINES; ++en) {
          assert(synth_presets(en) > 0 && synth_engine(en)[0]);
          for (int pr = 0; pr < synth_presets(en); ++pr) {
              source_synth(1, en, pr); assert(sounds[1].src == SRC_SYNTH && sounds[1].sub == en && sounds[1].preset == pr && !sounds[1].smp.pcm && sounds[1].smp.name[0]);
              engine_note(1, UNSET, 0, UNSET, UNSET, 0, -1); assert(voices[1].on && voices[1].gate == -1);
              if (!louder(900, 40)) { fprintf(stderr, "silent: %s %s\n", synth_engine(en), synth_preset_name(en, pr)); assert(0); }
              engine_release(1); assert(voices[1].gate == 0);
              for (int b = 0; b < 30000 && voices[1].on; ++b) { int a[CTL * 2]; engine_block(a); }
              assert(!voices[1].on);
          }
      } }
    /* the live kits: all sixteen sounds strike and end */
    for (int src = SRC_909; src <= SRC_808; ++src) {
        clear_pads(); engine_init(); source_kit(src);
        for (int p = 0; p < NPAD; ++p) {
            assert(sounds[p].src == src && sounds[p].smp.name[0]);
            engine_trigger(p, UNSET, UNSET, UNSET, UNSET); assert(voices[p].on);
            if (!louder(40, 30)) { fprintf(stderr, "silent: %d %s\n", src, sounds[p].smp.name); assert(0); }
            for (int b = 0; b < 16000 && voices[p].on; ++b) { int a[CTL * 2]; engine_block(a); }
            assert(!voices[p].on);
        }
    }
    /* the 303: a held note speaks until let go, a slide keeps its one envelope */
    clear_pads(); engine_init(); source_assign(2, SRC_303, 0, 0, 0, "303"); assert(sounds[2].src == SRC_303);
    engine_note(2, UNSET, 0, UNSET, UNSET, NOTE_ACCENT, -1); assert(louder(400, 200) && voices[2].on);
    engine_release(2); for (int b = 0; b < 16000 && voices[2].on; ++b) { int a[CTL * 2]; engine_block(a); } assert(!voices[2].on);
    engine_note(2, UNSET, 0, UNSET, UNSET, NOTE_SLIDE, 4000); { unsigned born = voices[2].born; louder(10, 0); assert(voices[2].slide); engine_note(2, UNSET, 7, UNSET, UNSET, 0, 4000); assert(voices[2].born == born); }
    /* a break: the built-in loop is on the pad, plays with the transport only, and its track started filled */
    clear_pads(); seq_init(); engine_init(); assert(load_break_builtin(3, 0) && sounds[3].src == SRC_BREAK && seq.pat[0].s[3][0].on && sounds[3].e[14] == 2);
    seq_play(1); { int heard = 0, a[CTL * 2]; for (int f = 0; f < bar; f += CTL) { seq_block(); engine_block(a); for (int i = 0; i < CTL * 2; ++i) heard |= abs(a[i]) > 200; } assert(heard); }
    seq_play(0); { int a[CTL * 2]; for (int b = 0; b < 400; ++b) engine_block(a); }
    assert(x0x_break_guess(FS * 4) == 2 && x0x_break_guess(FS * 2) == 1);
    /* it survives the machine going: the sample goes with it (ASan), and a sound assigned over it */
    source_synth(3, 0, 0); assert(sounds[3].src == SRC_SYNTH && !sounds[3].smp.pcm);
    /* the bus: on with X0X's sends, every tile named, the mix bounded */
    { engine_init(); clear_pads(); tile_t t; x0x_bus_set(0, 1); assert(x0x_bus_sends());
      for (int i = 0; i < BUS_TILES; ++i) assert(x0x_bus_tile(i, &t) && t.name);
      assert(!x0x_bus_tile(BUS_TILES, &t));
      sound_swap(1, tone_pcm(8000), 8000, "T", 0); sounds[1].p[P_LEVEL] = 127; sounds[1].p[P_REVERB] = 127; sounds[1].p[P_ECHO] = 127; engine_trigger(1, UNSET, UNSET, UNSET, UNSET);
      assert(louder(300, 300)); x0x_bus_set(0, 0); }
    /* TB-3PO writes a line into the track, in key; undo takes it back */
    { clear_pads(); seq_init(); engine_init(); source_assign(2, SRC_303, 0, 0, 0, "303"); seq.key = 0, seq.scale = 3;
      source_line(2, 0, 12345); int on = 0; for (int i = 0; i < BAR; ++i) { const step_t *s = &seq.pat[0].s[2][i]; if (!s->on) continue; ++on; int m = ((s->pitch % 12) + 12) % 12; assert(m == 0 || m == 3 || m == 5 || m == 7 || m == 10); }
      assert(on > 0); seq_undo(); for (int i = 0; i < BAR; ++i) assert(!seq.pat[0].s[2][i].on && seq.pat[0].s[2][i].pitch == UNSET);
      source_line(2, 0, 1); step_t before[NSTEP]; memcpy(before, seq.pat[0].s[2], sizeof before); source_line(2, 1, 2); assert(memcmp(before, seq.pat[0].s[2], sizeof before)); }
    /* a project keeps all of it, and a card file's nonsense is thrown out or brought into range */
    { clear_pads(); seq_init(); engine_init(); memset(pad_src, 0, sizeof pad_src);
      seq.key = 5, seq.scale = 3, seq.nsong = 3, seq.song[0] = 1, seq.song[1] = 2, seq.song[2] = 1;
      seq.pat[1].rate[2] = 3, seq.pat[1].len[2] = 64;
      step_t *s = &seq.pat[1].s[2][40]; s->on = 1, s->ratchet = 3, s->chance = 50, s->flags = NOTE_ACCENT | NOTE_SLIDE;
      source_synth(4, 2, 1); sounds[4].e[0] = 7; source_assign(5, SRC_909, kit_sub(SRC_909, 1), 0, 0, "Snare"); source_assign(6, SRC_303, 0, 0, 0, "303"); x0x_bus_set(1, 5);
      char pd[] = "/tmp/gbp3XXXXXX", pf[300]; assert(mkdtemp(pd)); assert(project_save(pd));
      clear_pads(); seq_init(); engine_init(); assert(project_load(pd)); pads_restore(pd);
      assert(seq.key == 5 && seq.scale == 3 && seq.nsong == 3 && seq.song[2] == 1 && seq.pat[1].rate[2] == 3 && seq.pat[1].len[2] == 64);
      s = &seq.pat[1].s[2][40]; assert(s->on && s->ratchet == 3 && s->chance == 50 && s->flags == (NOTE_ACCENT | NOTE_SLIDE));
      assert(sounds[4].src == SRC_SYNTH && sounds[4].sub == 2 && sounds[4].preset == 1 && sounds[4].e[0] == 7 && sounds[5].src == SRC_909 && sounds[5].sub == kit_sub(SRC_909, 1) && sounds[6].src == SRC_303 && x0x_bus_get(1) == 5);
      snprintf(pf, sizeof pf, "%s/project.txt", pd); FILE *t = fopen(pf, "w");
      fputs("pad1.src=99 0 0\npad2.src=1 99 0\npad3.src=1 0 9999\npad4.src=2 77 0\npad5.src=1 0 0\npad5.e=9999 -9999\nsong=99 1 2 -1\nbus=999\nscale=99\nkey=99\n", t);
      fputs("p1.t1=x len=999 rate=99\np1.t1.s1=0 0 0 0 0 99 200 255\np99.t1=x\np1.t99.s1=1 1 1 1 1\n", t); fclose(t);
      clear_pads(); seq_init(); engine_init(); memset(pad_src, 0, sizeof pad_src); project_load(pd); pads_restore(pd);
      for (int p = 0; p < 4; ++p) assert(sounds[p].src == SRC_SAMPLE);
      { tile_t tt; assert(sounds[4].src == SRC_SYNTH && synth_tile(4, 0, 0, &tt) && sounds[4].e[0] == tt.max && sounds[4].e[1] == tt.min); }
      assert(seq.nsong == 2 && seq.song[0] == 1 && !seq.scale && seq.key == 24 && seq.pat[0].len[0] == NSTEP && seq.pat[0].rate[0] == RATE_MAX && seq.pat[0].s[0][0].ratchet == 8 && !seq.pat[0].s[0][0].chance && seq.pat[0].s[0][0].flags == 3);
      unlink(pf); rmdir(pd); memset(pad_src, 0, sizeof pad_src); }

    /* ---- the panel ---- */
    /* every control clears the glass */
    { for (int i = 0; i < 3; ++i) on_glass(pill_x(i), TOP_Y, PILL_W, BAND_H);
      for (int m = 0; m < M_N; ++m) on_glass(tab_x(m), TABS_Y, TAB_W, BAND_H);
      on_glass(EDGE, TABS_Y, W - 2 * EDGE, BAND_H); on_glass(SIDE, HINT_Y, BODY_W, HINT_H);
      for (int i = 0; i < 2; ++i) on_glass(ARROW_X + i * (ARROW_W + 4), HINT_Y + 1, ARROW_W, BAND_H);
      for (int i = 0; i < NPAD; ++i) on_glass(cell_x(i), cell_y(i), CELL_W, CELL_H);
      for (int i = 0; i < BOXES; ++i) on_glass(box_x(i), box_y(i), BOX_W, BOX_H);
      for (int c = 4; c <= 5; ++c) for (int i = 0; i < c; ++i) on_glass(SIDE, left_y(c, i), LEFT_W, left_h(c));
      for (int r = 0; r < LIST_ROWS; ++r) on_glass(SIDE, BODY_Y + r * LIST_ROW, LEFT_W, LIST_ROW - 2);
      for (int r = 0; r < PICK_ROWS; ++r) on_glass(SIDE, BODY_Y + r * ROW_H, BODY_W, ROW_H - 2); }
    /* a project with no steps starts with a beat; one with any keeps what it has */
    seq_init(); seq_starter(); assert(seq.pat[0].s[0][0].on && seq.pat[0].s[0][12].on && seq.pat[0].s[1][4].on && !seq.pat[0].s[1][0].on && seq.pat[0].s[2][2].on);
    seq_init(); seq.pat[5].s[9][9].on = 1; seq_starter(); assert(!seq.pat[0].s[0][0].on);
    /* Notes: a pad is the scale's note from the key, on the selected sound, held until lifted */
    clear_pads(); seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick);
    sound_swap(0, tone_pcm(200), 200, "T", 0); seq.key = 2, seq.scale = 3; ui.mode = M_PADS; ui.pads = PADS_NOTES; tap_pad(2, 10);
    assert(voices[0].inc == (int)(65536 * exp2f((2 + 5) / 12.0f)) && seq_scale_note(5) == 12);
    { char nm[8]; note_name(-3, nm, sizeof nm); assert(!strcmp(nm, "A2")); note_name(0, nm, sizeof nm); assert(!strcmp(nm, "C3")); note_name(13, nm, sizeof nm); assert(!strcmp(nm, "C#4")); }
    source_synth(1, 0, 0); ui.sound = 1; { event_t t = tap_of(cell_x(0) + 5, cell_y(0) + 5, 'd'); ui_event(&t, 20); assert(voices[1].on && voices[1].gate == -1); t.what = 'u'; ui_event(&t, 30); assert(!voices[1].gate); }
    /* with Record on and the beat running the note lands on its step, its pitch kept */
    engine_stop(1); seq_play(1); seq_rec(1); seq.pos = 5512 + 100; ui.sound = 0; tap_pad(4, 40); assert(seq.pat[0].s[0][1].on && seq.pat[0].s[0][1].pitch == 2 + seq_scale_note(4)); seq_play(0); seq_rec(0);
    /* the wheel changes the sound the notes are on */
    ui.sound = 0; ui_event(&w, 50); assert(ui.sound == 1);
    /* on a break the pads are its slices and tricks: the eleventh is the last that means anything (ASan) */
    assert(load_break_builtin(3, 0)); ui.sound = 3; tap_pad(0, 80); tap_pad(10, 90); tap_pad(15, 100);
    /* Track options: Steps makes pages, the keys and the arrows turn them, a short track keeps its cells */
    clear_pads(); seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.sound = 1;
    tap_left(5, 4, 10); assert(ui.over == O_TRACK); tap_box(T_STEPS, 12);
    for (int i = 0; i < 16; ++i) ui_event(&w, 13);
    assert(seq.pat[0].len[1] == 32 && track_pages() == 2);
    press(KEY_RETURN, 14); assert(!ui.over);
    press(KEY_NEXT, 16); assert(ui.page == 1 && !seq.cur && seq.cue == -1); tap_pad(0, 18); assert(seq.pat[0].s[1][16].on);
    press(KEY_NEXT, 19); assert(ui.page == 1); press(KEY_PREV, 20); assert(!ui.page);
    tap_at(ARROW_X + ARROW_W + 10, HINT_Y + 5, 21); assert(ui.page == 1); tap_at(ARROW_X + 5, HINT_Y + 5, 22); assert(!ui.page);
    ui.page = 1; seq.pat[0].len[1] = 12; { static pix cv[W * H]; ui_render(cv, 23); } assert(!ui.page);
    tap_pad(14, 25); assert(!seq.pat[0].s[1][14].on); tap_pad(11, 26); assert(seq.pat[0].s[1][11].on);
    tap_left(5, 4, 27); tap_box(T_SPEED, 28); ui_event(&w, 29); assert(seq.pat[0].rate[1] == 1);
    /* Clear steps, and Undo brings them back; no tune boxes on a plain sample */
    { int id[32]; assert(boxes(id) == 4); tap_box(T_CLEAR, 30); assert(!seq.pat[0].s[1][11].on); tap_box(T_UNDO, 31); assert(seq.pat[0].s[1][11].on); }
    press(KEY_RETURN, 32); { static pix cv[W * H]; seq_play(1); ui.page = 0; ui_render(cv, 95); seq_play(0); }
    /* the Sound tab: a box is tapped then turned; a sample's lead with Volume, a synth's with its preset; no action among them */
    clear_pads(); seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); sound_swap(0, tone_pcm(200), 200, "T", 0); tap_tab(M_SOUND, 10); assert(ui.mode == M_SOUND);
    tap_box(1, 12); ui_event(&w, 13); assert(sounds[0].p[P_PITCH] == 1 && sounds[0].p[P_LEVEL] == 100); tap_left(5, 4, 14); assert(ui.box_page == 1 && ui.box == BOXES); tap_box(4, 15); ui_event(&w, 16); assert(sounds[0].p[P_REVERSE] == 1); tap_left(5, 4, 17); assert(!ui.box_page);
    source_synth(1, 0, 0); press(KEY_NEXT, 20); assert(ui.sound == 1);
    { int id[32], c = boxes(id); tile_t t; assert(id[0] == MACH + SY_PRESET); for (int i = 0; i < c; ++i) if (id[i] >= MACH) { assert(source_tile(1, id[i] - MACH, &t) && !t.action); } }
    tap_box(0, 21); { char before[32]; snprintf(before, sizeof before, "%s", sounds[1].smp.name); ui_event(&w, 30); assert(sounds[1].preset == 1 && strcmp(before, sounds[1].smp.name)); }
    /* a tune: Make writes, Undo takes it back, Return from the page isn't an undo */
    tap_tab(M_BEAT, 40); tap_left(5, 4, 41); { int id[32]; assert(boxes(id) == 7 && id[4] == T_MAKE); }
    { int on = 0; tap_box(T_MAKE, 42); for (int i = 0; i < BAR; ++i) on += seq.pat[0].s[1][i].on; assert(on); press(KEY_RETURN, 43); assert(!ui.over); on = 0; for (int i = 0; i < BAR; ++i) on += seq.pat[0].s[1][i].on; assert(on);
      press(KEY_RETURN, 44); on = 0; for (int i = 0; i < BAR; ++i) on += seq.pat[0].s[1][i].on; assert(!on && !strcmp(ui.msg, "Undone")); }
    all_screens(70);
    /* the settings: swing by fives, key, scale, and the bus's boxes */
    ui.mode = M_SONG; tap_left(5, 4, 80); assert(ui.over == O_SET); tap_box(X_SWING, 81); ui_event(&w, 82); assert(seq.swing == 5); tap_box(X_KEY, 83); ui_event(&w, 84); assert(seq.key == -2); tap_box(X_SCALE, 85); ui_event(&w, 86); assert(seq.scale == 1);
    tap_box(3, 87); ui_event(&w, 88); assert(x0x_bus_get(0) == 1); x0x_bus_set(0, 0); tap_left(5, 4, 89); assert(ui.box_page == 1); press(KEY_RETURN, 90); assert(!ui.over);
    /* the picker's sources: an engine and a preset, back a step, a drum, the bass, a loop */
    clear_pads(); seq_init(); engine_init(); ui_init(); memset(&pick, 0, sizeof pick); ui.sound = 4; pick_open();
    centre_on_row(R_SYNTHS, 20); assert(pick.menu == PM_ENGINE && pick.n == SYNTH_ENGINES); centre_on_row(3, 22); assert(pick.menu == PM_PRESET && pick.engine == 3 && pick.n == synth_presets(3));
    press(KEY_RETURN, 24); assert(pick.menu == PM_ENGINE && pick.cur == 3); press(KEY_CENTRE, 26); centre_on_row(1, 28); assert(pick.open && ui.job == 'y'); /* it stays open to try the next */
    run_job(30); assert(sounds[4].src == SRC_SYNTH && sounds[4].sub == 3 && sounds[4].preset == 1 && !strcmp(ui.msg, "Loaded")); pick.open = 0;
    pick_open(); centre_on_row(R_MACHINE, 41); assert(pick.menu == PM_MACHINE); centre_on_row(0, 42); assert(pick.menu == PM_VOICE && pick.n == NPAD && !strcmp(pick.row[12].name, "Kick Tight")); centre_on_row(12, 43); run_job(45); assert(sounds[4].src == SRC_909 && sounds[4].sub == kit_sub(SRC_909, 0) && !strcmp(sounds[4].smp.name, "Kick Tight"));
    press(KEY_RETURN, 46); assert(pick.menu == PM_MACHINE && !pick.cur); press(KEY_RETURN, 47); assert(pick.menu == PM_ROOT && pick.cur == R_MACHINE); pick.open = 0;
    pick_open(); centre_on_row(R_BASS, 51); assert(!pick.open); run_job(53); assert(sounds[4].src == SRC_303);
    pick_open(); centre_on_row(R_LOOPS, 61); assert(pick.menu == PM_BREAK && pick.n == 2); press(KEY_RETURN, 62); assert(pick.menu == PM_ROOT && pick.cur == R_LOOPS);
    centre_on_row(R_LOOPS, 64); centre_on_row(1, 65); run_job(67); assert(sounds[4].src == SRC_BREAK && !strcmp(sounds[4].smp.name, "Break Fill"));
    sound_swap(7, tone_pcm(FS), FS, "Loop", 0); ui.sound = 7; pick_open(); centre_on_row(R_LOOPS, 71); assert(pick.n == 3 && pick.row[2].kind == 't'); centre_on_row(2, 73); run_job(75); assert(sounds[7].src == SRC_BREAK && sounds[7].smp.pcm);
    /* a live kit from the kit rows */
    pick_open(); centre_on_row(R_KITS, 80); ui_tick(81); assert(pick.menu == PM_KITS && pick.row[1].kind == 'k' && !strcmp(pick.row[1].name, KIT_808_LIVE)); centre_on_row(1, 82); run_job(83); assert(sounds[0].src == SRC_808 && !strcmp(kit_name, KIT_808_LIVE));
    all_screens(85);
    strcpy(kit_name, "default");
    /* the drop server: only under its code, and whatever the sender says its name is stays a plain name in Drops */
    { char o[32], req[300], got[80];
      safe_name("..%2F..%2Fevil.m4a", o, sizeof o); assert(!strcmp(o, "evil.m4a")); safe_name("a%20b.wav&x=1", o, sizeof o); assert(!strcmp(o, "a b.wav")); safe_name("%2F", o, sizeof o); assert(!strcmp(o, "memo.m4a")); safe_name("....", o, sizeof o); assert(!strcmp(o, "memo.m4a"));
      assert(drop_start(DROP_PORT) && strlen(drop_code()) == 6);
      assert(http("GET /000000x HTTP/1.1\r\n\r\n", 0) == 404 && http("GET / HTTP/1.1\r\n\r\n", 0) == 404);
      snprintf(req, sizeof req, "GET /%s/ HTTP/1.1\r\n\r\n", drop_code()); assert(http(req, 0) == 200);
      snprintf(req, sizeof req, "POST /%s/up?name=x.m4a HTTP/1.1\r\n\r\n", drop_code()); assert(http(req, 0) == 413);
      snprintf(req, sizeof req, "POST /%s/up?name=x.m4a HTTP/1.1\r\nContent-Length: %lld\r\n\r\n", drop_code(), DROP_MAX + 1); assert(http(req, 0) == 413);
      assert(!drop_take(got, sizeof got));
      snprintf(req, sizeof req, "POST /%s/up?name=..%%2F..%%2Fevil.m4a HTTP/1.1\r\nContent-Length: 5\r\n\r\n", drop_code()); assert(http(req, "hello") == 200);
      assert(drop_take(got, sizeof got) && strstr(got, "/Music/Groovebox/Drops/evil.m4a") && !drop_take(got, sizeof got));
      FILE *t = fopen(got, "r"); char body[8] = ""; assert(t && fread(body, 1, 7, t) == 5 && !strcmp(body, "hello")); fclose(t);
      snprintf(pth, sizeof pth, "%s.new", got); assert(access(pth, F_OK));
      snprintf(req, sizeof req, "POST /%s/up?name=evil.m4a HTTP/1.1\r\nContent-Length: 2\r\n\r\n", drop_code()); assert(http(req, "hi") == 200 && drop_take(got, sizeof got) && strstr(got, "evil-1.m4a"));
      drop_stop(); }
    /* the phone screen: its row starts the server, an arrival goes to the file's choices, leaving stops it */
    ui.sound = 2; ui.mode = M_SOUND; pick_open(); centre_on_row(R_PHONE, 101); assert(pick.menu == PM_DROP); { static pix cv[W * H]; ui_tick(102); assert(pick.drop_on); ui_render(cv, 103); }
    { char req[300]; snprintf(req, sizeof req, "POST /%s/up?name=memo.wav HTTP/1.1\r\nContent-Length: 2\r\n\r\n", drop_code()); assert(http(req, "hi") == 200); ui_tick(104); assert(pick.menu == PM_ASK && strstr(pick.file, "memo.wav")); ui_tick(105); assert(!pick.drop_on); press(KEY_RETURN, 106); ui_tick(107); assert(pick.menu == PM_DIR); press(KEY_RETURN, 107); assert(pick.menu == PM_ROOT); }
    centre_on_row(R_PHONE, 108); ui_tick(109); assert(pick.drop_on); press(KEY_RETURN, 110); ui_tick(111); assert(!pick.drop_on && pick.menu == PM_ROOT);
    /* Back leaves it too, and an idle minute on it is no reason to quit */
    centre_on_row(R_PHONE, 112); ui_tick(113); assert(pick.drop_on); ui_tick(113 + IDLE_QUIT_MS + 1); assert(!ui.quit); tap_at(100, TABS_Y + 5, 114 + IDLE_QUIT_MS); ui_tick(115 + IDLE_QUIT_MS); assert(!pick.drop_on && pick.menu == PM_ROOT); pick.open = 0;
    clear_pads();
    { char cmd[64]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); assert(!system(cmd)); }
    for (int p = 0; p < NPAD; ++p) sound_swap(p, 0, 0, "", 0);
    puts("check: ok");
    return 0;
}

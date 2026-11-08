/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* From SLOOP's firmware/src/fx.c (bac9c05): the tempo delay and the reverb. The chorus bus is taken
 * out, ECHO THROW and the section attributes too; with no chorus sent the output is SLOOP's, bit for bit. */
#define DLY_LEN 65536u           /* 1.49 s: 1/4 down to 41 BPM, 1/8 dotted down to 31, 1/4 dotted down to 61 (clamped below) */
static int16_t dly_buf[DLY_LEN];
/* the reverb: two input diffusers, then four delay lines mixed by a Hadamard matrix (a feedback delay
 * network: every echo feeds all four, so it thickens instead of ringing like a comb), damped in the
 * loop, one line slowly modulated (no metallic tone on long tails); left and right take different lines */
#define REV_MOD 12               /* samples the modulated line moves (+-) */
static const uint16_t REV_LINE[4] = {1559, 1931, 2389, 2791};   /* 35..63 ms, coprime */
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2];
static int16_t rev_ap[556 + 441];
static struct {
    uint32_t dly_w, rev_ph;
    int32_t dly_lp;
    uint16_t line_i[4], ap_i[2];
    int32_t line_lp[4];
} fx;
static volatile uint8_t rev_frz;         /* punch-in FREEZE (punch.c): the reverb holds, no loss, no damping */
static int32_t rev_l[CTL], rev_r[CTL];   /* the reverb's own return of this block (FREEZE plays it) */

static uint32_t delay_samples(void)
{
    uint32_t s = dly_samples((uint32_t)song.g[G_DTIME]);
    return s < 16u ? 16u : s >= DLY_LEN ? DLY_LEN - 1u : s;
}

/* process the two buses for one block; sends in, wet out (stereo). The reverb's LFO is computed per
 * block and ramped: no sine per sample */
#define HALF0(v) ((v) >= 0 ? (v) >> 1 : -(-(v) >> 1))   /* v / 2 towards zero */
#define CTL_LOG2 5
static void fx_buses(const int32_t *dly_in, const int32_t *rev_in, int32_t *wet_l, int32_t *wet_r, uint32_t n)
{
    uint32_t i, k, dl = delay_samples();
    int32_t fb = song.g[G_DFDBK] * 230, col = 2000 + song.g[G_DCOLOR] * 240;
    int32_t dmix = song.g[G_DMIX] * 258;
    int32_t g = 17000 + song.g[G_RSIZE] * 104, lpk = 32767 - song.g[G_RDAMP] * 200;   /* loop gain (RT60 ~0.4..4 s), damping */
    int32_t frnd = rev_frz ? 128 : 0;                  /* FREEZE: what is in the lines stays there (below: no
                                                         * loss, rounded to nearest; 0: as always, bit for bit) */
    int32_t ma, mb;
    const uint32_t L0 = REV_LINE[0] + REV_MOD + 2u, B1 = L0, B2 = B1 + REV_LINE[1], B3 = B2 + REV_LINE[2];
    {   /* line 0's extra length, at both ends of the block */
        int32_t m0 = osc_sine(fx.rev_ph), m1;
        fx.rev_ph += LFO_INC[30];
        m1 = osc_sine(fx.rev_ph);
        ma = (REV_MOD << 8) + ((m0 * REV_MOD) >> 7), mb = (REV_MOD << 8) + ((m1 * REV_MOD) >> 7);
    }
    for (i = 0; i < n; i++) {
        int32_t yl, yr, x, a, o0, o1, o2, o3;
        /* delay with a low-passed feedback (in the middle) */
        x = dly_buf[(fx.dly_w - dl) & (DLY_LEN - 1u)];
        fx.dly_lp += mulq15(x - fx.dly_lp, col);
        dly_buf[fx.dly_w & (DLY_LEN - 1u)] =
            (int16_t)clamp((dly_in[i] >> 1) + mulq15(fx.dly_lp, fb), -32768, 32767);
        fx.dly_w++;
        x = mulq15(x << 1, dmix);
        yl = x;
        yr = x;
        /* reverb: two diffusers, then the four lines */
        a = mulq15(rev_in[i], 13000);
        {
            int16_t *c = rev_ap;
            for (k = 0; k < 2u; k++) {
                int32_t b = c[fx.ap_i[k]], v = a + (b >> 1);
                c[fx.ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
                a = b - (v >> 1);
                if (++fx.ap_i[k] >= REV_AP[k])
                    fx.ap_i[k] = 0;
                c += REV_AP[k];
            }
        }
        {
            int16_t *c = rev_line;
            int32_t s0, s1, d0, d1, r = ma + (((mb - ma) * (int32_t)i) >> CTL_LOG2);   /* (between the two: never below 0) */
            uint32_t ri = fx.line_i[0] + ((uint32_t)r >> 8), rj;
            if (ri >= L0)                                   /* (the oldest sample is at line_i: reading */
                ri -= L0;                                   /* past it shortens line 0 by 0..2 REV_MOD) */
            rj = ri + 1u >= L0 ? 0u : ri + 1u;
            o0 = c[ri] + (((c[rj] - c[ri]) * (r & 255) + frnd) >> 8);
            o1 = c[B1 + fx.line_i[1]];
            o2 = c[B2 + fx.line_i[2]];
            o3 = c[B3 + fx.line_i[3]];
            s0 = o0 + o1, d0 = o0 - o1, s1 = o2 + o3, d1 = o2 - o3;   /* Hadamard / 2: each feeds all four */
            if (!frnd) {
                fx.line_lp[0] += mulq15(((s0 + s1) >> 1) - fx.line_lp[0], lpk);
                fx.line_lp[1] += mulq15(((d0 + d1) >> 1) - fx.line_lp[1], lpk);
                fx.line_lp[2] += mulq15(((s0 - s1) >> 1) - fx.line_lp[2], lpk);
                fx.line_lp[3] += mulq15(((d0 - d1) >> 1) - fx.line_lp[3], lpk);
                c[fx.line_i[0]] = (int16_t)clamp(mulq15(fx.line_lp[0], g) + a, -32768, 32767);
                c[B1 + fx.line_i[1]] = (int16_t)clamp(mulq15(fx.line_lp[1], g) - a, -32768, 32767);
                c[B2 + fx.line_i[2]] = (int16_t)clamp(mulq15(fx.line_lp[2], g) + a, -32768, 32767);
                c[B3 + fx.line_i[3]] = (int16_t)clamp(mulq15(fx.line_lp[3], g) - a, -32768, 32767);
            } else {                                    /* FREEZE: a loop gain of exactly 1, so no rounding that
                                                         * leans one way (a DC that would grow while held): the
                                                         * halves towards zero, no damping or gain multiply */
                fx.line_lp[0] = HALF0(s0 + s1), fx.line_lp[1] = HALF0(d0 + d1);
                fx.line_lp[2] = HALF0(s0 - s1), fx.line_lp[3] = HALF0(d0 - d1);
                c[fx.line_i[0]] = (int16_t)clamp(fx.line_lp[0] + a, -32768, 32767);
                c[B1 + fx.line_i[1]] = (int16_t)clamp(fx.line_lp[1] - a, -32768, 32767);
                c[B2 + fx.line_i[2]] = (int16_t)clamp(fx.line_lp[2] + a, -32768, 32767);
                c[B3 + fx.line_i[3]] = (int16_t)clamp(fx.line_lp[3] - a, -32768, 32767);
            }
            if (++fx.line_i[0] >= L0) fx.line_i[0] = 0;
            if (++fx.line_i[1] >= REV_LINE[1]) fx.line_i[1] = 0;
            if (++fx.line_i[2] >= REV_LINE[2]) fx.line_i[2] = 0;
            if (++fx.line_i[3] >= REV_LINE[3]) fx.line_i[3] = 0;
        }
        wet_l[i] = yl + o0 + o2;
        wet_r[i] = yr + o1 - o3;
        rev_l[i] = o0 + o2;
        rev_r[i] = o1 - o3;
    }
}

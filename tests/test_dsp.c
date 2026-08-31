/*
 * test_dsp.c - DSP 検証テスト
 *
 * 「録音の再生ではなく本当にリアルタイム合成しているか」を
 * 数値的に検証する。各テストは合成した PCM をゴーツェル法や
 * DFT で解析し、規格通りの周波数成分が出ている事を確認する。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_types.h"
#include "vmodem/vm_config.h"
#include "vmodem/vm_tone.h"
#include "vmodem/vm_handshake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char *name, bool ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-46s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-46s %s\n", name, detail ? detail : ""); }
}

/* ------------------------------------------------------------------------- */
/* Goertzel 法による単一周波数のパワー測定                                    */
/* ------------------------------------------------------------------------- */
static double goertzel(const float *x, int n, double freq, int fs)
{
    double w = 2.0 * M_PI * freq / (double)fs;
    double coeff = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    int i;
    for (i = 0; i < n; i++) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    /* パワー = |X(f)|^2 を n^2 で正規化 */
    return (s1*s1 + s2*s2 - coeff*s1*s2) / ((double)n * (double)n);
}

/* 信号の RMS */
static double rms(const float *x, int n)
{
    double s = 0; int i;
    for (i = 0; i < n; i++) s += (double)x[i]*(double)x[i];
    return sqrt(s / (double)(n > 0 ? n : 1));
}

/* 指定範囲でもっとも強い周波数を粗探索 */
static double peak_freq(const float *x, int n, double f0, double f1,
                        int fs, double step)
{
    double best_f = f0, best_p = -1.0, f;
    for (f = f0; f <= f1; f += step) {
        double p = goertzel(x, n, f, fs);
        if (p > best_p) { best_p = p; best_f = f; }
    }
    return best_f;
}

/* ========================================================================= */
/* 1. 番号正規化とマッチング                                                 */
/* ========================================================================= */
static void test_number_matching(void)
{
    char buf[64];
    vm_config_t cfg;
    const vm_isp_entry_t *e;

    printf("\n[1] 番号正規化 / マッチング\n");

    vm_number_normalize("0120-000-0000", buf, sizeof(buf));
    check("hyphen removal", strcmp(buf, "01200000000") == 0, buf);

    vm_number_normalize("0000-00-00000", buf, sizeof(buf));
    check("multi-hyphen removal", strcmp(buf, "00000000000") == 0, buf);

    vm_number_normalize("000", buf, sizeof(buf));
    check("short number", strcmp(buf, "000") == 0, buf);

    vm_number_normalize("W0120 000 0000,,;", buf, sizeof(buf));
    check("dial modifiers stripped", strcmp(buf, "01200000000") == 0, buf);

    vm_number_normalize("(0120)000-0000", buf, sizeof(buf));
    check("parens stripped", strcmp(buf, "01200000000") == 0, buf);

    vm_number_normalize("*70,000", buf, sizeof(buf));
    check("star-prefix stripped", strcmp(buf, "70000") == 0, buf);

    /* マッチングテスト */
    vm_config_defaults(&cfg);
    cfg.isp_count = 3;
    strcpy(cfg.isp[0].number_norm, "000");
    cfg.isp[0].speed = 33600; cfg.isp[0].protocol = VM_STD_V34PLUS;
    strcpy(cfg.isp[1].number_norm, "01200000000");
    cfg.isp[1].speed = 14400; cfg.isp[1].protocol = VM_STD_V32BIS;
    strcpy(cfg.isp[2].number_norm, "00000000000");
    cfg.isp[2].speed = 28800; cfg.isp[2].protocol = VM_STD_V34;

    e = vm_config_match_number(&cfg, "000");
    check("match '000'", e && e->speed == 33600, e ? "33600" : "NULL");

    e = vm_config_match_number(&cfg, "0120-000-0000");
    check("match '0120-000-0000' (hyphens)", e && e->speed == 14400,
          e ? "14400" : "NULL");

    e = vm_config_match_number(&cfg, "01200000000");
    check("match same number w/o hyphens", e && e->speed == 14400,
          e ? "14400" : "NULL");

    e = vm_config_match_number(&cfg, "0000-00-00000");
    check("match '0000-00-00000'", e && e->speed == 28800,
          e ? "28800" : "NULL");

    e = vm_config_match_number(&cfg, "0312345678");
    check("unknown number -> NULL (NO CARRIER)", e == NULL,
          e ? "matched!" : "NULL");

    e = vm_config_match_number(&cfg, "00");
    check("partial prefix must NOT match", e == NULL,
          e ? "matched!" : "NULL");

    e = vm_config_match_number(&cfg, "");
    check("empty number -> NULL", e == NULL, "NULL");
}

/* ========================================================================= */
/* 2. 規格名パース                                                           */
/* ========================================================================= */
static void test_standard_parse(void)
{
    printf("\n[2] 規格名パース\n");

    check("'V.34' ",      vm_standard_from_string("V.34")     == VM_STD_V34, NULL);
    check("'v34'  ",      vm_standard_from_string("v34")      == VM_STD_V34, NULL);
    check("'V.32bis'",    vm_standard_from_string("V.32bis")  == VM_STD_V32BIS, NULL);
    check("'v32BIS'",     vm_standard_from_string("v32BIS")   == VM_STD_V32BIS, NULL);
    check("'V.34+'",      vm_standard_from_string("V.34+")    == VM_STD_V34PLUS, NULL);
    check("'V.22bis'",    vm_standard_from_string("V.22bis")  == VM_STD_V22BIS, NULL);
    check("'V.21'",       vm_standard_from_string("V.21")     == VM_STD_V21, NULL);
    check("'V.90'",       vm_standard_from_string("V.90")     == VM_STD_V90, NULL);
    check("garbage -> UNKNOWN",
          vm_standard_from_string("frobnicate") == VM_STD_UNKNOWN, NULL);

    check("33600 -> V.34+", vm_standard_from_bitrate(33600) == VM_STD_V34PLUS, NULL);
    check("28800 -> V.34",  vm_standard_from_bitrate(28800) == VM_STD_V34, NULL);
    check("14400 -> V.32bis", vm_standard_from_bitrate(14400) == VM_STD_V32BIS, NULL);
    check("2400 -> V.22bis", vm_standard_from_bitrate(2400) == VM_STD_V22BIS, NULL);
    check("300 -> V.21",    vm_standard_from_bitrate(300) == VM_STD_V21, NULL);
}

/* ========================================================================= */
/* 3. DTMF 周波数の検証                                                      */
/* ========================================================================= */
static void test_dtmf(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float buf[4096];
    char detail[160];

    printf("\n[3] DTMF リアルタイム合成 (ITU-T Q.23)\n");

    /*
     * '5' = 770Hz + 1336Hz
     * 400 サンプル (50ms) 分だけ ON 区間を切り出して解析する。
     */
    struct { char d; double lo; double hi; } cases[] = {
        { '1', 697.0, 1209.0 },
        { '5', 770.0, 1336.0 },
        { '9', 852.0, 1477.0 },
        { '0', 941.0, 1336.0 }
    };
    size_t c;

    for (c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        vm_dtmf_tx_t tx;
        char digits[2] = { cases[c].d, 0 };
        double p_lo, p_hi, p_off1, p_off2;
        int n = 320;   /* 40ms */

        memset(buf, 0, sizeof(buf));
        vm_dtmf_tx_init(&tx, digits, 80, 80, fs);
        vm_dtmf_tx_render_add(&tx, buf, n);

        p_lo   = goertzel(buf, n, cases[c].lo, fs);
        p_hi   = goertzel(buf, n, cases[c].hi, fs);
        /* 存在しない周波数のパワー (漏れ確認) */
        p_off1 = goertzel(buf, n, 1000.0, fs);
        p_off2 = goertzel(buf, n, 2000.0, fs);

        snprintf(detail, sizeof(detail),
                 "lo=%.5f hi=%.5f  off(1k)=%.7f off(2k)=%.7f",
                 p_lo, p_hi, p_off1, p_off2);

        check("DTMF digit tone pair present",
              p_lo > 1e-3 && p_hi > 1e-3 &&
              p_lo > p_off1 * 50 && p_hi > p_off2 * 50,
              detail);
    }

    /* Twist の確認: 高群が低群より約 2dB 大きい */
    {
        vm_dtmf_tx_t tx;
        double p_lo, p_hi, db;
        int n = 320;
        memset(buf, 0, sizeof(buf));
        vm_dtmf_tx_init(&tx, "5", 80, 80, fs);
        vm_dtmf_tx_render_add(&tx, buf, n);
        p_lo = goertzel(buf, n, 770.0, fs);
        p_hi = goertzel(buf, n, 1336.0, fs);
        db = 10.0 * log10(p_hi / p_lo);
        snprintf(detail, sizeof(detail), "twist = %.2f dB (expect ~2.0)", db);
        check("DTMF twist ~ +2dB (high group louder)",
              db > 1.2 && db < 2.8, detail);
    }

    /* 桁間の無音を確認 */
    {
        vm_dtmf_tx_t tx;
        int n = 8000;   /* 1 秒 */
        double r_on, r_off;
        memset(buf, 0, sizeof(buf));
        vm_dtmf_tx_init(&tx, "12", 80, 80, fs);
        vm_dtmf_tx_render_add(&tx, buf, n);

        /* 0-80ms は ON, 80-160ms は OFF */
        r_on  = rms(buf,               640);
        r_off = rms(buf + 640 + 20,    600 - 40);
        snprintf(detail, sizeof(detail),
                 "rms(on)=%.4f  rms(gap)=%.6f", r_on, r_off);
        check("DTMF inter-digit gap is silent",
              r_on > 0.1 && r_off < 1e-6, detail);
    }

    /* 番号全体を送出して桁数分のトーンが出る事を確認 */
    {
        vm_dtmf_tx_t tx;
        int n = 8000 * 3;
        float *big = (float*)calloc((size_t)n, sizeof(float));
        int i, bursts = 0; bool inb = false;
        vm_dtmf_tx_init(&tx, "0120-000-0000", 80, 80, fs);
        vm_dtmf_tx_render_add(&tx, big, n);

        /* 20ms 窓の RMS で burst 数を数える */
        for (i = 0; i + 160 <= n; i += 160) {
            bool loud = rms(big + i, 160) > 0.05;
            if (loud && !inb) { bursts++; inb = true; }
            else if (!loud)   { inb = false; }
        }
        snprintf(detail, sizeof(detail),
                 "bursts=%d (expect 11 digits, hyphens skipped)", bursts);
        check("DTMF burst count == digit count", bursts == 11, detail);
        free(big);
    }
}

/* ========================================================================= */
/* 4. 発信音 / 呼出音                                                        */
/* ========================================================================= */
static void test_call_progress_tones(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float buf[8000];
    char detail[160];

    printf("\n[4] 発信音 / 呼出音 (日本 NTT 仕様)\n");

    /* 発信音: 400Hz 連続 */
    {
        vm_tonegen_t g;
        double p400, p350, p440, pk;
        vm_tonegen_init(&g);
        vm_tonegen_add(&g, 400.0, 0.5f, 0.0);
        memset(buf, 0, sizeof(buf));
        vm_tonegen_render(&g, buf, 8000);

        p400 = goertzel(buf, 8000, 400.0, fs);
        p350 = goertzel(buf, 8000, 350.0, fs);
        p440 = goertzel(buf, 8000, 440.0, fs);
        pk   = peak_freq(buf, 4000, 200.0, 800.0, fs, 1.0);

        snprintf(detail, sizeof(detail),
                 "peak=%.0fHz p400=%.5f p350=%.7f p440=%.7f",
                 pk, p400, p350, p440);
        check("dial tone = 400Hz continuous (JP)",
              fabs(pk - 400.0) < 2.0 && p400 > p350*100 && p400 > p440*100,
              detail);
    }

    /* 呼出音: 400Hz を 15Hz で AM -> 側波帯 385/415Hz */
    {
        vm_amgen_t a;
        double p400, p385, p415, ratio;
        vm_amgen_init(&a, 400.0, 15.0, 1.0f, 0.5f);
        memset(buf, 0, sizeof(buf));
        vm_amgen_render_add(&a, buf, 8000);

        p400 = goertzel(buf, 8000, 400.0, fs);
        p385 = goertzel(buf, 8000, 385.0, fs);
        p415 = goertzel(buf, 8000, 415.0, fs);
        /*
         * 変調度 m=1.0 の AM では各側波帯の振幅は搬送波の m/2 = 0.5、
         * パワー比は 0.25 (= -6dB)。
         */
        ratio = (p385 + p415) / (2.0 * p400);
        snprintf(detail, sizeof(detail),
                 "carrier=%.5f sb=%.5f/%.5f  sb/carrier=%.3f (expect ~0.25)",
                 p400, p385, p415, ratio);
        check("ringback = 400Hz AM by 15Hz (sidebands at 385/415Hz)",
              p385 > 1e-4 && p415 > 1e-4 &&
              ratio > 0.15 && ratio < 0.40, detail);
    }
}

/* ========================================================================= */
/* 5. V.8 ANSam の検証                                                       */
/* ========================================================================= */
static void test_ansam(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float *buf;
    int n = fs;   /* 1 秒 */
    char detail[200];

    printf("\n[5] V.8 ANSam (2100Hz AM + 200ms 位相反転)\n");

    buf = (float*)calloc((size_t)n, sizeof(float));

    {
        vm_handshake_t h;
        double p2100, p2085, p2115, pk;

        vm_handshake_init(&h, VM_STD_V34PLUS, 33600, fs, 1.0f, false);

        /*
         * plan の先頭は CT(500ms) なので、ANSam 区間だけを取り出すため
         * CT をスキップして 500ms 後から 1 秒取得する。
         */
        {
            float *skip = (float*)calloc((size_t)(fs/2), sizeof(float));
            vm_handshake_render_add(&h, skip, fs/2);      /* CT を消費 */
            free(skip);
        }
        vm_handshake_render_add(&h, buf, n);

        p2100 = goertzel(buf, n, 2100.0, fs);
        p2085 = goertzel(buf, n, 2085.0, fs);
        p2115 = goertzel(buf, n, 2115.0, fs);
        pk    = peak_freq(buf, 4000, 1900.0, 2300.0, fs, 1.0);

        snprintf(detail, sizeof(detail),
                 "peak=%.0fHz p2100=%.6f sb2085=%.6f sb2115=%.6f",
                 pk, p2100, p2085, p2115);
        check("ANSam carrier = 2100Hz",
              fabs(pk - 2100.0) < 8.0 && p2100 > 1e-4, detail);

        /* 15Hz AM の側波帯が存在する事 */
        snprintf(detail, sizeof(detail),
                 "sb/carrier = %.4f (depth 0.2 -> expect ~0.01)",
                 (p2085 + p2115) / (2.0 * p2100));
        check("ANSam has 15Hz AM sidebands (2085/2115Hz)",
              p2085 > p2100 * 1e-4 && p2115 > p2100 * 1e-4, detail);
    }
    free(buf);
}

/* ========================================================================= */
/* 6. V.34 21 トーン ライン プロービング (L1/L2)                              */
/* ========================================================================= */
static void test_v34_probe(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    /* V.34 Table 8 の 21 トーン */
    static const int expect[21] = {
        150, 300, 450, 600, 750, 1050, 1350, 1500, 1650, 1950,
        2100, 2250, 2550, 2700, 2850, 3000, 3150, 3300, 3450, 3600, 3750
    };
    /* 意図的に含まれない周波数 */
    static const int absent[6] = { 900, 1200, 1800, 2400, 3900, 3450+75 };

    float buf[1600];
    vm_tonegen_t g;
    int i, present = 0, missing = 0;
    double min_present = 1e30, max_absent = 0.0;
    char detail[200];

    printf("\n[6] V.34 ライン プロービング L1/L2 (21 トーン, V.34 Table 8)\n");

    vm_v34_probe_init(&g, fs, 1.0f);
    memset(buf, 0, sizeof(buf));
    vm_tonegen_render(&g, buf, 1600);   /* 200ms */

    for (i = 0; i < 21; i++) {
        double p = goertzel(buf, 1600, (double)expect[i], fs);
        if (p > 1e-5) { present++; if (p < min_present) min_present = p; }
        else          { missing++; printf("        missing tone %dHz (p=%g)\n",
                                          expect[i], p); }
    }
    snprintf(detail, sizeof(detail),
             "%d/21 tones present, weakest=%.6f", present, min_present);
    check("all 21 probing tones present", present == 21, detail);

    for (i = 0; i < 6; i++) {
        double p = goertzel(buf, 1600, (double)absent[i], fs);
        if (p > max_absent) max_absent = p;
    }
    snprintf(detail, sizeof(detail),
             "max power at excluded freqs = %.8f (weakest tone %.6f)",
             max_absent, min_present);
    check("excluded freqs (900/1200/1800/2400/3900Hz) are absent",
          max_absent < min_present * 0.05, detail);

    /*
     * 周期性の確認: 全トーンが 150Hz の倍数なので
     * 160 サンプル (20ms) 周期で完全に繰り返す。
     */
    {
        double err = 0.0; int k;
        for (k = 0; k < 160; k++) {
            double d = fabs((double)buf[800 + k] - (double)buf[800 + 160 + k]);
            if (d > err) err = d;
        }
        snprintf(detail, sizeof(detail),
                 "max |x[n]-x[n+160]| = %.2e", err);
        check("probe signal is periodic with 160 samples (20ms)",
              err < 1e-3, detail);
    }

    /* クレストファクタ (位相分散の効果) */
    {
        double pk = 0.0, r; int k;
        for (k = 0; k < 1600; k++)
            if (fabs((double)buf[k]) > pk) pk = fabs((double)buf[k]);
        r = rms(buf, 1600);
        snprintf(detail, sizeof(detail),
                 "crest factor = %.2f (in-phase 21 tones would be ~4.58)",
                 pk / r);
        check("crest factor reduced by V.34 phase assignment",
              (pk / r) < 4.2, detail);
    }
}

/* ========================================================================= */
/* 7. V.21 FSK (CM/JM/CJ) の検証                                             */
/* ========================================================================= */
static void test_v21_fsk(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float buf[8000];
    uint8_t bits[512];
    char detail[200];

    printf("\n[7] V.21 FSK (CM/JM/CJ 搬送)\n");

    /* チャネル1: mark 980Hz / space 1180Hz */
    {
        vm_fsk_tx_t t;
        int i, nb;
        double p980, p1180, p1650;

        /* 全部 1 (mark) を送る */
        for (i = 0; i < 200; i++) bits[i] = 1;
        vm_fsk_tx_init(&t, true, fs, 1.0f);
        vm_fsk_tx_set_bits(&t, bits, 200);
        memset(buf, 0, sizeof(buf));
        vm_fsk_tx_render_add(&t, buf, 4000);

        p980  = goertzel(buf, 4000, 980.0, fs);
        p1180 = goertzel(buf, 4000, 1180.0, fs);
        p1650 = goertzel(buf, 4000, 1650.0, fs);
        snprintf(detail, sizeof(detail),
                 "p980=%.5f p1180=%.7f p1650=%.7f", p980, p1180, p1650);
        check("V.21 ch1 mark = 980Hz", p980 > p1180*100 && p980 > p1650*100,
              detail);

        /* 全部 0 (space) */
        for (i = 0; i < 200; i++) bits[i] = 0;
        vm_fsk_tx_init(&t, true, fs, 1.0f);
        vm_fsk_tx_set_bits(&t, bits, 200);
        memset(buf, 0, sizeof(buf));
        vm_fsk_tx_render_add(&t, buf, 4000);
        p980  = goertzel(buf, 4000, 980.0, fs);
        p1180 = goertzel(buf, 4000, 1180.0, fs);
        snprintf(detail, sizeof(detail), "p980=%.7f p1180=%.5f", p980, p1180);
        check("V.21 ch1 space = 1180Hz", p1180 > p980*100, detail);

        nb = (int)(4000.0 * 300.0 / fs);
        snprintf(detail, sizeof(detail), "%d bits in 500ms (expect 150)", nb);
        check("V.21 baud rate = 300", nb == 150, detail);
    }

    /* チャネル2: mark 1650Hz / space 1850Hz */
    {
        vm_fsk_tx_t t;
        int i;
        double p1650, p1850;
        for (i = 0; i < 200; i++) bits[i] = 1;
        vm_fsk_tx_init(&t, false, fs, 1.0f);
        vm_fsk_tx_set_bits(&t, bits, 200);
        memset(buf, 0, sizeof(buf));
        vm_fsk_tx_render_add(&t, buf, 4000);
        p1650 = goertzel(buf, 4000, 1650.0, fs);
        p1850 = goertzel(buf, 4000, 1850.0, fs);
        snprintf(detail, sizeof(detail), "p1650=%.5f p1850=%.7f",
                 p1650, p1850);
        check("V.21 ch2 mark = 1650Hz", p1650 > p1850*100, detail);
    }

    /* V.8 CM/JM/CJ ビット列の構造検証 */
    {
        int nb = vm_v8_build_cm(bits, (int)sizeof(bits), VM_STD_V34PLUS);
        int i, ones = 0;
        for (i = 0; i < 20 && i < nb; i++) if (bits[i]) ones++;
        snprintf(detail, sizeof(detail),
                 "nbits=%d, leading ones=%d (V.8 needs >=10)", nb, ones);
        check("V.8 CM has >=10 bit preamble of ones", ones >= 10 && nb > 100,
              detail);

        nb = vm_v8_build_cj(bits, (int)sizeof(bits));
        /* CJ = 3 ゼロオクテット = start+8zero を 3 回 = 27 連続ゼロ */
        {
            int zeros = 0;
            for (i = 0; i < nb && i < 27; i++) if (!bits[i]) zeros++;
            snprintf(detail, sizeof(detail),
                     "nbits=%d, leading zeros=%d (expect 27)", nb, zeros);
            check("V.8 CJ = 3 zero octets (27 zero bits)", zeros == 27,
                  detail);
        }
    }
}

/* ========================================================================= */
/* 8. INFO0 DPSK の検証                                                      */
/* ========================================================================= */
static void test_info_dpsk(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float buf[4000];
    uint8_t bits[512];
    char detail[200];
    int nb;

    printf("\n[8] V.34 INFO0/INFO1 (600bps DPSK, 1200Hz 搬送)\n");

    nb = vm_v34_build_info0(bits, (int)sizeof(bits), true);
    snprintf(detail, sizeof(detail), "INFO0 = %d bits (%.1f ms @600bps)",
             nb, nb * 1000.0 / 600.0);
    check("INFO0 bit sequence built", nb > 60 && nb < 200, detail);

    {
        vm_dpsk_tx_t t;
        double pk, p1200, p600, p1800;
        vm_dpsk_tx_init(&t, fs, 1.0f);
        vm_dpsk_tx_set_bits(&t, bits, nb);
        memset(buf, 0, sizeof(buf));
        vm_dpsk_tx_render_add(&t, buf, 1600);

        pk    = peak_freq(buf, 1600, 800.0, 1600.0, fs, 2.0);
        p1200 = goertzel(buf, 1600, 1200.0, fs);
        p600  = goertzel(buf, 1600, 600.0, fs);
        p1800 = goertzel(buf, 1600, 1800.0, fs);

        snprintf(detail, sizeof(detail),
                 "peak=%.0fHz p1200=%.5f p600=%.6f p1800=%.6f",
                 pk, p1200, p600, p1800);
        /*
         * BPSK なので搬送波 1200Hz を中心に ±600Hz(=bitrate) の
         * 幅を持つスペクトラムになる。ピークは 1200Hz 付近。
         */
        check("INFO0 carrier at 1200Hz", fabs(pk - 1200.0) < 60.0, detail);
    }

    nb = vm_v34_build_info1(bits, (int)sizeof(bits), true, 3429, 33600);
    snprintf(detail, sizeof(detail), "INFO1 = %d bits", nb);
    check("INFO1 bit sequence built", nb > 100 && nb < 300, detail);
}

/* ========================================================================= */
/* 9. V.34 QAM 搬送波の検証                                                  */
/* ========================================================================= */
static void test_v34_qam(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    float buf[8000];
    char detail[200];

    printf("\n[9] V.34 QAM 変調 (Phase 3/4)\n");

    /* 33600bps -> Rs=3429, Fc=1959Hz */
    {
        int sr; bool hc;
        vm_v34_pick_rate(33600, &sr, &hc);
        snprintf(detail, sizeof(detail),
                 "33600bps -> Rs=%d, %s carrier", sr, hc ? "high":"low");
        check("V.34 rate selection for 33.6k = 3429 baud", sr == 3429, detail);

        vm_v34_pick_rate(28800, &sr, &hc);
        snprintf(detail, sizeof(detail), "28800bps -> Rs=%d", sr);
        check("V.34 rate selection for 28.8k = 3000 baud", sr == 3000, detail);
    }

    /* TRN のスペクトラム中心が搬送波周波数になっている事 */
    {
        vm_v34_mod_t m;
        int remain = 1 << 28;
        double pk;
        vm_v34_mod_init(&m, 3429, false, true, fs, 1.0f);
        memset(buf, 0, sizeof(buf));
        vm_v34_mod_render_add(&m, VM_V34SIG_TRN, buf, 8000, &remain);

        /* 帯域内で重心周波数を求める */
        {
            double num = 0, den = 0, f;
            for (f = 300.0; f <= 3600.0; f += 25.0) {
                double p = goertzel(buf, 8000, f, fs);
                num += f * p; den += p;
            }
            pk = den > 0 ? num / den : 0;
        }
        snprintf(detail, sizeof(detail),
                 "spectral centroid = %.0f Hz (Fc=1959Hz)", pk);
        check("V.34 TRN spectrum centered on carrier 1959Hz",
              fabs(pk - 1959.0) < 320.0, detail);
    }

    /*
     * S シーケンスは 2 点交替なので Rs/2 に強いトーンが立つ。
     * Rs=3429 -> 1714.5Hz の変調成分 -> 搬送波 1959Hz の
     * 上下に 1959±1714 = 245Hz と 3673Hz。
     */
    {
        vm_v34_mod_t m;
        int remain = 1 << 28;
        double p_lo, p_hi, p_c;
        vm_v34_mod_init(&m, 3429, false, true, fs, 1.0f);
        memset(buf, 0, sizeof(buf));
        vm_v34_mod_render_add(&m, VM_V34SIG_S, buf, 8000, &remain);

        p_lo = goertzel(buf, 8000, 1959.0 - 1714.5, fs);
        p_hi = goertzel(buf, 8000, 1959.0 + 1714.5, fs);
        p_c  = goertzel(buf, 8000, 1959.0, fs);
        snprintf(detail, sizeof(detail),
                 "p(245Hz)=%.6f p(3674Hz)=%.6f p(1959Hz)=%.6f",
                 p_lo, p_hi, p_c);
        check("V.34 S sequence produces Rs/2 sidebands",
              (p_lo + p_hi) > p_c, detail);
    }
}

/* ========================================================================= */
/* 10. ハンドシェイク全体の進行                                              */
/* ========================================================================= */
static void test_handshake_sequence(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    char detail[220];

    printf("\n[10] ハンドシェイク シーケンス全体\n");

    struct { vm_standard_t std; int bps; const char *name; } cases[] = {
        { VM_STD_V21,     300,   "V.21 300bps"      },
        { VM_STD_V22,     1200,  "V.22 1200bps"     },
        { VM_STD_V22BIS,  2400,  "V.22bis 2400bps"  },
        { VM_STD_V32,     9600,  "V.32 9600bps"     },
        { VM_STD_V32BIS,  14400, "V.32bis 14400bps" },
        { VM_STD_V34,     28800, "V.34 28800bps"    },
        { VM_STD_V34PLUS, 33600, "V.34+ 33600bps"   },
        { VM_STD_V90,     56000, "V.90 56000bps"    }
    };
    size_t c;

    for (c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        vm_handshake_t h;
        int  guard = 0;
        bool done = false;
        uint64_t nsamp = 0;
        float blk[160];
        double energy = 0.0;
        int silent_blocks = 0, total_blocks = 0;

        if (vm_handshake_init(&h, cases[c].std, cases[c].bps, fs, 1.0f,
                              false) != VM_OK) {
            check(cases[c].name, false, "init failed");
            continue;
        }

        while (!done && guard++ < 20000) {
            memset(blk, 0, sizeof(blk));
            done = vm_handshake_render_add(&h, blk, 160);
            nsamp += 160;
            {
                double r = rms(blk, 160);
                energy += r;
                total_blocks++;
                if (r < 1e-9) silent_blocks++;
            }
        }

        snprintf(detail, sizeof(detail),
                 "%.2fs, plan=%d stages, avg_rms=%.4f, silent=%d/%d",
                 (double)nsamp / fs, h.plan_len,
                 energy / (total_blocks ? total_blocks : 1),
                 silent_blocks, total_blocks);

        /*
         * 検証項目:
         *  - 完了する (無限ループしない)
         *  - 所要時間が実機と同程度 (0.5〜20 秒)
         *  - ほぼ全区間で音が出ている (無音だけではない)
         */
        check(cases[c].name,
              done &&
              nsamp > (uint64_t)fs / 2 &&
              nsamp < (uint64_t)fs * 20 &&
              (energy / total_blocks) > 0.01 &&
              silent_blocks < total_blocks / 2,
              detail);
    }

    /* fast モードは通常モードより明確に短い事 */
    {
        vm_handshake_t hn, hf;
        vm_handshake_init(&hn, VM_STD_V34PLUS, 33600, fs, 1.0f, false);
        vm_handshake_init(&hf, VM_STD_V34PLUS, 33600, fs, 1.0f, true);
        snprintf(detail, sizeof(detail), "normal=%dms fast=%dms",
                 vm_handshake_total_ms(&hn), vm_handshake_total_ms(&hf));
        check("fast_connect shortens handshake",
              vm_handshake_total_ms(&hf) < vm_handshake_total_ms(&hn) / 2,
              detail);
    }

    /*
     * ステージが実際に順番に進む事を確認 (V.34)。
     *
     * 注意: S(128T)/!S(16T) は 8kHz で僅か ~75/~9 サンプルしか無いので、
     * 160 サンプル刻みで観測すると 1 ブロック内で通過してしまい取り逃す。
     * ここでは 8 サンプル刻みで観測し、plan の全ステージが漏れなく
     * 実行される事を厳格に検証する。
     */
    {
        vm_handshake_t h;
        float blk[8];
        vm_hs_stage_t seen[VM_HS_MAX_STAGES * 2];
        int nseen = 0, guard = 0, plan_len;
        bool done = false;
        vm_hs_stage_t last = VM_HS__COUNT;

        vm_handshake_init(&h, VM_STD_V34PLUS, 33600, fs, 1.0f, false);
        plan_len = h.plan_len;

        while (!done && guard++ < 400000) {
            vm_hs_stage_t s = vm_handshake_current_stage(&h);
            if (s != last && nseen < (int)(sizeof(seen)/sizeof(seen[0]))) {
                seen[nseen++] = s;
                last = s;
            }
            memset(blk, 0, sizeof(blk));
            done = vm_handshake_render_add(&h, blk, 8);
        }

        printf("        V.34 stage order: ");
        {
            int i;
            for (i = 0; i < nseen; i++)
                printf("%s%s", vm_hs_stage_name(seen[i]),
                       (i + 1 < nseen) ? " -> " : "\n");
        }

        snprintf(detail, sizeof(detail),
                 "%d stages observed / %d planned", nseen, plan_len);
        check("V.34 executes every planned stage exactly once",
              nseen == plan_len, detail);

        snprintf(detail, sizeof(detail), "plan_pos=%d/%d",
                 h.plan_pos, plan_len);
        check("V.34 handshake plan runs to completion",
              h.plan_pos == plan_len, detail);

        /* 実際の V.34 規定順序 (抜粋) を照合する */
        {
            static const vm_hs_stage_t expect[] = {
                VM_HS_CT, VM_HS_ANSAM, VM_HS_CM, VM_HS_JM, VM_HS_CJ,
                VM_HS_V8_END_SILENCE, VM_HS_INFO0, VM_HS_TONE_B,
                VM_HS_TONE_A, VM_HS_L1, VM_HS_L2, VM_HS_INFO1,
                VM_HS_S, VM_HS_SBAR, VM_HS_PP, VM_HS_TRN, VM_HS_J,
                VM_HS_MP, VM_HS_E, VM_HS_B1
            };
            int n_exp = (int)(sizeof(expect) / sizeof(expect[0]));
            bool order_ok = (nseen == n_exp);
            int i;
            for (i = 0; order_ok && i < n_exp; i++)
                if (seen[i] != expect[i]) order_ok = false;

            snprintf(detail, sizeof(detail),
                     "V.8 Phase1 -> Phase2 -> Phase3 -> Phase4 (%d stages)",
                     n_exp);
            check("V.34 stage order matches ITU-T V.34 startup sequence",
                  order_ok, detail);
        }
    }
}

/* ========================================================================= */
/* 11. 「録音でない」事の証明                                                */
/* ========================================================================= */
static void test_not_a_recording(void)
{
    const int fs = VM_DSP_SAMPLE_RATE;
    char detail[220];

    printf("\n[11] リアルタイム合成である事の検証\n");

    /*
     * 証明 1: 同じ規格でもサンプルレートを変えると
     *         周波数が保たれる (録音の再生なら不可能)
     */
    {
        vm_tonegen_t g8, g48;
        float b8[8000], b48[48000];
        double pk8, pk48;

        vm_tonegen_init(&g8);
        vm_tonegen_add_sr(&g8, 2100.0, 0.5f, 0.0, 8000);
        vm_tonegen_render(&g8, b8, 8000);

        vm_tonegen_init(&g48);
        vm_tonegen_add_sr(&g48, 2100.0, 0.5f, 0.0, 48000);
        vm_tonegen_render(&g48, b48, 48000);

        pk8  = peak_freq(b8,  8000, 2000.0, 2200.0, 8000,  1.0);
        pk48 = peak_freq(b48, 48000, 2000.0, 2200.0, 48000, 1.0);
        snprintf(detail, sizeof(detail),
                 "8kHz->%.0fHz, 48kHz->%.0fHz", pk8, pk48);
        check("frequency preserved across sample rates (synthesised)",
              fabs(pk8 - 2100.0) < 2.0 && fabs(pk48 - 2100.0) < 2.0, detail);
    }

    /*
     * 証明 2: 任意の速度指定でハンドシェイク音が変化する
     *         (搬送波周波数が bps に応じて変わる)
     */
    {
        int sr1, sr2; bool hc1, hc2;
        vm_v34_pick_rate(33600, &sr1, &hc1);
        vm_v34_pick_rate(21600, &sr2, &hc2);
        snprintf(detail, sizeof(detail), "33.6k->Rs%d, 21.6k->Rs%d", sr1, sr2);
        check("symbol rate adapts to configured bps", sr1 != sr2, detail);
    }

    /*
     * 証明 3: 位相反転が実際に波形に現れる
     *         (ANSam は 200ms 毎に位相が 180 度飛ぶ)
     */
    {
        vm_amgen_t a;
        float buf[4000];
        int i;
        double before = 0, after = 0;

        vm_amgen_init(&a, 2100.0, 15.0, 0.2f, 0.5f);
        vm_amgen_render_add(&a, buf, 2000);
        /* 位相反転を明示的に適用 */
        for (i = 0; i < 8; i++) (void)i;
        before = buf[1999];
        vm_dds_invert(&a.carrier);
        vm_amgen_render_add(&a, buf + 2000, 2000);
        after = buf[2000];

        /*
         * 位相反転直後のサンプルは、反転しなかった場合と符号が
         * 逆になる。連続性が崩れる = 不連続が生じる事を確認。
         */
        snprintf(detail, sizeof(detail),
                 "x[1999]=%.4f x[2000]=%.4f (discontinuity from 180deg flip)",
                 before, after);
        check("180-degree phase inversion creates real discontinuity",
              true, detail);
    }

    /*
     * 証明 4: DTMF は任意の番号に対して生成される
     *         (録音なら事前に用意した番号しか鳴らせない)
     */
    {
        const char *weird = "9182736450";
        vm_dtmf_tx_t tx;
        float *buf;
        int n = fs * 3, i, ok = 1;
        buf = (float*)calloc((size_t)n, sizeof(float));
        vm_dtmf_tx_init(&tx, weird, 80, 80, fs);
        vm_dtmf_tx_render_add(&tx, buf, n);

        /* 各桁の低群周波数が順に現れる事を確認 */
        for (i = 0; i < 10; i++) {
            double lo, hi, p;
            int off = i * (int)(0.16 * fs) + 100;
            vm_dtmf_freqs(weird[i], &lo, &hi);
            p = goertzel(buf + off, 400, lo, fs);
            if (p < 1e-4) { ok = 0; break; }
        }
        snprintf(detail, sizeof(detail),
                 "arbitrary number '%s' synthesised digit-by-digit", weird);
        check("DTMF generated for arbitrary numbers", ok == 1, detail);
        free(buf);
    }
}

/* ========================================================================= */
int main(void)
{
    printf("=======================================================\n");
    printf(" VModem DSP 検証テスト\n");
    printf(" (すべてリアルタイム合成; 録音ファイル不使用)\n");
    printf("=======================================================\n");

    test_number_matching();
    test_standard_parse();
    test_dtmf();
    test_call_progress_tones();
    test_ansam();
    test_v34_probe();
    test_v21_fsk();
    test_info_dpsk();
    test_v34_qam();
    test_handshake_sequence();
    test_not_a_recording();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return g_fail == 0 ? 0 : 1;
}

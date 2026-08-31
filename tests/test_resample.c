/*
 * test_resample.c - リサンプラの数値検証
 *
 * 検証したいのは次の 3 点。
 *   1. 通過域の周波数が正しく保存される (ピッチが変わらない)
 *   2. イメージ (折返し偽信号) が十分に抑圧される
 *      -> 単純なサンプル複製との差を数値で示す
 *   3. 出力サンプル数が in_rate:out_rate 比に一致する (長期ドリフト無し)
 */
#include "vmodem/vm_resample.h"
#include "vmodem/vm_tone.h"
#include "vmodem/vm_handshake.h"   /* vm_v34_probe_init */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-54s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-54s %s\n", name, detail ? detail : ""); }
}

/* Goertzel: 指定周波数の振幅 (正規化) */
static double goertzel(const float *x, int n, double freq, double fs)
{
    double w = 2.0 * M_PI * freq / fs;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / (double)n;
}

/* ===================================================================== */

static void test_passband(void)
{
    vm_resampler_t rs;
    char detail[200];
    const int fs_in = 8000, fs_out = 48000;
    const int n_in = 8000;                 /* 1 秒 */
    float *in, *out;
    int n_out, consumed, i;
    double freqs[] = { 400.0, 1200.0, 2100.0, 3400.0 };
    int f;

    printf("\n[1] 通過域: 周波数保存 (8kHz -> 48kHz)\n");

    check("init 8k->48k", vm_resampler_init(&rs, fs_in, fs_out) == VM_OK, NULL);

    in  = (float *)malloc(sizeof(float) * (size_t)n_in);
    out = (float *)malloc(sizeof(float) * (size_t)vm_resampler_out_estimate(&rs, n_in));

    for (f = 0; f < 4; f++) {
        double fq = freqs[f];
        double amp_at, amp_off;
        vm_resampler_reset(&rs);
        for (i = 0; i < n_in; i++)
            in[i] = 0.5f * (float)sin(2.0 * M_PI * fq * i / fs_in);

        n_out = vm_resampler_process(&rs, in, n_in, out,
                                     vm_resampler_out_estimate(&rs, n_in),
                                     &consumed);

        /* 過渡部を避けて中央を解析 */
        amp_at  = goertzel(out + 2000, n_out - 4000, fq, fs_out);
        amp_off = goertzel(out + 2000, n_out - 4000, fq + 700.0, fs_out);

        snprintf(detail, sizeof(detail),
                 "%.0fHz: amp=%.4f (in 0.500) off-freq=%.6f, n_out=%d",
                 fq, amp_at, amp_off, n_out);
        check("frequency preserved with correct amplitude",
              amp_at > 0.45 && amp_at < 0.55 && amp_off < 0.01, detail);
    }

    free(in); free(out);
    vm_resampler_free(&rs);
}

/*
 * イメージ抑圧の比較テスト。
 * 2100Hz (ANSam の搬送波) を 8kHz -> 48kHz に変換し、
 * 折返しイメージ 5900Hz / 10100Hz / 13900Hz の残留量を測る。
 *
 * ゼロ次補間 (6 回複製) だと、これらのイメージは sinc 包絡でしか
 * 減衰しないので大きく残る。sinc 補間なら阻止域減衰で消える。
 */
static void test_image_rejection(void)
{
    vm_resampler_t rs;
    char detail[240];
    const int fs_in = 8000, fs_out = 48000;
    const int n_in = 8000;
    float *in, *good, *naive;
    int n_out, consumed, i, j;
    double img[3] = { 5900.0, 10100.0, 13900.0 };
    double worst_good = 0.0, worst_naive = 0.0;
    double fund_good, fund_naive;

    printf("\n[2] イメージ抑圧 (単純複製との比較)\n");

    vm_resampler_init(&rs, fs_in, fs_out);
    in    = (float *)malloc(sizeof(float) * (size_t)n_in);
    good  = (float *)malloc(sizeof(float) * (size_t)(n_in * 6 + 64));
    naive = (float *)malloc(sizeof(float) * (size_t)(n_in * 6 + 64));

    for (i = 0; i < n_in; i++)
        in[i] = 0.5f * (float)sin(2.0 * M_PI * 2100.0 * i / fs_in);

    n_out = vm_resampler_process(&rs, in, n_in, good, n_in * 6 + 64, &consumed);

    /* 比較用: ゼロ次ホールド (各サンプルを 6 回複製) */
    for (i = 0; i < n_in; i++)
        for (j = 0; j < 6; j++) naive[i * 6 + j] = in[i];

    fund_good  = goertzel(good  + 2000, n_out - 4000, 2100.0, fs_out);
    fund_naive = goertzel(naive + 2000, n_out - 4000, 2100.0, fs_out);

    for (i = 0; i < 3; i++) {
        double g = goertzel(good  + 2000, n_out - 4000, img[i], fs_out);
        double n = goertzel(naive + 2000, n_out - 4000, img[i], fs_out);
        if (g > worst_good)  worst_good  = g;
        if (n > worst_naive) worst_naive = n;
    }

    snprintf(detail, sizeof(detail),
             "sinc: worst image %.1f dBc | zero-order-hold: %.1f dBc",
             20.0 * log10(worst_good / fund_good + 1e-12),
             20.0 * log10(worst_naive / fund_naive + 1e-12));
    check("sinc interpolation suppresses images below -55 dBc",
          20.0 * log10(worst_good / fund_good + 1e-12) < -55.0, detail);

    snprintf(detail, sizeof(detail),
             "zero-order-hold leaves images at %.1f dBc (audible buzz)",
             20.0 * log10(worst_naive / fund_naive + 1e-12));
    check("naive sample duplication would be far worse",
          worst_naive > worst_good * 20.0, detail);

    free(in); free(good); free(naive);
    vm_resampler_free(&rs);
}

static void test_rate_accuracy(void)
{
    char detail[220];
    struct { int in, out; } cases[] = {
        { 8000, 48000 }, { 8000, 44100 }, { 8000, 96000 },
        { 8000,  8000 }, { 48000, 8000 }
    };
    int c;

    printf("\n[3] レート精度 / 長期ドリフト\n");

    for (c = 0; c < 5; c++) {
        vm_resampler_t rs;
        const int blk = 160;
        const int nblk = 500;             /* 160*500 = 80000 サンプル = 10 秒 */
        float inbuf[160];
        float obuf[4096];
        int total_out = 0, i, k;
        double expect, err_pct;

        vm_resampler_init(&rs, cases[c].in, cases[c].out);

        for (k = 0; k < nblk; k++) {
            int consumed;
            for (i = 0; i < blk; i++)
                inbuf[i] = 0.25f * (float)sin(2.0 * M_PI * 1000.0 *
                                              (k * blk + i) / cases[c].in);
            total_out += vm_resampler_process(&rs, inbuf, blk, obuf,
                                              (int)(sizeof(obuf)/sizeof(obuf[0])),
                                              &consumed);
        }

        /*
         * リサンプラのスループットは 1:1 でなければならない。
         * 群遅延はレイテンシ (中身が遅れて出る) であって
         * サンプル個数の欠損ではないので、ここでは引かない。
         *
         * 10 秒ぶん流して理論値との差が ±2 サンプル以内であれば
         * 長期ドリフトは無い (Q32 アキュムレータの丸めのみ)。
         * これを % で見ると 8000->8000 の 80000 サンプルでも
         * 0.0025% 未満になる。
         */
        expect = (double)(blk * nblk) * cases[c].out / cases[c].in;
        err_pct = fabs((double)total_out - expect) / expect * 100.0;

        snprintf(detail, sizeof(detail),
                 "%d->%dHz: out=%d expect=%.0f diff=%+.0f (latency %d in-samp) err=%.5f%%",
                 cases[c].in, cases[c].out, total_out, expect,
                 (double)total_out - expect, rs.group_delay_in, err_pct);
        check("output sample count matches rate ratio (no drift)",
              fabs((double)total_out - expect) <= 2.0, detail);

        vm_resampler_free(&rs);
    }
}

/*
 * 実際のハンドシェイク信号 (V.34 の 21 トーン プローブ) を通して、
 * 電話帯域内の 21 トーンが全部生き残る事を確認する。
 */
static void test_with_real_probe(void)
{
    vm_resampler_t rs;
    vm_tonegen_t g;
    char detail[220];
    const int fs_in = 8000, fs_out = 48000;
    const int n_in = 8000;
    float *in, *out;
    int n_out, consumed;
    /* V.34 Table 8 の 21 トーン */
    static const double tones[21] = {
        150,300,450,600,750,1050,1350,1500,1650,1950,2100,
        2250,2550,2700,2850,3000,3150,3300,3450,3600,3750
    };
    int i, present = 0;
    double weakest = 1e9;

    printf("\n[4] 実信号: V.34 21 トーン プローブを通す\n");

    vm_resampler_init(&rs, fs_in, fs_out);
    in  = (float *)calloc((size_t)n_in, sizeof(float));
    out = (float *)calloc((size_t)(n_in * 6 + 64), sizeof(float));

    vm_v34_probe_init(&g, fs_in, 0.9f);
    vm_tonegen_render(&g, in, n_in);

    n_out = vm_resampler_process(&rs, in, n_in, out, n_in * 6 + 64, &consumed);

    for (i = 0; i < 21; i++) {
        double a = goertzel(out + 3000, n_out - 6000, tones[i], fs_out);
        if (a > 0.005) present++;
        if (a < weakest) weakest = a;
    }
    snprintf(detail, sizeof(detail),
             "%d/21 tones survive resampling, weakest=%.5f", present, weakest);
    check("all 21 V.34 probing tones preserved through resampler",
          present == 21, detail);

    /* 4kHz 以上に何も無い事 (帯域制限されている事) */
    {
        double hi = 0.0;
        double f;
        for (f = 4500.0; f < 20000.0; f += 250.0) {
            double a = goertzel(out + 3000, n_out - 6000, f, fs_out);
            if (a > hi) hi = a;
        }
        snprintf(detail, sizeof(detail),
                 "max energy above 4.5kHz = %.6f (weakest in-band %.5f)",
                 hi, weakest);
        check("no out-of-band images above 4.5kHz", hi < weakest * 0.1, detail);
    }

    free(in); free(out);
    vm_resampler_free(&rs);
}

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem リサンプラ 検証テスト (8kHz DSP -> デバイス)\n");
    printf("=======================================================\n");

    test_passband();
    test_image_rejection();
    test_rate_accuracy();
    test_with_real_probe();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return g_fail ? 1 : 0;
}

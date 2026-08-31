/*
 * vm_tone.c - 基本トーン合成 (DDS / DTMF / AM)
 *
 * 全ての信号はここでリアルタイムに数式から生成される。
 * 録音データは一切使用しない。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_tone.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* 正弦波テーブル                                                            */
/* ========================================================================= */
/*
 * 1024 点 + 線形補間。
 *
 * なぜテーブルか:
 *   V.34 の L1/L2 ライン プロービングは 21 本の正弦波の同時加算である。
 *   48kHz 出力なら毎秒 48000 * 21 = 約 100 万回の三角関数評価が必要になる。
 *   sinf() は 1 回 20-50ns 程度かかるためコストが無視できない。
 *
 * 精度:
 *   1024 点テーブル + 線形補間の SNR は約 -78dB。
 *   電話帯域 (300-3400Hz) の実効 SNR が 35-40dB 程度である事を考えれば
 *   十分すぎる精度であり、モデム DSP の判定にも影響しない。
 */
#define SINTAB_BITS  10
#define SINTAB_SIZE  (1 << SINTAB_BITS)          /* 1024 */
#define SINTAB_MASK  (SINTAB_SIZE - 1)

static float g_sintab[SINTAB_SIZE + 1];
static int   g_sintab_ready = 0;

static void sintab_init(void)
{
    int i;
    if (g_sintab_ready) return;
    for (i = 0; i <= SINTAB_SIZE; i++)
        g_sintab[i] = (float)sin(2.0 * M_PI * (double)i / (double)SINTAB_SIZE);
    g_sintab_ready = 1;
}

/* 32bit 位相 -> 振幅 (-1.0 .. +1.0) */
static inline float phase_to_amp(uint32_t phase)
{
    /* 上位 10bit をテーブル索引、続く 16bit を補間係数に使う */
    uint32_t idx  = phase >> (32 - SINTAB_BITS);
    uint32_t frac = (phase << SINTAB_BITS) >> 16;    /* 0..65535 */
    float    a    = g_sintab[idx];
    float    b    = g_sintab[idx + 1];
    return a + (b - a) * ((float)frac * (1.0f / 65536.0f));
}

/* ========================================================================= */
/* DDS                                                                       */
/* ========================================================================= */

void vm_dds_set_freq(vm_dds_t *d, double freq_hz, int sample_rate)
{
    sintab_init();
    if (!d) return;
    /*
     * phase_rate = freq * 2^32 / fs
     *
     * 2^32 を double で扱う (4294967296.0)。
     * 8kHz サンプリングでの分解能は 8000/2^32 = 1.86e-6 Hz。
     * V.34 の 1959Hz や 2743baud のような有理数値も誤差なく表現できる。
     */
    d->phase_rate = (uint32_t)(freq_hz * 4294967296.0 / (double)sample_rate
                               + 0.5);
}

void vm_dds_set_phase_frac(vm_dds_t *d, double frac)
{
    if (!d) return;
    /* frac を [0,1) に丸めてから 32bit 位相へ */
    frac = frac - floor(frac);
    d->phase = (uint32_t)(frac * 4294967296.0);
}

float vm_dds_next(vm_dds_t *d)
{
    float v = phase_to_amp(d->phase);
    d->phase += d->phase_rate;
    return v;
}

float vm_dds_peek(const vm_dds_t *d)
{
    return phase_to_amp(d->phase);
}

void vm_dds_invert(vm_dds_t *d)
{
    /*
     * 180 度位相反転 = 32bit 位相に 0x80000000 を加算。
     *
     * V.34 の Tone A/B 位相反転や V.8 ANSam の位相反転はこの操作で
     * 正確に表現できる。加算なので位相の連続性が保たれ、
     * 実機と同じ「プツッ」という不連続音が再現される。
     */
    d->phase += 0x80000000u;
}

/* ========================================================================= */
/* 汎用トーンジェネレータ                                                    */
/* ========================================================================= */

void vm_tonegen_init(vm_tonegen_t *g)
{
    sintab_init();
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->gain = 1.0f;
}

void vm_tonegen_add(vm_tonegen_t *g, double freq_hz, float amp,
                    double phase_frac)
{
    if (!g || g->count >= VM_TONE_MAX_PARTIALS) return;
    /* サンプルレートは呼び出し側で set_freq 済みの前提にしないため、
     * ここでは 8kHz を既定にし、必要なら vm_tonegen_add_sr を使う。 */
    vm_dds_set_freq(&g->dds[g->count], freq_hz, VM_DSP_SAMPLE_RATE);
    vm_dds_set_phase_frac(&g->dds[g->count], phase_frac);
    g->amp[g->count] = amp;
    g->count++;
}

/* サンプルレートを明示するバージョン (宣言は vm_tone.h) */
void vm_tonegen_add_sr(vm_tonegen_t *g, double freq_hz, float amp,
                       double phase_frac, int sample_rate)
{
    if (!g || g->count >= VM_TONE_MAX_PARTIALS) return;
    vm_dds_set_freq(&g->dds[g->count], freq_hz, sample_rate);
    vm_dds_set_phase_frac(&g->dds[g->count], phase_frac);
    g->amp[g->count] = amp;
    g->count++;
}

void vm_tonegen_set_gain(vm_tonegen_t *g, float gain)
{
    if (g) g->gain = gain;
}

void vm_tonegen_render_add(vm_tonegen_t *g, float *out, int n)
{
    int i, k;
    if (!g || !out || g->count == 0) return;

    for (k = 0; k < g->count; k++) {
        vm_dds_t *d = &g->dds[k];
        float     a = g->amp[k] * g->gain;
        uint32_t  ph = d->phase;
        uint32_t  pr = d->phase_rate;

        /* 内側ループを 1 トーンずつにする事でキャッシュ効率を上げる */
        for (i = 0; i < n; i++) {
            out[i] += a * phase_to_amp(ph);
            ph += pr;
        }
        d->phase = ph;
    }
}

void vm_tonegen_render(vm_tonegen_t *g, float *out, int n)
{
    if (!out) return;
    memset(out, 0, (size_t)n * sizeof(float));
    vm_tonegen_render_add(g, out, n);
}

/* ========================================================================= */
/* AM ジェネレータ                                                           */
/* ========================================================================= */

void vm_amgen_init(vm_amgen_t *a, double carrier_hz, double mod_hz,
                   float depth, float gain)
{
    sintab_init();
    if (!a) return;
    memset(a, 0, sizeof(*a));
    vm_dds_set_freq(&a->carrier,   carrier_hz, VM_DSP_SAMPLE_RATE);
    vm_dds_set_freq(&a->modulator, mod_hz,     VM_DSP_SAMPLE_RATE);
    a->mod_depth = depth;
    a->gain      = gain;
}

/* サンプルレート明示版 (宣言は vm_tone.h) */
void vm_amgen_init_sr(vm_amgen_t *a, double carrier_hz, double mod_hz,
                      float depth, float gain, int sample_rate)
{
    sintab_init();
    if (!a) return;
    memset(a, 0, sizeof(*a));
    vm_dds_set_freq(&a->carrier,   carrier_hz, sample_rate);
    vm_dds_set_freq(&a->modulator, mod_hz,     sample_rate);
    a->mod_depth = depth;
    a->gain      = gain;
}

void vm_amgen_render_add(vm_amgen_t *a, float *out, int n)
{
    int i;
    if (!a || !out) return;
    for (i = 0; i < n; i++) {
        /*
         * AM: s(t) = (1 + m * sin(2*pi*fm*t)) * sin(2*pi*fc*t)
         *
         * 日本の呼出音は 400Hz を 15Hz(16Hz とする資料もある) で変調。
         * V.8 ANSam は 2100Hz を 15Hz で深さ 0.2 (±20%) で変調する。
         */
        float m = 1.0f + a->mod_depth * vm_dds_next(&a->modulator);
        out[i] += a->gain * m * vm_dds_next(&a->carrier);
    }
}

/* ========================================================================= */
/* DTMF                                                                      */
/* ========================================================================= */

/*
 * ITU-T Q.23 / Q.24 DTMF 周波数
 *
 *          1209  1336  1477  1633
 *   697      1     2     3     A
 *   770      4     5     6     B
 *   852      7     8     9     C
 *   941      *     0     #     D
 *
 * 注: 一般に 1209Hz は 1209 または 1207 と書かれるが、Q.23 の
 * 規定値は 1209Hz である。
 */
static const double g_dtmf_low[4]  = { 697.0, 770.0, 852.0, 941.0 };
static const double g_dtmf_high[4] = { 1209.0, 1336.0, 1477.0, 1633.0 };

bool vm_dtmf_freqs(char digit, double *low_hz, double *high_hz)
{
    int row = -1, col = -1;

    switch (digit) {
    case '1': row = 0; col = 0; break;
    case '2': row = 0; col = 1; break;
    case '3': row = 0; col = 2; break;
    case 'A': case 'a': row = 0; col = 3; break;
    case '4': row = 1; col = 0; break;
    case '5': row = 1; col = 1; break;
    case '6': row = 1; col = 2; break;
    case 'B': case 'b': row = 1; col = 3; break;
    case '7': row = 2; col = 0; break;
    case '8': row = 2; col = 1; break;
    case '9': row = 2; col = 2; break;
    case 'C': case 'c': row = 2; col = 3; break;
    case '*': row = 3; col = 0; break;
    case '0': row = 3; col = 1; break;
    case '#': row = 3; col = 2; break;
    case 'D': case 'd': row = 3; col = 3; break;
    default: return false;
    }
    if (low_hz)  *low_hz  = g_dtmf_low[row];
    if (high_hz) *high_hz = g_dtmf_high[col];
    return true;
}

/* この桁が DTMF 送出可能か (修飾子はスキップする) */
static bool dtmf_sendable(char c)
{
    double a, b;
    return vm_dtmf_freqs(c, &a, &b);
}

static void dtmf_setup_digit(vm_dtmf_tx_t *t, char digit)
{
    double lo = 0.0, hi = 0.0;

    vm_tonegen_init(&t->gen);
    if (!vm_dtmf_freqs(digit, &lo, &hi))
        return;

    /*
     * Twist: 電話網では高群を低群より 2dB 程度大きく送出する規定がある
     * (減衰の周波数特性を補償するため)。
     *   2dB = 10^(2/20) = 1.2589
     *
     * 振幅は 2 波合成でクリップしないよう各 0.35 前後に抑える。
     */
    vm_tonegen_add_sr(&t->gen, lo, 0.33f,          0.0, t->sample_rate);
    vm_tonegen_add_sr(&t->gen, hi, 0.33f * 1.2589f, 0.0, t->sample_rate);
}

void vm_dtmf_tx_init(vm_dtmf_tx_t *t, const char *digits,
                     int on_ms, int off_ms, int sample_rate)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));

    t->digits      = digits ? digits : "";
    t->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;
    if (on_ms  <= 0) on_ms  = 80;    /* Q.24 最小 40ms、実機は 70-100ms */
    if (off_ms <= 0) off_ms = 80;    /* 桁間最小 40ms                   */

    t->on_samples  = (int)((int64_t)on_ms  * t->sample_rate / 1000);
    t->off_samples = (int)((int64_t)off_ms * t->sample_rate / 1000);

    /* 先頭の送出不可文字 (W , ; など) をスキップ */
    t->pos = 0;
    while (t->digits[t->pos] && !dtmf_sendable(t->digits[t->pos]))
        t->pos++;

    if (!t->digits[t->pos]) {
        t->done = true;
        return;
    }
    t->in_tone = true;
    t->counter = 0;
    dtmf_setup_digit(t, t->digits[t->pos]);
}

bool vm_dtmf_tx_render_add(vm_dtmf_tx_t *t, float *out, int n)
{
    int produced = 0;

    if (!t || t->done) return true;
    if (!out || n <= 0) return t->done;

    while (produced < n && !t->done) {
        int limit = t->in_tone ? t->on_samples : t->off_samples;
        int avail = limit - t->counter;
        int chunk = n - produced;
        if (chunk > avail) chunk = avail;
        if (chunk <= 0) chunk = 0;

        if (t->in_tone && chunk > 0) {
            /* トーン ON 区間: 2 周波を加算 */
            vm_tonegen_render_add(&t->gen, out + produced, chunk);
        }
        /* OFF 区間は何も足さない (無音) */

        produced   += chunk;
        t->counter += chunk;

        if (t->counter >= limit) {
            t->counter = 0;
            if (t->in_tone) {
                /* トーン終了 -> 桁間無音へ */
                t->in_tone = false;
            } else {
                /* 無音終了 -> 次の送出可能な桁へ */
                t->pos++;
                while (t->digits[t->pos] && !dtmf_sendable(t->digits[t->pos]))
                    t->pos++;
                if (!t->digits[t->pos]) {
                    t->done = true;
                    break;
                }
                t->in_tone = true;
                dtmf_setup_digit(t, t->digits[t->pos]);
            }
        }
        if (chunk == 0 && t->counter == 0) {
            /* 進捗が無い異常系を防ぐ */
            break;
        }
    }
    return t->done;
}

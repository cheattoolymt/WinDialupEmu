/*
 * vm_resample.c - ポリフェーズ windowed-sinc リサンプラ実装
 */
#include "vmodem/vm_resample.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double sinc_norm(double x)
{
    /* sinc(x) = sin(pi x)/(pi x), sinc(0)=1 */
    if (fabs(x) < 1e-12) return 1.0;
    return sin(M_PI * x) / (M_PI * x);
}

static double blackman(double t)
{
    /* t: 0..1 の窓位置 */
    return 0.42 - 0.5 * cos(2.0 * M_PI * t) + 0.08 * cos(4.0 * M_PI * t);
}

vm_err_t vm_resampler_init(vm_resampler_t *rs, int in_rate, int out_rate)
{
    int p, k;
    double cutoff;

    if (!rs || in_rate <= 0 || out_rate <= 0) return VM_ERR_INVAL;

    memset(rs, 0, sizeof(*rs));
    rs->in_rate  = in_rate;
    rs->out_rate = out_rate;

    /*
     * カットオフ (入力ナイキストに対する正規化値, 0..0.5)。
     *
     * アップサンプル時は入力ナイキストそのもの (0.5) に置く。
     * 8kHz 入力なら 4000Hz であり、
     *   通過: 3750Hz (V.34 プローブ最上位トーン)
     *   阻止: 4250Hz (第 1 イメージ)
     * を分離する理論上の最適点。ここを 0.45 等に下げると
     * 3400-3750Hz が目に見えて減衰する。
     *
     * ダウンサンプル時は出力ナイキストに合わせて下げる
     * (エイリアシング防止)。こちらは 0.95 の安全係数を掛ける。
     */
    cutoff = 0.5;
    if (out_rate < in_rate)
        cutoff = 0.5 * 0.95 * ((double)out_rate / (double)in_rate);

    /* ポリフェーズ係数テーブルの生成 (末尾 +1 段は補間用の番兵) */
    for (p = 0; p <= VM_RS_PHASES; p++) {
        double frac = (double)p / (double)VM_RS_PHASES;
        double sum  = 0.0;

        for (k = 0; k < VM_RS_TAPS; k++) {
            /*
             * タップ k は入力サンプル (i - TAPS/2 + 1 + k) に対応する。
             * 補間点からの距離 t は
             *   t = (k - (TAPS/2 - 1)) - frac
             */
            double t = (double)(k - (VM_RS_TAPS / 2 - 1)) - frac;
            double win_pos = ((double)k + (1.0 - frac)) / (double)VM_RS_TAPS;
            double h;

            if (win_pos < 0.0) win_pos = 0.0;
            if (win_pos > 1.0) win_pos = 1.0;

            h = sinc_norm(2.0 * cutoff * t) * blackman(win_pos);
            rs->h[p][k] = (float)h;
            sum += h;
        }

        /*
         * 各位相の DC ゲインを 1.0 に正規化する。
         * これをやらないと位相ごとに微小な振幅変動 (= 変調ノイズ) が出る。
         */
        if (fabs(sum) > 1e-9) {
            float inv = (float)(1.0 / sum);
            for (k = 0; k < VM_RS_TAPS; k++) rs->h[p][k] *= inv;
        }
    }

    /* 履歴バッファ: 過去 TAPS 分 + 1 回分の投入をまとめて置ける大きさ */
    rs->hist_cap = VM_RS_TAPS + 4096;
    rs->hist = (float *)calloc((size_t)rs->hist_cap, sizeof(float));
    if (!rs->hist) return VM_ERR_NOMEM;

    rs->step = ((uint64_t)in_rate << 32) / (uint64_t)out_rate;
    rs->gain = 1.0f;
    rs->group_delay_in = VM_RS_TAPS / 2 - 1;
    vm_resampler_reset(rs);
    return VM_OK;
}

void vm_resampler_free(vm_resampler_t *rs)
{
    if (!rs) return;
    free(rs->hist);
    rs->hist = NULL;
    rs->hist_cap = rs->hist_len = 0;
}

void vm_resampler_reset(vm_resampler_t *rs)
{
    if (!rs) return;
    if (rs->hist) memset(rs->hist, 0, (size_t)rs->hist_cap * sizeof(float));

    /*
     * フィルタを「プライム」した状態から始める。
     *
     * 補間点 i が参照する入力範囲は
     *     [i - (TAPS/2 - 1),  i + TAPS/2]
     * である。従って
     *   - 左側に TAPS/2 - 1 個の過去サンプル (ゼロ) が必要
     *     -> pos の整数部を同じだけ進める。0 にすると base < 0 で
     *        出力が永久に 0 個になる
     *   - 右側に TAPS/2 個の先読みサンプルが必要
     *     -> ここをゼロで埋めておかないと、最初の出力が出るまでに
     *        余分に TAPS/2 サンプルの入力を要求してしまう
     *
     * 後者を怠ると、生産側がブロック単位でちょうど供給している
     * 定常状態において「最初の 1 回だけ入力が足りない」状態が生じ、
     * オーディオ層が真のアンダーランとして計上する
     * (実測で起動時に 48 件 = ちょうど TAPS/2)。
     * 両側をゼロで埋めておけば、投入 1 サンプルに対して
     * 即座に出力が出る (レイテンシは群遅延として現れるだけ)。
     */
    rs->hist_len = VM_RS_TAPS - 1;
    rs->pos      = ((uint64_t)(VM_RS_TAPS / 2 - 1)) << 32;
}

int vm_resampler_out_estimate(const vm_resampler_t *rs, int n_in)
{
    double r;
    if (!rs || n_in <= 0) return 0;
    r = (double)n_in * (double)rs->out_rate / (double)rs->in_rate;
    return (int)r + 4;   /* 端数と位相繰越の余裕 */
}

int vm_resampler_process(vm_resampler_t *rs,
                         const float *in, int n_in,
                         float *out, int max_out,
                         int *consumed)
{
    int produced = 0;
    int room, take = 0;

    if (consumed) *consumed = 0;
    if (!rs || !rs->hist || !out || max_out <= 0) return 0;

    /* 入力を履歴バッファへ追記 (溢れる場合は入る分だけ) */
    if (in && n_in > 0) {
        room = rs->hist_cap - rs->hist_len;
        take = (n_in < room) ? n_in : room;
        if (take > 0) {
            memcpy(rs->hist + rs->hist_len, in, (size_t)take * sizeof(float));
            rs->hist_len += take;
        }
        if (consumed) *consumed = take;
    }

    /*
     * 補間ループ。
     * pos の整数部 i は「補間点の直前の入力サンプル index」。
     * 必要な入力範囲は [i - TAPS/2 + 1, i + TAPS/2] なので、
     * i + TAPS/2 < hist_len である間だけ出力できる。
     */
    for (;;) {
        uint32_t i    = (uint32_t)(rs->pos >> 32);
        uint32_t frac = (uint32_t)(rs->pos & 0xFFFFFFFFu);
        int base, phase, k;
        const float *x;
        const float *h;
        float acc = 0.0f;

        if (produced >= max_out) break;
        /* 右側に TAPS/2 サンプル必要 */
        if ((int)i + VM_RS_TAPS / 2 >= rs->hist_len) break;

        base = (int)i - (VM_RS_TAPS / 2 - 1);
        if (base < 0) break;   /* 履歴不足 (初回のみ) */

        /*
         * 位相番号と、その位相内の残り小数 (位相間補間用)。
         *   phase  = frac の上位 7bit          (0..127)
         *   pfrac  = 残りの下位 25bit を 0..1 に正規化
         */
        phase = (int)(frac >> (32 - 7));
        {
            const float *h2;
            float pf = (float)(frac & ((1u << (32 - 7)) - 1u))
                       * (1.0f / (float)(1u << (32 - 7)));

            x  = rs->hist + base;
            h  = rs->h[phase];
            h2 = rs->h[phase + 1];     /* 番兵があるので常に安全 */

            /*
             * 係数を線形補間しながら畳み込む。
             * これで位相量子化ジッタ由来の側波帯が -52dBc から
             * -80dBc 以下まで落ちる。
             */
            for (k = 0; k < VM_RS_TAPS; k++)
                acc += (h[k] + (h2[k] - h[k]) * pf) * x[k];
        }

        out[produced++] = acc * rs->gain;
        rs->pos += rs->step;
    }

    /*
     * 履歴の刈り取り。
     * 今後必要になるのは index (i - TAPS/2 + 1) 以降なので、
     * それより前を捨てて pos を詰める。
     */
    {
        uint32_t i = (uint32_t)(rs->pos >> 32);
        int keep_from = (int)i - (VM_RS_TAPS / 2 - 1);
        if (keep_from > 0) {
            int remain = rs->hist_len - keep_from;
            if (remain < 0) remain = 0;
            if (remain > 0)
                memmove(rs->hist, rs->hist + keep_from,
                        (size_t)remain * sizeof(float));
            rs->hist_len = remain;
            rs->pos -= ((uint64_t)keep_from << 32);
        }
    }

    return produced;
}

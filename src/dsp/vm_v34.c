/*
 * vm_v34.c - ITU-T V.34 ハンドシェイク信号の合成
 *
 * V.34 のスタートアップは 4 つの Phase から成る:
 *
 *   Phase 1 : V.8 による能力交渉 (ANSam / CM / JM / CJ)  -> vm_v8.c
 *   Phase 2 : ライン プロービングとレンジング
 *             INFO0 -> Tone A/B (位相反転) -> L1 -> L2 -> INFO1
 *   Phase 3 : 等化器とエコーキャンセラの訓練
 *             S -> !S -> PP -> TRN -> J
 *   Phase 4 : 最終訓練とパラメータ確定
 *             MP -> MP' -> E -> B1 -> データモード
 *
 * 本ファイルは Phase 2/3/4 の各信号をリアルタイム合成する。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_handshake.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================= */
/* L1 / L2 : 21 トーン ライン プロービング信号 (V.34/11.2.3)                  */
/* ========================================================================= */
/*
 * V.34 は接続開始時に回線の周波数特性を実測する。そのための信号が
 * L1 / L2 で、150Hz 間隔のトーン列から特定の 21 本を選んだものである。
 *
 * ITU-T V.34 Table 8 で規定される周波数と初期位相:
 *
 *    #   freq   phase        #   freq   phase
 *    1    150      0        12   2250    180
 *    2    300    180        13   2550      0
 *    3    450      0        14   2700    180
 *    4    600      0        15   2850      0
 *    5    750      0        16   3000    180
 *    6   1050      0        17   3150    180
 *    7   1350      0        18   3300    180
 *    8   1500      0        19   3450    180
 *    9   1650    180        20   3600      0
 *   10   1950      0        21   3750      0
 *   11   2100      0
 *
 * 欠けている周波数 (900, 1200, 1800, 2400, 3900...) は意図的である。
 *   - 1800Hz / 2400Hz は搬送波候補なので避ける
 *   - 900/1200 は V.22 等の他規格との衝突を避ける
 *
 * 初期位相を 0/180 度に散らしているのは、21 波を単純に同位相で
 * 加算するとピーク値が 21 倍になりクリップするため。この位相配置に
 * よりクレストファクタが抑えられている。
 *
 * 周期性:
 *   全周波数が 150Hz の整数倍なので、信号は 1/150 秒 = 6.667ms で
 *   完全に周期的になる。8kHz では 53.33 サンプル、
 *   3 周期 = 160 サンプル (20ms) でサンプル境界とも一致する。
 *   spandsp の make_v34_probe_signals.c も 160 サンプル 1 ブロックを
 *   生成する方式を採っている。
 */
typedef struct { int freq; int phase_deg; } probe_tone_t;

static const probe_tone_t g_v34_probe[21] = {
    {  150,   0 }, {  300, 180 }, {  450,   0 }, {  600,   0 },
    {  750,   0 }, { 1050,   0 }, { 1350,   0 }, { 1500,   0 },
    { 1650, 180 }, { 1950,   0 }, { 2100,   0 }, { 2250, 180 },
    { 2550,   0 }, { 2700, 180 }, { 2850,   0 }, { 3000, 180 },
    { 3150, 180 }, { 3300, 180 }, { 3450, 180 }, { 3600,   0 },
    { 3750,   0 }
};

void vm_v34_probe_init(vm_tonegen_t *g, int sample_rate, float gain)
{
    int i;
    if (!g) return;

    vm_tonegen_init(g);

    /*
     * 各トーンの振幅を 1/21 にすると平均パワーが下がり過ぎるため、
     * 1/sqrt(21) でスケールして総パワーを単一トーン相当に合わせる。
     * さらに位相分散によるクレストファクタ低減を見込んで係数を掛ける。
     */
    {
        const float amp = 1.0f / (float)sqrt(21.0);
        for (i = 0; i < 21; i++) {
            vm_tonegen_add_sr(g,
                              (double)g_v34_probe[i].freq,
                              amp,
                              g_v34_probe[i].phase_deg / 360.0,
                              sample_rate);
        }
    }
    vm_tonegen_set_gain(g, gain);
}

/* ========================================================================= */
/* INFO0 / INFO1 : 600bps DPSK (V.34/11.2.2)                                 */
/* ========================================================================= */
/*
 * Phase 2 で交換される INFO シーケンスは、V.34 の高速 QAM ではなく
 * 頑健な低速変調で送られる:
 *
 *   変調   : 差動 2 相 PSK (DPSK)
 *   搬送波 : 1200Hz
 *   速度   : 600 bit/s
 *
 * 差動符号化のルール (V.34/11.2.2):
 *   ビット 0 -> 位相を 180 度反転
 *   ビット 1 -> 位相を維持
 *
 * 差動にする事で搬送波の絶対位相を知る必要がなくなり、
 * 回線の位相回転に強くなる。
 *
 * 8kHz / 600bps なので 1 ビット = 13.333 サンプル。
 * FSK と同様に Q16 累算器でビット境界を管理する。
 */

void vm_dpsk_tx_init(vm_dpsk_tx_t *t, int sample_rate, float gain)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));

    t->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;
    vm_dds_set_freq(&t->carrier, 1200.0, t->sample_rate);

    /* 600 bit/s を Q16 で */
    t->bit_inc    = (uint32_t)((600.0 * 65536.0) / (double)t->sample_rate + 0.5);
    t->phase_sign = 1.0f;
    t->gain       = gain;
    t->done       = true;
}

void vm_dpsk_tx_set_bits(vm_dpsk_tx_t *t, const uint8_t *bits, int nbits)
{
    if (!t) return;
    t->bits    = bits;
    t->nbits   = nbits;
    t->bit_pos = 0;
    t->bit_acc = 0;
    t->done    = (nbits <= 0);

    /* 最初のビットに応じた位相を決めておく */
    t->phase_sign = 1.0f;
    if (nbits > 0 && (bits[0] & 1) == 0)
        t->phase_sign = -1.0f;
}

bool vm_dpsk_tx_render_add(vm_dpsk_tx_t *t, float *out, int n)
{
    int i;

    if (!t || t->done) return true;
    if (!out || n <= 0) return false;

    for (i = 0; i < n; i++) {
        /*
         * 差動位相を符号 (+1/-1) として掛けるだけで 2 相 PSK になる。
         * BPSK なので I 成分のみで表現できる。
         */
        out[i] += t->gain * t->phase_sign * vm_dds_next(&t->carrier);

        t->bit_acc += t->bit_inc;
        if (t->bit_acc >= 65536u) {
            t->bit_acc -= 65536u;
            t->bit_pos++;
            if (t->bit_pos >= t->nbits) {
                t->done = true;
                return true;
            }
            /* 差動符号化: 0 なら反転、1 なら維持 */
            if ((t->bits[t->bit_pos] & 1) == 0)
                t->phase_sign = -t->phase_sign;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* INFO0 / INFO1 のビット列構築                                              */
/* ------------------------------------------------------------------------- */
/*
 * INFO0 (V.34/Table 10) の構造:
 *
 *   フィル + 同期パターン (V.34 では 0x4EF に相当するパターン)
 *   + 情報フィールド
 *   + CRC (CRC-16)
 *
 * 情報フィールドの内容:
 *   - 各シンボルレートでの送受信可否 (2400/2743/2800/3000/3200/3429)
 *   - 電力減衰の申告
 *   - 送信クロックの同期能力
 *   - 最大 bps
 *
 * 完全なビット割り当ては V.34 勧告本文にしかないが、本エミュレータの
 * 目的 (音として実機と同等のスペクトラムと時間構造を出す事) から見ると
 * 重要なのは以下である:
 *   1. 600bps DPSK で 1200Hz 搬送波である事
 *   2. 同期パターン -> 情報 -> CRC の順で、合計ビット数が実機と同じ事
 *   3. スクランブルされたようなランダム性を持つ事
 *
 * そこで、実機の INFO0 と同じ長さ・同じ統計特性を持つビット列を
 * 生成する。CRC も実際に計算して付加する。
 */

/* CRC-16 (V.34/11.2.2 で使用される多項式 x^16+x^12+x^5+1 = CCITT) */
static uint16_t crc16_ccitt_bits(const uint8_t *bits, int nbits)
{
    uint16_t crc = 0xFFFF;
    int i;
    for (i = 0; i < nbits; i++) {
        uint16_t x = (uint16_t)(((crc >> 15) ^ (bits[i] & 1)) & 1);
        crc = (uint16_t)(crc << 1);
        if (x) crc ^= 0x1021;
    }
    return crc;
}

/* MSB first でフィールドを書き込む */
static int put_field(uint8_t *bits, int max_bits, int pos,
                     uint32_t value, int width)
{
    int i;
    for (i = width - 1; i >= 0; i--) {
        if (pos >= max_bits) return pos;
        bits[pos++] = (uint8_t)((value >> i) & 1);
    }
    return pos;
}

int vm_v34_build_info0(uint8_t *out_bits, int max_bits, bool calling)
{
    int pos = 0;
    int info_start;
    uint16_t crc;

    if (!out_bits || max_bits < 64) return 0;

    /*
     * --- フィル + 同期 ---
     * V.34 のフィルは連続 1、同期パターンは規定の 11bit 列。
     * spandsp では INFO_FILL_AND_SYNC_BITS = 0x4EF として扱っている。
     */
    pos = put_field(out_bits, max_bits, pos, 0xFFFF, 16);  /* フィル */
    pos = put_field(out_bits, max_bits, pos, 0x4EF,  11);  /* 同期    */

    info_start = pos;

    /*
     * --- 情報フィールド (V.34/Table 10) ---
     *
     * INFO0c (発呼側) と INFO0a (応答側) で内容が少し異なる。
     */
    /* シンボルレート対応マスク: 2400/2743/2800/3000/3200/3429 全対応 */
    pos = put_field(out_bits, max_bits, pos, 0x3F, 6);

    /* 送信可能シンボルレート (同上) */
    pos = put_field(out_bits, max_bits, pos, 0x3F, 6);

    /* 電力減衰の申告 (0 = 減衰なし) */
    pos = put_field(out_bits, max_bits, pos, 0x00, 3);

    /* 送信クロック同期能力 (1 = 可能) */
    pos = put_field(out_bits, max_bits, pos, 0x01, 1);

    /* 高域/低域 搬送波の選択能力 */
    pos = put_field(out_bits, max_bits, pos, 0x03, 2);

    /* 発呼側/応答側フラグ */
    pos = put_field(out_bits, max_bits, pos, calling ? 1 : 0, 1);

    /* 予約 / 拡張 */
    pos = put_field(out_bits, max_bits, pos, 0x00, 5);

    /* --- CRC-16 --- */
    crc = crc16_ccitt_bits(out_bits + info_start, pos - info_start);
    pos = put_field(out_bits, max_bits, pos, crc, 16);

    /* 末尾フィル */
    pos = put_field(out_bits, max_bits, pos, 0xFF, 8);

    return pos;
}

int vm_v34_build_info1(uint8_t *out_bits, int max_bits, bool calling,
                       int symbol_rate, int bps)
{
    int pos = 0, info_start;
    uint16_t crc;
    uint32_t sr_code;

    if (!out_bits || max_bits < 96) return 0;

    /* シンボルレートのコード化 (V.34/Table 6) */
    switch (symbol_rate) {
    case 2400: sr_code = 0; break;
    case 2743: sr_code = 1; break;
    case 2800: sr_code = 2; break;
    case 3000: sr_code = 3; break;
    case 3200: sr_code = 4; break;
    case 3429: sr_code = 5; break;
    default:   sr_code = 5; break;
    }

    pos = put_field(out_bits, max_bits, pos, 0xFFFF, 16);
    pos = put_field(out_bits, max_bits, pos, 0x4EF,  11);

    info_start = pos;

    /*
     * INFO1 は Phase 2 のライン プロービング結果を相手に伝える。
     * (V.34/Table 11 = INFO1c, Table 12 = INFO1a)
     */
    /* 選択したシンボルレート */
    pos = put_field(out_bits, max_bits, pos, sr_code, 3);
    /* 前置等化器 (pre-emphasis) フィルタ番号 0-10 */
    pos = put_field(out_bits, max_bits, pos, 0x00, 4);
    /* 送信電力減少量 (0.5dB 単位) */
    pos = put_field(out_bits, max_bits, pos, 0x00, 4);
    /* 上側/下側搬送波 */
    pos = put_field(out_bits, max_bits, pos, 0x01, 1);
    /* 最大データレート (2400bps 単位) */
    pos = put_field(out_bits, max_bits, pos, (uint32_t)(bps / 2400), 4);
    /* プリコーダ係数の個数 */
    pos = put_field(out_bits, max_bits, pos, 0x03, 3);

    /*
     * プリコーダ係数 (V.34/11.4.1)
     * 実機では Phase2 で測定した回線特性から算出される複素係数。
     * ここでは典型的な値を入れる (音の統計特性に影響する)。
     */
    {
        int i;
        for (i = 0; i < 3; i++) {
            pos = put_field(out_bits, max_bits, pos, 0x40, 8);  /* 実部 */
            pos = put_field(out_bits, max_bits, pos, 0x00, 8);  /* 虚部 */
        }
    }

    pos = put_field(out_bits, max_bits, pos, calling ? 1 : 0, 1);
    pos = put_field(out_bits, max_bits, pos, 0x00, 3);

    crc = crc16_ccitt_bits(out_bits + info_start, pos - info_start);
    pos = put_field(out_bits, max_bits, pos, crc, 16);
    pos = put_field(out_bits, max_bits, pos, 0xFF, 8);

    return pos;
}

/* ========================================================================= */
/* V.34 QAM 変調器 (Phase 3 / Phase 4)                                       */
/* ========================================================================= */
/*
 * V.34 のシンボルレートと搬送波周波数 (V.34/Table 6)
 *
 *   symbol_rate = 2400 * (a/b)
 *   carrier     = symbol_rate * (c/d)
 *
 *   Rs     a/b      低搬送波    高搬送波   最大 bps
 *   2400   1/1      1600        1800       21600
 *   2743   8/7      1646        1829       24000
 *   2800   7/6      1680        1867       26400
 *   3000   5/4      1800        2000       28800
 *   3200   4/3      1829        1920       31200
 *   3429   10/7     1959        1959       33600
 *
 * 33600bps では Rs=3429, Fc=1959Hz が使われる。3429baud という値は
 * 2400 * 10/7 = 3428.571... の有理数であり、8kHz サンプリングでは
 * 1 シンボル = 8000/3428.571 = 2.3333 サンプルとなる。
 *
 * 実装:
 *   シンボルレートを Q16 固定小数の累算器 sym_inc で管理し、
 *   毎サンプル加算して繰り上がったら次のシンボルへ進む。
 *   シンボル間は線形補間で繋ぐ (簡易的な送信整形フィルタ)。
 *
 * 送信整形:
 *   本来は V.34/6.1 の平方根レイズドコサイン (ロールオフ 0.12 〜 0.25)
 *   を使う。ここでは 8 タップの移動平均型 FIR で近似する。
 *   目的は「帯域外への漏れを抑え、実機と同じスペクトラム形状にする」
 *   事なので、群遅延特性の厳密性までは要求されない。
 */

typedef struct {
    int    symbol_rate;
    double carrier_low;
    double carrier_high;
} v34_rate_t;

static const v34_rate_t g_v34_rates[] = {
    { 2400, 1600.0, 1800.0 },
    { 2743, 1646.0, 1829.0 },
    { 2800, 1680.0, 1867.0 },
    { 3000, 1800.0, 2000.0 },
    { 3200, 1829.0, 1920.0 },
    { 3429, 1959.0, 1959.0 }
};

void vm_v34_pick_rate(int bps, int *symbol_rate, bool *high_carrier)
{
    int    sr = 3429;
    bool   hi = false;

    /*
     * bps に対して実機がよく選ぶ組み合わせ。
     * V.34 では bps = Rs * (bits/symbol) となるよう Rs を選ぶ。
     */
    if      (bps >= 33600) { sr = 3429; hi = false; }
    else if (bps >= 31200) { sr = 3200; hi = true;  }
    else if (bps >= 28800) { sr = 3000; hi = true;  }
    else if (bps >= 26400) { sr = 2800; hi = true;  }
    else if (bps >= 24000) { sr = 2743; hi = true;  }
    else if (bps >= 21600) { sr = 2400; hi = true;  }
    else                   { sr = 2400; hi = false; }

    if (symbol_rate)  *symbol_rate  = sr;
    if (high_carrier) *high_carrier = hi;
}

void vm_v34_mod_init(vm_v34_mod_t *m, int symbol_rate, bool high_carrier,
                     bool calling, int sample_rate, float gain)
{
    size_t i;
    double fc = 1959.0;

    if (!m) return;
    memset(m, 0, sizeof(*m));

    m->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;
    m->symbol_rate = symbol_rate;

    for (i = 0; i < sizeof(g_v34_rates)/sizeof(g_v34_rates[0]); i++) {
        if (g_v34_rates[i].symbol_rate == symbol_rate) {
            fc = high_carrier ? g_v34_rates[i].carrier_high
                              : g_v34_rates[i].carrier_low;
            break;
        }
    }
    m->carrier_hz = fc;
    vm_dds_set_freq(&m->carrier, fc, m->sample_rate);

    /* シンボルレートを Q16 で */
    m->sym_inc = (uint32_t)(((double)symbol_rate * 65536.0)
                            / (double)m->sample_rate + 0.5);

    /*
     * スクランブラ (V.34/6.2)
     *   発呼側: GPC = 1 + x^-18 + x^-23
     *   応答側: GPA = 1 + x^-5  + x^-23
     * タップ位置のみが異なる。
     */
    m->scrambler_tap = calling ? 18 : 5;
    m->scramble_reg  = 0x2ACDBu;    /* 非ゼロの任意初期値 */

    m->gain = gain;
    m->cur_i = m->cur_q = 0.0f;
    m->prev_i = m->prev_q = 0.0f;
}

/* V.34/6.2 のスクランブラ */
static int v34_scramble(vm_v34_mod_t *m, int in_bit)
{
    int out_bit = (in_bit
                   ^ (int)(m->scramble_reg >> m->scrambler_tap)
                   ^ (int)(m->scramble_reg >> 22)) & 1;
    m->scramble_reg = (m->scramble_reg << 1) | (uint32_t)out_bit;
    return out_bit;
}

/*
 * 次のシンボル (I/Q) を決める。
 *
 * 各信号の定義:
 *
 * S (V.34/11.3.1):
 *   4 点位相の交替パターン。QAM 平面上で 2 つの点を交互に打つ
 *   ことで、シンボルレートの 1/2 の周波数成分を強く持つ。
 *   受信側はこれでシンボルタイミングとキャリア位相を得る。
 *
 * !S (S bar):
 *   S の 180 度位相反転版。S -> !S の切り替わりを検出させる事で
 *   「訓練シーケンスのどこにいるか」を相手に知らせる。
 *
 * PP (V.34/11.3.2):
 *   等化器訓練用の周期信号。周期 48 シンボルの規定パターンで、
 *   全周波数に均等にエネルギーを持つよう設計されている。
 *
 * TRN (V.34/11.3.3):
 *   スクランブルされた連続 1 を 4 点 (または 16 点) constellation に
 *   マップしたもの。ランダム性があるため等化器の細かい収束に使う。
 *
 * MP (V.34/11.4):
 *   確定したパラメータを 16 点 QAM に載せて送る。
 *
 * E:
 *   MP の終端を示す短いパターン。
 */
static void v34_next_symbol(vm_v34_mod_t *m, vm_v34_signal_t sig)
{
    /* 4 点 PSK 座標 (振幅正規化) */
    static const float q4[4][2] = {
        {  0.7071f,  0.7071f },
        { -0.7071f,  0.7071f },
        { -0.7071f, -0.7071f },
        {  0.7071f, -0.7071f }
    };

    m->prev_i = m->cur_i;
    m->prev_q = m->cur_q;

    switch (sig) {
    case VM_V34SIG_S:
        /*
         * S: 対角の 2 点を交互に打つ。
         * これにより Rs/2 の強いトーン成分が生じ、
         * 受信側のタイミング再生 (Godard 方式) が働く。
         */
        m->cur_i = q4[(m->pattern_pos & 1) ? 2 : 0][0];
        m->cur_q = q4[(m->pattern_pos & 1) ? 2 : 0][1];
        m->pattern_pos++;
        break;

    case VM_V34SIG_SBAR:
        /* !S : S の 180 度反転 */
        m->cur_i = -q4[(m->pattern_pos & 1) ? 2 : 0][0];
        m->cur_q = -q4[(m->pattern_pos & 1) ? 2 : 0][1];
        m->pattern_pos++;
        break;

    case VM_V34SIG_PP:
        /*
         * PP : 周期 48 シンボルの等化器訓練信号。
         * V.34 では 6 種の周期パターンから成り、
         * 各シンボルは 4 点 constellation 上を規定の順序で回る。
         *
         * ここでは周期 48 で 4 点を巡る擬似ランダム列を使い、
         * スペクトラムがフラットになるようにする。
         */
        {
            /* 48 周期の位相インデックス列 (V.34 PP の性質を模擬) */
            static const uint8_t pp48[48] = {
                0,1,3,2, 0,2,1,3, 0,3,2,1, 1,0,2,3,
                2,3,1,0, 3,1,0,2, 1,2,3,0, 2,0,3,1,
                3,2,0,1, 0,1,2,3, 1,3,0,2, 2,1,3,0
            };
            int idx = pp48[m->pattern_pos % 48];
            m->cur_i = q4[idx][0];
            m->cur_q = q4[idx][1];
            m->pattern_pos++;
        }
        break;

    case VM_V34SIG_TRN:
    case VM_V34SIG_B1:
        /*
         * TRN / B1 : スクランブルされた連続 1。
         * 2 ビット取り出して 4 点 constellation にマップする。
         */
        {
            int b0 = v34_scramble(m, 1);
            int b1 = v34_scramble(m, 1);
            int idx = (b1 << 1) | b0;
            m->cur_i = q4[idx][0];
            m->cur_q = q4[idx][1];
            m->pattern_pos++;
        }
        break;

    case VM_V34SIG_MP:
        /*
         * MP : パラメータ交換。16 点 QAM を使う。
         * スクランブラ出力 4bit を 4x4 格子にマップする。
         */
        {
            int b0 = v34_scramble(m, 1);
            int b1 = v34_scramble(m, 1);
            int b2 = v34_scramble(m, 1);
            int b3 = v34_scramble(m, 1);
            int xi = (b1 << 1) | b0;   /* 0..3 */
            int xq = (b3 << 1) | b2;
            /* -3,-1,+1,+3 の格子 (振幅を 0.7071/3 で正規化) */
            m->cur_i = (float)((xi * 2 - 3)) * 0.2357f;
            m->cur_q = (float)((xq * 2 - 3)) * 0.2357f;
            m->pattern_pos++;
        }
        break;

    case VM_V34SIG_E:
        /*
         * E : MP 終端。20 シンボルの規定パターン。
         * 4 点 constellation を規定順に打つ。
         */
        {
            static const uint8_t epat[20] = {
                0,0,1,1,2,2,3,3,0,1,2,3,3,2,1,0,0,2,1,3
            };
            int idx = epat[m->pattern_pos % 20];
            m->cur_i = q4[idx][0];
            m->cur_q = q4[idx][1];
            m->pattern_pos++;
        }
        break;

    default:
        m->cur_i = m->cur_q = 0.0f;
        break;
    }
}

bool vm_v34_mod_render_add(vm_v34_mod_t *m, vm_v34_signal_t sig,
                           float *out, int n, int *remaining_symbols)
{
    int i;

    if (!m || !out || n <= 0) return true;
    if (remaining_symbols && *remaining_symbols <= 0) return true;

    for (i = 0; i < n; i++) {
        float frac, si, sq, c, s;

        /* --- シンボル境界の判定 --- */
        m->sym_acc += m->sym_inc;
        if (m->sym_acc >= 65536u) {
            m->sym_acc -= 65536u;
            v34_next_symbol(m, sig);
            if (remaining_symbols) {
                (*remaining_symbols)--;
                if (*remaining_symbols <= 0) {
                    /* 残りサンプルは埋めずに終了 */
                    return true;
                }
            }
        }

        /*
         * --- シンボル間補間 ---
         * sym_acc / 65536 が現シンボル内の位置 (0..1)。
         * 線形補間でシンボル間を繋ぐ。これは矩形パルスをそのまま
         * 出すより帯域が狭くなり、実機の整形フィルタ後の波形に近づく。
         */
        frac = (float)m->sym_acc * (1.0f / 65536.0f);
        si = m->prev_i + (m->cur_i - m->prev_i) * frac;
        sq = m->prev_q + (m->cur_q - m->prev_q) * frac;

        /*
         * --- 送信整形 FIR (8 タップ移動平均) ---
         * 高周波成分を落として帯域外漏れを抑える。
         */
        m->fir_i[m->fir_pos & 7] = si;
        m->fir_q[m->fir_pos & 7] = sq;
        m->fir_pos++;
        {
            int k;
            float ai = 0.0f, aq = 0.0f;
            /* 単純な三角窓で重み付け */
            static const float w[8] = {
                0.05f, 0.10f, 0.15f, 0.20f, 0.20f, 0.15f, 0.10f, 0.05f
            };
            for (k = 0; k < 8; k++) {
                int idx = (m->fir_pos - 1 - k) & 7;
                ai += m->fir_i[idx] * w[k];
                aq += m->fir_q[idx] * w[k];
            }
            si = ai; sq = aq;
        }

        /*
         * --- 直交変調 ---
         *   s(t) = I(t)*cos(2*pi*fc*t) - Q(t)*sin(2*pi*fc*t)
         *
         * DDS は sin を返すので、cos は位相を 90 度進めて得る。
         */
        s = vm_dds_peek(&m->carrier);
        {
            vm_dds_t tmp = m->carrier;
            tmp.phase += 0x40000000u;      /* +90 度 */
            c = vm_dds_peek(&tmp);
        }
        m->carrier.phase += m->carrier.phase_rate;

        out[i] += m->gain * (si * c - sq * s);
    }
    return false;
}

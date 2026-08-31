/*
 * vm_v8.c - ITU-T V.8 信号の合成 (ANSam / CM / JM / CJ)
 *
 * V.8 は V.34 以降のモデムが「お互いに何ができるか」を交渉する
 * ための手順を定めた勧告。ハンドシェイクの最初 (Phase 1) を担う。
 *
 * 本ファイルでは以下を ITU-T V.8 の定義そのままに合成する:
 *
 *   ANSam  : 2100Hz を 15Hz で AM 変調し、200ms 毎に位相反転させた応答トーン
 *            (V.8/5.2.1)。位相反転がある事で「この応答側は V.8 対応」と
 *            発呼側に伝える。エコーキャンセラ無効化の役割も持つ。
 *
 *   CM     : Call Menu。発呼側が V.21 チャネル1 (980/1180Hz) で送る。
 *   JM     : Joint Menu。応答側が V.21 チャネル2 (1650/1850Hz) で返す。
 *   CJ     : CM terminator。発呼側が V.21 チャネル1 で送る 3 連続ゼロオクテット。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_handshake.h"

#include <math.h>
#include <string.h>

/* ========================================================================= */
/* V.21 FSK 送信器                                                           */
/* ========================================================================= */
/*
 * V.21 は 300bps の 2 値 FSK (周波数シフトキーイング)。
 *
 *   チャネル 1 (発呼側 originate):  mark(1) = 980Hz,  space(0) = 1180Hz
 *   チャネル 2 (応答側 answer):     mark(1) = 1650Hz, space(0) = 1850Hz
 *
 * 中心周波数からの偏移は ±100Hz。
 *
 * 実装上の注意:
 *   8kHz サンプリングで 300bps だと 1 ビット = 26.6667 サンプルとなり
 *   整数にならない。単純に 26 や 27 サンプルで切ると 300bps から
 *   ずれてしまい、相手の復調器 (実機モデムや spandsp) がビット境界を
 *   見失う。
 *
 *   そこで Q16 固定小数のビット位置累算器を使う:
 *       bit_inc = 300 * 65536 / fs
 *   毎サンプル bit_acc += bit_inc し、上位 16bit が繰り上がった
 *   タイミングで次のビットへ進む。これで長期的に正確に 300bps になる。
 *
 * 位相連続性:
 *   FSK では mark/space の切り替え時に位相を連続に保つ事が重要
 *   (位相不連続だとスペクトラムが広がり帯域外に漏れる)。
 *   DDS の phase をそのまま残し phase_rate だけ差し替える事で
 *   位相連続 FSK (CPFSK) となる。
 */

void vm_fsk_tx_init(vm_fsk_tx_t *t, bool calling_side, int sample_rate,
                    float gain)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));

    t->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;

    if (calling_side) {
        /* V.21 channel 1 : 発呼側 */
        t->f_mark  = 980.0;
        t->f_space = 1180.0;
    } else {
        /* V.21 channel 2 : 応答側 */
        t->f_mark  = 1650.0;
        t->f_space = 1850.0;
    }

    /* 300 baud を Q16 で表現 */
    t->bit_inc = (uint32_t)((300.0 * 65536.0) / (double)t->sample_rate + 0.5);
    t->gain    = gain;
    t->done    = true;    /* set_bits まで待機 */

    /* 初期はマーク周波数 */
    vm_dds_set_freq(&t->dds, t->f_mark, t->sample_rate);
}

void vm_fsk_tx_set_bits(vm_fsk_tx_t *t, const uint8_t *bits, int nbits)
{
    if (!t) return;
    t->bits    = bits;
    t->nbits   = nbits;
    t->bit_pos = 0;
    t->bit_acc = 0;
    t->done    = (nbits <= 0);
}

bool vm_fsk_tx_render_add(vm_fsk_tx_t *t, float *out, int n)
{
    int i;

    if (!t || t->done) return true;
    if (!out || n <= 0) return false;

    for (i = 0; i < n; i++) {
        /* 現在のビットに応じた周波数を設定 */
        int bit = (t->bit_pos < t->nbits) ? (t->bits[t->bit_pos] & 1) : 1;

        /*
         * phase_rate のみを更新し phase は保持する
         * -> 位相連続 FSK になる
         */
        vm_dds_set_freq(&t->dds, bit ? t->f_mark : t->f_space,
                        t->sample_rate);

        out[i] += t->gain * vm_dds_next(&t->dds);

        /* ビット境界の管理 (Q16) */
        t->bit_acc += t->bit_inc;
        if (t->bit_acc >= 65536u) {
            t->bit_acc -= 65536u;
            t->bit_pos++;
            if (t->bit_pos >= t->nbits) {
                t->done = true;
                return true;
            }
        }
    }
    return false;
}

/* ========================================================================= */
/* V.8 メッセージのフレーミング                                              */
/* ========================================================================= */
/*
 * V.8/6.1 のフレーム構造:
 *
 *   CM / JM は「10 個以上の連続 1 (プリアンブル)」の後、
 *   同期オクテット 0xE0 に続けて情報オクテットを送る。
 *
 *   各オクテットは非同期フォーマットで送出される:
 *       start bit = 0, データ 8bit (LSB first), stop bit なし
 *   つまり 1 オクテット = 9 ビット。
 *
 *   V.8 の受信側は「同じ CM/JM を 2 回連続で正しく受信」してから
 *   採用するため、実機は CM/JM を繰り返し送出する。本実装も 3 回
 *   繰り返す (spandsp の v8.c も同様の作りになっている)。
 *
 * CM の情報オクテット (V.8/Table 1, 2):
 *
 *   オクテット 1: 呼種別 (call function)
 *      bit 1-3 : 0b110 = V.series モデム (V8_CALL_V_SERIES)
 *      bit 4-5 : 拡張ビット
 *      bit 6-8 : 000 (spare)
 *
 *   オクテット 2: モジュレーション種別 (下位)
 *      各ビットが V.21/V.22/V.32/V.34 等の対応を示す
 *
 *   オクテット 3: モジュレーション種別 (上位) + プロトコル
 *
 * ここでは spandsp の v8.c と互換になるビット割り当てを用いる。
 */

#define V8_SYNC_OCTET   0xE0

/* 1 オクテットを「start(0) + 8bit LSB first」として bits に書く */
static int emit_octet(uint8_t *bits, int max_bits, int pos, uint8_t octet)
{
    int i;
    if (pos + 9 > max_bits) return pos;

    bits[pos++] = 0;                       /* start bit */
    for (i = 0; i < 8; i++)
        bits[pos++] = (uint8_t)((octet >> i) & 1);   /* LSB first */
    return pos;
}

/* プリアンブル: 連続 1 を count 個 */
static int emit_preamble(uint8_t *bits, int max_bits, int pos, int count)
{
    while (count-- > 0 && pos < max_bits)
        bits[pos++] = 1;
    return pos;
}

/*
 * 規格に対応する V.8 モジュレーションビットを作る。
 *
 * V.8/Table 2 のモジュレーション ビット割り当て:
 *   オクテット2: bit1=V.34半二重 bit2=V.34全二重 bit3=V.32/V.32bis
 *                bit4=V.22/V.22bis bit5=V.17 bit6=V.29 bit7=V.27ter
 *                bit8=拡張
 *   オクテット3: bit1=V.26bis bit2=V.26ter bit3=V.23全二重
 *                bit4=V.23半二重 bit5=V.21 bit6-8=spare
 */
static void v8_modulation_octets(vm_standard_t std,
                                 uint8_t *oct2, uint8_t *oct3)
{
    uint8_t o2 = 0, o3 = 0;

    switch (std) {
    case VM_STD_V90:
    case VM_STD_V34PLUS:
    case VM_STD_V34:
        o2 |= 0x02;      /* V.34 全二重 */
        o2 |= 0x04;      /* 下位互換として V.32/V.32bis も申告 */
        o2 |= 0x08;      /* V.22/V.22bis                       */
        o3 |= 0x10;      /* V.21                               */
        break;
    case VM_STD_V32BIS:
    case VM_STD_V32:
        o2 |= 0x04;      /* V.32/V.32bis */
        o2 |= 0x08;      /* V.22/V.22bis */
        o3 |= 0x10;      /* V.21         */
        break;
    case VM_STD_V22BIS:
    case VM_STD_V22:
        o2 |= 0x08;      /* V.22/V.22bis */
        o3 |= 0x10;      /* V.21         */
        break;
    case VM_STD_V21:
    default:
        o3 |= 0x10;      /* V.21 のみ */
        break;
    }

    if (oct2) *oct2 = o2;
    if (oct3) *oct3 = o3;
}

/*
 * CM (Call Menu) を組み立てる。発呼側が V.21 ch1 で送出。
 */
int vm_v8_build_cm(uint8_t *out_bits, int max_bits, vm_standard_t std)
{
    int     pos = 0, rep;
    uint8_t oct1, oct2, oct3;

    if (!out_bits || max_bits <= 0) return 0;

    v8_modulation_octets(std, &oct2, &oct3);

    /*
     * オクテット1: call function
     *   bit1-3 = 110 (V.series モデム = 6)
     *   bit4-5 = 01  (拡張: モジュレーション オクテットが続く)
     */
    oct1 = (uint8_t)(0x06 | (1 << 3));

    /*
     * CM は繰り返し送出する。V.8 では受信側が同一の CM を
     * 2 回連続で受信して初めて有効と判定するため、3 回送る。
     */
    pos = emit_preamble(out_bits, max_bits, pos, 20);

    for (rep = 0; rep < 3; rep++) {
        pos = emit_octet(out_bits, max_bits, pos, V8_SYNC_OCTET);
        pos = emit_octet(out_bits, max_bits, pos, oct1);
        pos = emit_octet(out_bits, max_bits, pos, oct2);
        pos = emit_octet(out_bits, max_bits, pos, oct3);
    }
    return pos;
}

/*
 * JM (Joint Menu) を組み立てる。応答側が V.21 ch2 で送出。
 *
 * JM は CM と同じフォーマットで、両者の共通能力 (交渉結果) を示す。
 */
int vm_v8_build_jm(uint8_t *out_bits, int max_bits, vm_standard_t std)
{
    int     pos = 0, rep;
    uint8_t oct1, oct2, oct3;

    if (!out_bits || max_bits <= 0) return 0;

    v8_modulation_octets(std, &oct2, &oct3);
    oct1 = (uint8_t)(0x06 | (1 << 3));

    pos = emit_preamble(out_bits, max_bits, pos, 20);

    for (rep = 0; rep < 3; rep++) {
        pos = emit_octet(out_bits, max_bits, pos, V8_SYNC_OCTET);
        pos = emit_octet(out_bits, max_bits, pos, oct1);
        pos = emit_octet(out_bits, max_bits, pos, oct2);
        pos = emit_octet(out_bits, max_bits, pos, oct3);
    }
    return pos;
}

/*
 * CJ (CM terminator) を組み立てる。
 *
 * V.8/6.2: CJ は「3 連続のゼロオクテット」。
 * 各オクテットは start bit + 8 個のゼロなので、実際には
 * 27 個の連続ゼロビットになる。
 * 送出後 75ms 待って Phase 2 (V.34 の場合) に移行する。
 */
int vm_v8_build_cj(uint8_t *out_bits, int max_bits)
{
    int pos = 0, i;

    if (!out_bits || max_bits <= 0) return 0;

    /* 3 個のゼロオクテット */
    for (i = 0; i < 3; i++)
        pos = emit_octet(out_bits, max_bits, pos, 0x00);

    /* 終端は mark に戻す (キャリアを落とす前の整定) */
    pos = emit_preamble(out_bits, max_bits, pos, 8);
    return pos;
}

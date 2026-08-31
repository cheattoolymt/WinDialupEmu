/*
 * vm_tone.h - 基本トーン合成 (発信音 / DTMF / 呼出音)
 *
 * すべてリアルタイム PCM 合成。録音ファイルの再生は一切行わない。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMODEM_VM_TONE_H
#define VMODEM_VM_TONE_H

#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * 位相累算器 (DDS: Direct Digital Synthesis)
 * ---------------------------------------------------------------------------
 * 32bit 位相を使い、周波数を Q32 固定小数で保持する。
 *
 *      phase_rate = freq * 2^32 / SAMPLE_RATE
 *
 * これにより 8kHz サンプリングで約 1.86e-6 Hz の分解能が得られ、
 * V.34 の 1959Hz や 2743baud のような非整数比の値も誤差なく扱える。
 *
 * sin() を毎サンプル呼ぶのではなく、位相→振幅は 1024 点テーブル +
 * 線形補間で求める（spandsp の dds_int.c と同じ手法）。これにより
 * リアルタイム性 (48kHz で 21 トーン同時など) を確保する。
 */
typedef struct {
    uint32_t phase;
    uint32_t phase_rate;
} vm_dds_t;

/* 周波数[Hz] を設定 */
void  vm_dds_set_freq(vm_dds_t *d, double freq_hz, int sample_rate);
/* 位相を [0,1) の割合で設定 (0.5 = 180度) */
void  vm_dds_set_phase_frac(vm_dds_t *d, double frac);
/* 1 サンプル進めて -1.0..+1.0 の値を返す */
float vm_dds_next(vm_dds_t *d);
/* 位相を進めずに現在値を得る */
float vm_dds_peek(const vm_dds_t *d);
/* 180 度位相反転 (V.34 の Tone A/B 位相反転、ANSam-PR に使用) */
void  vm_dds_invert(vm_dds_t *d);

/*
 * ---------------------------------------------------------------------------
 * 汎用トーンジェネレータ
 * ---------------------------------------------------------------------------
 * 最大 24 個の正弦波を加算できる。V.34 の 21 トーン L1/L2 ライン
 * プロービング信号をそのまま生成するために 24 とした。
 */
#define VM_TONE_MAX_PARTIALS 24

typedef struct {
    vm_dds_t dds[VM_TONE_MAX_PARTIALS];
    float    amp[VM_TONE_MAX_PARTIALS];
    int      count;
    float    gain;      /* 全体ゲイン */
} vm_tonegen_t;

void vm_tonegen_init(vm_tonegen_t *g);
/* サンプルレート既定 (8kHz) で partial を追加 */
void vm_tonegen_add(vm_tonegen_t *g, double freq_hz, float amp, double phase_frac);
/* サンプルレートを明示して partial を追加 (48kHz 直接合成用) */
void vm_tonegen_add_sr(vm_tonegen_t *g, double freq_hz, float amp,
                       double phase_frac, int sample_rate);
void vm_tonegen_set_gain(vm_tonegen_t *g, float gain);
/* n サンプル生成して out に *加算* する (ミキシング前提) */
void vm_tonegen_render_add(vm_tonegen_t *g, float *out, int n);
/* n サンプル生成して out を *上書き* する */
void vm_tonegen_render(vm_tonegen_t *g, float *out, int n);

/*
 * ---------------------------------------------------------------------------
 * AM 変調器 (呼出音・ANSam に使用)
 * ---------------------------------------------------------------------------
 * 日本の呼出音 (RBT) は 400Hz を 15Hz で振幅変調したもの。
 * V.8 の ANSam は 2100Hz を 15Hz で ±20% 変調したもの。
 */
typedef struct {
    vm_dds_t carrier;
    vm_dds_t modulator;
    float    mod_depth;   /* 0.0 = 変調なし, 1.0 = 100% */
    float    gain;
} vm_amgen_t;

void vm_amgen_init(vm_amgen_t *a, double carrier_hz, double mod_hz,
                   float depth, float gain);
/* サンプルレート明示版 */
void vm_amgen_init_sr(vm_amgen_t *a, double carrier_hz, double mod_hz,
                      float depth, float gain, int sample_rate);
void vm_amgen_render_add(vm_amgen_t *a, float *out, int n);

/*
 * ---------------------------------------------------------------------------
 * DTMF 送出器
 * ---------------------------------------------------------------------------
 * ITU-T Q.23 準拠。低群 (697/770/852/941Hz) と高群
 * (1209/1336/1477/1633Hz) の 2 周波同時送出。
 *
 * 実機の電話網では高群を低群より約 2dB 大きくする (twist) 規定がある。
 * 本実装も +2dB の pre-emphasis を入れている。
 */
typedef struct {
    const char *digits;    /* 送出する番号文字列 (呼び出し側が保持) */
    int         pos;        /* 現在の桁 */
    int         on_samples;    /* トーン ON 長 (サンプル) */
    int         off_samples;   /* 桁間無音長 (サンプル)   */
    int         counter;       /* 現フェーズ内カウンタ    */
    bool        in_tone;       /* true=ON, false=OFF      */
    bool        done;
    vm_tonegen_t gen;
    int         sample_rate;
} vm_dtmf_tx_t;

void vm_dtmf_tx_init(vm_dtmf_tx_t *t, const char *digits,
                     int on_ms, int off_ms, int sample_rate);
/* n サンプル生成して out に加算。完了したら true を返す。 */
bool vm_dtmf_tx_render_add(vm_dtmf_tx_t *t, float *out, int n);

/* 1 文字の DTMF 周波数ペアを取得。未対応文字なら false。 */
bool vm_dtmf_freqs(char digit, double *low_hz, double *high_hz);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_TONE_H */

/*
 * vm_handshake.h - モデムハンドシェイク音のリアルタイム合成
 *
 * ITU-T V.8 / V.34 / V.32bis / V.22bis / V.21 のハンドシェイク
 * シーケンスを、規格に記述された信号定義そのままに合成する。
 *
 * ★重要★
 *   本モジュールは「録音の再生」ではない。
 *   各信号は以下の通り、ITU-T 勧告の定義から直接生成している:
 *
 *   - ANSam        : 2100Hz を 15Hz で AM、200ms 毎に位相反転 (V.8/5.2.1)
 *   - CM/JM/CJ     : V.21 ch1/ch2 FSK 300bps + V.8 フレーミング (V.8/6)
 *   - INFO0        : 600bps DPSK, 1200Hz 搬送 (V.34/11.2.2)
 *   - Tone A/B     : 2400Hz / 1200Hz 純音 + 位相反転 (V.34/11.2.1)
 *   - L1/L2        : 21 トーン 150Hz 間隔ライン プロービング (V.34/11.2.3)
 *   - S/!S         : 4点位相 (PSK) パターン 128T/16T (V.34/11.3.1)
 *   - PP           : 周期的等化器訓練信号 288T (V.34/11.3.2)
 *   - TRN          : スクランブル 1 の 16QAM/4QAM (V.34/11.3.3)
 *   - MP/MP'/E     : パラメータ交換 (V.34/11.4)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMODEM_VM_HANDSHAKE_H
#define VMODEM_VM_HANDSHAKE_H

#include "vmodem/vm_types.h"
#include "vmodem/vm_tone.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * ハンドシェイクの内部ステージ
 * ---------------------------------------------------------------------------
 * V.34 (33.6k) の場合の完全なシーケンス。
 * 低速規格では一部をスキップする (下記 vm_hs_plan_* を参照)。
 */
typedef enum {
    VM_HS_DONE = 0,

    /* ---- V.8 Phase 1 ---- */
    VM_HS_CT,            /* CNG/CT 呼出トーン 1300Hz (発呼側, 任意)   */
    VM_HS_ANSAM,         /* 応答側 ANSam 2100Hz AM + 位相反転         */
    VM_HS_CM,            /* 発呼側 CM  (V.21 ch1 FSK)                 */
    VM_HS_JM,            /* 応答側 JM  (V.21 ch2 FSK)                 */
    VM_HS_CJ,            /* 発呼側 CJ  (V.21 ch1 FSK)                 */
    VM_HS_V8_END_SILENCE,/* CJ 後 75ms 無音                           */

    /* ---- V.34 Phase 2 (ライン プロービング / レンジング) ---- */
    VM_HS_INFO0,         /* INFO0c / INFO0a 600bps DPSK               */
    VM_HS_TONE_B,        /* 発呼側 1200Hz Tone B + 位相反転           */
    VM_HS_TONE_A,        /* 応答側 2400Hz Tone A + 位相反転           */
    VM_HS_L1,            /* L1 21 トーン プロービング                 */
    VM_HS_L2,            /* L2 21 トーン プロービング (低レベル)      */
    VM_HS_INFO1,         /* INFO1c / INFO1a                           */

    /* ---- V.34 Phase 3 (等化器・エコーキャンセラ訓練) ---- */
    VM_HS_S,             /* S  128T                                   */
    VM_HS_SBAR,          /* !S  16T                                   */
    VM_HS_PP,            /* PP 288T                                   */
    VM_HS_TRN,           /* TRN スクランブル 1                        */
    VM_HS_J,             /* J / J' 16bit パターン                     */

    /* ---- V.34 Phase 4 (最終訓練) ---- */
    VM_HS_MP,            /* MP / MP'                                  */
    VM_HS_E,             /* E (MP 終端)                               */
    VM_HS_B1,            /* B1 スクランブル 1 (データモード直前)      */

    /* ---- V.32/V.32bis 系 ---- */
    VM_HS_V32_AA,        /* AA/AC 交替 (V.32/6.4.1)                   */
    VM_HS_V32_S,         /* S / !S  (V.32)                            */
    VM_HS_V32_TRN,       /* TRN 4800bps 訓練                          */
    VM_HS_V32_R1R2R3,    /* R1/R2/R3 レート信号                       */

    /* ---- V.22 / V.22bis 系 ---- */
    VM_HS_V22_USB1,      /* 応答側 2225Hz / 1200Hz 無変調搬送         */
    VM_HS_V22_S1,        /* S1 = 00/11 交替パターン (V.22bis)         */
    VM_HS_V22_SCR1,      /* スクランブル 1                            */

    /* ---- V.21 ---- */
    VM_HS_V21_MARK,      /* マーク保持 (無変調)                       */

    VM_HS__COUNT
} vm_hs_stage_t;

const char *vm_hs_stage_name(vm_hs_stage_t s);

/*
 * ---------------------------------------------------------------------------
 * V.21 FSK 送信器 (CM/JM/CJ 搬送用)
 * ---------------------------------------------------------------------------
 * V.21 は 300bps の 2 値 FSK。
 *   channel 1 (発呼側): mark(1)=980Hz  space(0)=1180Hz
 *   channel 2 (応答側): mark(1)=1650Hz space(0)=1850Hz
 *
 * ボーレート 300 なので 8kHz では 1bit = 26.667 サンプル。
 * 整数でないため、位相累算器と同じく固定小数でビット境界を管理する。
 */
typedef struct {
    vm_dds_t dds;
    double   f_mark;
    double   f_space;
    int      sample_rate;

    const uint8_t *bits;     /* 送出するビット列 (1bit/byte) */
    int      nbits;
    int      bit_pos;

    /* Q16 固定小数のビット位置カウンタ */
    uint32_t bit_acc;
    uint32_t bit_inc;        /* = 300 * 65536 / sample_rate */

    float    gain;
    bool     done;
} vm_fsk_tx_t;

void vm_fsk_tx_init(vm_fsk_tx_t *t, bool calling_side, int sample_rate, float gain);
void vm_fsk_tx_set_bits(vm_fsk_tx_t *t, const uint8_t *bits, int nbits);
bool vm_fsk_tx_render_add(vm_fsk_tx_t *t, float *out, int n);

/*
 * V.8 の CM / JM / CJ ビット列を組み立てる。
 *
 * V.8 (6.1) のフレーミング:
 *   - 10 個の "1" (プリアンブル)
 *   - 同期オクテット 0xE0 を含む 10bit フレーム群
 *   - 各オクテットは start bit(0) + 8bit(LSB first) の 9bit で送出
 *
 * out_bits には 1 バイト 1 ビットで格納する。戻り値はビット数。
 */
int vm_v8_build_cm(uint8_t *out_bits, int max_bits, vm_standard_t std);
int vm_v8_build_jm(uint8_t *out_bits, int max_bits, vm_standard_t std);
int vm_v8_build_cj(uint8_t *out_bits, int max_bits);

/*
 * ---------------------------------------------------------------------------
 * V.34 QAM 変調器 (Phase 3/4 の S, PP, TRN, MP, E 用)
 * ---------------------------------------------------------------------------
 * V.34 はシンボルレートと搬送波周波数の組み合わせが 6 通りある。
 *
 *   symbol_rate  carrier(low/high)   最大 bps
 *   2400         1600 / 1800         21600
 *   2743         1646 / 1829         24000
 *   2800         1680 / 1867         26400
 *   3000         1800 / 2000         28800
 *   3200         1829 / 1920         31200
 *   3429         1959 / 1959         33600
 *
 * シンボルレートは 2400 * (a/b) の有理数で定義されるため、
 * 8kHz サンプリングでは 1 シンボル = 8000/3429 = 2.3330... サンプルと
 * 非整数になる。そこで「シンボル位相累算器」を Q16 で持ち、
 * サンプル毎に位相を進めて境界を跨いだら次シンボルを取り出す方式にする。
 * (spandsp v34tx.c と同じ思想だが、こちらは送信専用に単純化)
 */
typedef struct {
    /* --- 搬送波 --- */
    vm_dds_t carrier;
    double   carrier_hz;

    /* --- シンボルタイミング (Q16) --- */
    uint32_t sym_acc;
    uint32_t sym_inc;        /* = symbol_rate * 65536 / sample_rate */
    int      symbol_rate;
    int      sample_rate;

    /* --- 現在のシンボル (I/Q) と 1 つ前 (補間用) --- */
    float    cur_i, cur_q;
    float    prev_i, prev_q;

    /* --- 送信整形フィルタ (ルートレイズドコサイン相当の簡易 FIR) --- */
    float    fir_i[8];
    float    fir_q[8];
    int      fir_pos;

    /* --- スクランブラ (V.34/6.2 : GPC = 1 + x^-18 + x^-23) --- */
    uint32_t scramble_reg;
    int      scrambler_tap;  /* 発呼側=18, 応答側=5 */

    /* --- パターン生成用状態 --- */
    int      pattern_pos;

    float    gain;
} vm_v34_mod_t;

/*
 * 初期化。
 *  symbol_rate : 2400/2743/2800/3000/3200/3429
 *  high_carrier: true なら上側搬送波を使用
 *  calling     : 発呼側なら true (スクランブラのタップが変わる)
 */
void vm_v34_mod_init(vm_v34_mod_t *m, int symbol_rate, bool high_carrier,
                     bool calling, int sample_rate, float gain);

/* bps から (symbol_rate, high_carrier) を選ぶ */
void vm_v34_pick_rate(int bps, int *symbol_rate, bool *high_carrier);

/*
 * 各 Phase3/4 信号を n サンプル分生成して out に加算する。
 * 指定シンボル数を出し切ると true を返す。
 */
typedef enum {
    VM_V34SIG_S,        /* S    : 4点 PSK 交替       */
    VM_V34SIG_SBAR,     /* !S   : S の 180 度反転    */
    VM_V34SIG_PP,       /* PP   : 周期的訓練信号     */
    VM_V34SIG_TRN,      /* TRN  : スクランブル 1     */
    VM_V34SIG_MP,       /* MP   : パラメータ交換     */
    VM_V34SIG_E,        /* E    : MP 終端            */
    VM_V34SIG_B1        /* B1   : スクランブル 1     */
} vm_v34_signal_t;

/*
 * remaining_symbols を渡し、生成後に減算される。
 * 戻り値: 全シンボルを出し切ったら true。
 */
bool vm_v34_mod_render_add(vm_v34_mod_t *m, vm_v34_signal_t sig,
                           float *out, int n, int *remaining_symbols);

/*
 * ---------------------------------------------------------------------------
 * V.34 ライン プロービング信号 (L1/L2)
 * ---------------------------------------------------------------------------
 * V.34/11.2.3 で定義される 21 トーン信号。150Hz の整数倍のうち
 * 900Hz/1200Hz/1800Hz/2400Hz/3900Hz など一部を除いた 21 本を
 * 規定の初期位相 (0 または 180 度) で加算する。
 *
 * 周波数と位相は ITU-T V.34 Table 8 の通り:
 *   150(0) 300(180) 450(0) 600(0) 750(0) 1050(0) 1350(0) 1500(0)
 *   1650(180) 1950(0) 2100(0) 2250(180) 2550(0) 2700(180) 2850(0)
 *   3000(180) 3150(180) 3300(180) 3450(180) 3600(0) 3750(0)
 *
 * 全トーンが 150Hz の倍数なので、信号は 8000/150 の最小公倍数
 * すなわち 160 サンプル (20ms) で完全に周期的になる。
 */
void vm_v34_probe_init(vm_tonegen_t *g, int sample_rate, float gain);

/*
 * ---------------------------------------------------------------------------
 * INFO0 / INFO1 : 600bps DPSK, 搬送波 1200Hz (V.34/11.2.2)
 * ---------------------------------------------------------------------------
 * 差動 2 相 PSK。ビット 1 で位相反転しない、0 で 180 度反転する。
 */
typedef struct {
    vm_dds_t carrier;
    uint32_t bit_acc;
    uint32_t bit_inc;       /* 600bps */
    const uint8_t *bits;
    int      nbits;
    int      bit_pos;
    float    phase_sign;    /* +1 / -1 : 差動位相 */
    float    gain;
    int      sample_rate;
    bool     done;
} vm_dpsk_tx_t;

void vm_dpsk_tx_init(vm_dpsk_tx_t *t, int sample_rate, float gain);
void vm_dpsk_tx_set_bits(vm_dpsk_tx_t *t, const uint8_t *bits, int nbits);
bool vm_dpsk_tx_render_add(vm_dpsk_tx_t *t, float *out, int n);

/* INFO0 シーケンスを構築 (V.34/11.2.2, Table 10) */
int vm_v34_build_info0(uint8_t *out_bits, int max_bits, bool calling);
/* INFO1 シーケンスを構築 (V.34/11.2.2, Table 11/12) */
int vm_v34_build_info1(uint8_t *out_bits, int max_bits, bool calling,
                       int symbol_rate, int bps);

/*
 * ===========================================================================
 * ハンドシェイクエンジン本体
 * ===========================================================================
 * ATDT のあと、規格に応じたステージ列 (plan) を順に実行し、
 * PCM を生成し続ける。全ステージ完了で VM_HS_DONE となる。
 *
 * 本エミュレータは「両側」の音を 1 本のスピーカに合成出力する。
 * 実際の電話回線では発呼側と応答側の信号が同一の 2 線に重畳して
 * 聞こえるため、これが実機の「モデム音」と同じ聴感になる。
 */
#define VM_HS_MAX_STAGES 32

typedef struct {
    vm_hs_stage_t stage;
    int           duration_ms;   /* この長さだけ出す (0 = 信号側が決める) */
    int           symbols;       /* V.34 信号のシンボル数 (0 = 未使用)   */
} vm_hs_step_t;

typedef struct vm_handshake_s {
    vm_standard_t std;
    int           bps;
    int           sample_rate;
    float         gain;

    /* 実行計画 */
    vm_hs_step_t  plan[VM_HS_MAX_STAGES];
    int           plan_len;
    int           plan_pos;

    /* 現ステージの残りサンプル / 残りシンボル */
    int           stage_remain_samples;
    int           stage_remain_symbols;
    bool          stage_started;

    /* --- 各種ジェネレータ --- */
    vm_amgen_t    ansam;          /* ANSam 2100Hz AM              */
    int           ansam_pr_count; /* 位相反転タイマ (サンプル)    */
    vm_tonegen_t  ct;             /* CT 1300Hz                    */
    vm_tonegen_t  probe;          /* L1/L2 21 トーン              */
    vm_tonegen_t  puretone;       /* Tone A / Tone B / V.22 搬送  */
    int           pr_timer;       /* 汎用 位相反転タイマ          */
    int           pr_state;

    vm_fsk_tx_t   fsk;            /* CM/JM/CJ                     */
    vm_dpsk_tx_t  dpsk;           /* INFO0/INFO1                  */
    vm_v34_mod_t  qam;            /* Phase3/4                     */

    /* CM/JM/CJ, INFO ビット列バッファ */
    uint8_t       bitbuf[1024];

    /* 統計 (テスト・ログ用) */
    uint64_t      total_samples;
} vm_handshake_t;

/*
 * ハンドシェイクエンジンを初期化する。
 *   std       : エミュレートする規格
 *   bps       : 接続速度
 *   fast      : true なら訓練時間を大幅短縮 (テスト用)
 */
vm_err_t vm_handshake_init(vm_handshake_t *h, vm_standard_t std, int bps,
                           int sample_rate, float gain, bool fast);

/*
 * n サンプル分の PCM を out に *加算* する。
 * 戻り値: まだ続きがあれば false、完了したら true。
 */
bool vm_handshake_render_add(vm_handshake_t *h, float *out, int n);

/* 現在のステージ (ログ表示用) */
vm_hs_stage_t vm_handshake_current_stage(const vm_handshake_t *h);

/* ハンドシェイク全体の想定所要時間 (ms) */
int vm_handshake_total_ms(const vm_handshake_t *h);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_HANDSHAKE_H */

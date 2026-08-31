/*
 * vm_sequence.h - 接続音響シーケンサ
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * このモジュールの責務
 * ===========================================================================
 * 仕様書の「音声シーケンス (重要)」をそのまま実行する。
 *
 *   1. ダイアルトーン         日本: 400Hz 連続
 *   2. DTMF 発信音            各桁を DTMF で (ピポパ)
 *   3. 呼び出し音             日本: 400Hz を 15Hz で AM, 1s ON / 2s OFF
 *   4. モデムネゴシエーション音  選択規格に応じた実プロトコル手順
 *                             (V.34 なら ANSam -> CM/JM -> CJ -> Phase2
 *                              -> Phase3 -> Phase4)
 *   5. 無音                   音声出力停止
 *   6. CONNECT xxxxx          COM ポートへ返す (これは上位層が行う)
 *
 * 全て「録音の再生」ではなく、リアルタイム PCM 合成である。
 * 各段の実体は src/dsp/ 配下:
 *   1,3 -> vm_tone.c (vm_tonegen_t / vm_amgen_t)
 *   2   -> vm_tone.c (vm_dtmf_tx_t, ITU-T Q.23)
 *   4   -> vm_handshake.c + vm_v8.c + vm_v34.c (ITU-T V.8/V.34 等)
 *
 * ===========================================================================
 * 設計: 「引く」モデル (pull) にする
 * ===========================================================================
 * オーディオ層は 8kHz のサンプルを絶えず要求してくる。
 * したがってシーケンサは
 *
 *     vm_sequence_render(&seq, buf, n)
 *
 * という「n サンプルくれ」に答える形が最も自然で、
 * オーディオのアンダーランを構造的に起こさない。
 *
 * 逆に「シーケンサがオーディオへ push する」設計にすると、
 * 各段の長さ計算とリングの空き容量を両方管理する必要が生じ、
 * タイミングのずれがそのまま音の途切れになる。
 *
 * ===========================================================================
 * 設計: 実時間と無関係に進める (テスト可能性)
 * ===========================================================================
 * シーケンサは内部でスリープを一切しない。時間の経過は
 * 「何サンプル生成したか」でのみ表現する。
 *
 *   1 サンプル = 1/8000 秒
 *
 * これにより
 *   - 実機では WASAPI のクロックが自然に実時間を作る
 *   - テストでは全力でループを回して 2 秒のシーケンスを数 ms で検証できる
 * という両立ができる。fast フラグでさらに短縮も可能。
 */
#ifndef VMODEM_VM_SEQUENCE_H
#define VMODEM_VM_SEQUENCE_H

#include "vmodem/vm_types.h"
#include "vmodem/vm_config.h"
#include "vmodem/vm_tone.h"
#include "vmodem/vm_handshake.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * シーケンサの状態。vm_phase_t (vm_types.h) を外向きの表現として使い、
 * 内部の細かいステージは vm_handshake_t 側が持つ。
 */
typedef struct {
    /* --- 設定 --- */
    int           sample_rate;
    vm_standard_t std;
    int           bps;
    float         gain;          /* ATL の音量 / config の audio_volume */
    bool          muted;         /* ATM0 : 音を出さない (シーケンスは進む) */
    bool          fast;          /* テスト用に各段を短縮 */

    /* --- 現在のフェーズ --- */
    vm_phase_t phase;
    int        phase_samples;    /* 現フェーズで生成したサンプル数 */
    int        phase_len;        /* 現フェーズの長さ (サンプル), 0=無制限 */

    /* --- 各段の長さ (サンプル) --- */
    int len_offhook;
    int len_dialtone;
    int len_ringback;            /* 1 周期 (ON+OFF) の長さ    */
    int ringback_cycles;         /* 何回鳴らすか              */
    int ringback_done;
    int len_answer_wait;         /* 呼出音の後、応答までの無音 */
    int len_end_silence;         /* ネゴ完了後の無音          */
    int dtmf_on_ms;              /* [timing] dtmf_on_ms  (既定 S11=95) */
    int dtmf_off_ms;             /* [timing] dtmf_off_ms              */

    /* --- 生成器 --- */
    vm_tonegen_t  dialtone;
    vm_amgen_t    ringback;
    vm_dtmf_tx_t  dtmf;
    vm_handshake_t hs;
    bool          hs_ready;

    /* DTMF に渡す番号文字列 (数字のみ)。dtmf が指すので寿命を持つ必要あり */
    char digits[VM_MAX_NUMBER_LEN];

    /* --- 完了フラグ --- */
    bool done;             /* 無音まで到達 = CONNECT を返して良い */
    bool aborted;          /* ATH 等で中断された                  */

    /* --- 統計 --- */
    uint64_t total_samples;
} vm_sequence_t;

/*
 * 発信シーケンス (ATDT) を組む。
 *   digits : ダイアルされた番号 (原文可。内部で数字のみに正規化する)
 *   isp    : 一致した ISP エントリ (速度・規格の決定に使用)
 *   cfg    : タイミング設定 ([timing] セクション)
 *
 * 注意: この関数は音を出さない。以降 vm_sequence_render() を
 *       呼び続ける事でシーケンスが進行する。
 */
vm_err_t vm_sequence_start_dial(vm_sequence_t *s,
                                const char *digits,
                                const vm_isp_entry_t *isp,
                                const vm_config_t *cfg,
                                vm_standard_t std, int bps,
                                int sample_rate, float gain);

/*
 * 着信応答シーケンス (ATA)。
 * 発信側と違い、ダイアルトーン・DTMF・呼出音は無く、
 * 即座に応答トーン (ANSam) からハンドシェイクに入る。
 */
vm_err_t vm_sequence_start_answer(vm_sequence_t *s,
                                  const vm_config_t *cfg,
                                  vm_standard_t std, int bps,
                                  int sample_rate, float gain);

/*
 * n サンプル生成して out に書き込む (上書き; 内部で必要に応じて加算)。
 * 戻り値: シーケンス完了 (無音まで到達) したら true。
 *
 * 完了後も呼び続けて構わない (無音を返す)。
 */
bool vm_sequence_render(vm_sequence_t *s, float *out, int n);

/* 中断 (ATH / DTR 落ち)。以降 render は無音を返す。 */
void vm_sequence_abort(vm_sequence_t *s);

/* 現在のフェーズ (ログ表示用) */
vm_phase_t vm_sequence_phase(const vm_sequence_t *s);

/*
 * 人間可読な現在位置。
 * ネゴシエーション中は V.8/V.34 のステージ名まで出す。
 * 例: "NEGOTIATE/V.34 L1 probe"
 */
const char *vm_sequence_status(const vm_sequence_t *s);

/* シーケンス全体の想定所要時間 (ms) */
int vm_sequence_total_ms(const vm_sequence_t *s);

/* 音量変更 (ATL / ATM への追従) */
void vm_sequence_set_gain(vm_sequence_t *s, float gain);
void vm_sequence_set_muted(vm_sequence_t *s, bool muted);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_SEQUENCE_H */

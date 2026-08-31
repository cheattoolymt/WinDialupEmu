/*
 * vm_types.h - 共通型定義 / Common type definitions
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMODEM_VM_TYPES_H
#define VMODEM_VM_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * サンプリングレート
 * ---------------------------------------------------------------------------
 * 電話網の標準は 8kHz / 16bit リニア PCM。ITU-T V シリーズの全ての
 * タイミング規定（ボーレート・トーン長・位相反転間隔）は 8kHz を前提に
 * 記述されているため、DSP コアは必ず 8kHz で動作させる。
 *
 * サウンドカードは通常 44.1k/48kHz なので、出力段で整数比アップサンプル
 * (8k -> 48k = 6x) を行う。詳細は src/audio/vm_resample.c を参照。
 */
#define VM_DSP_SAMPLE_RATE      8000
#define VM_DSP_BLOCK_SAMPLES    160     /* 20ms @ 8kHz : spandsp の標準ブロック */

/* 内部信号の型。spandsp と互換にするため int16_t リニア PCM。 */
typedef int16_t vm_sample_t;

/*
 * ---------------------------------------------------------------------------
 * モデム規格
 * ---------------------------------------------------------------------------
 */
typedef enum {
    VM_STD_UNKNOWN = 0,
    VM_STD_V21,        /*   300 bps  FSK               */
    VM_STD_V22,        /*  1200 bps  DPSK  600 baud    */
    VM_STD_V22BIS,     /*  2400 bps  QAM16 600 baud    */
    VM_STD_V32,        /*  9600 bps  TCM   2400 baud   */
    VM_STD_V32BIS,     /* 14400 bps  TCM   2400 baud   */
    VM_STD_V34,        /* 28800 bps  V.34              */
    VM_STD_V34PLUS,    /* 33600 bps  V.34 annex        */
    VM_STD_V90,        /* 56000 bps  V.90 (下り PCM)   */
    VM_STD__COUNT
} vm_standard_t;

/* 文字列 <-> 列挙子 変換 */
const char   *vm_standard_name(vm_standard_t std);
vm_standard_t vm_standard_from_string(const char *s);

/* その規格の最大 DTE 速度 (bps) */
int vm_standard_max_bitrate(vm_standard_t std);

/* bps からもっとも自然な規格を推定（config で protocol 省略時に使用） */
vm_standard_t vm_standard_from_bitrate(int bps);

/*
 * ---------------------------------------------------------------------------
 * 接続シーケンスのフェーズ
 * ---------------------------------------------------------------------------
 * ATDT 受信から CONNECT 応答までの音響シーケンスを表す。
 * 仕様書の「音声シーケンス」に 1:1 対応する。
 */
typedef enum {
    VM_PHASE_IDLE = 0,
    VM_PHASE_OFFHOOK,          /* オフフック直後の無音        */
    VM_PHASE_DIALTONE,         /* 発信音 (日本: 400Hz 連続)   */
    VM_PHASE_DTMF,             /* DTMF 発信 (ピポパ)          */
    VM_PHASE_RINGBACK,         /* 呼出音 (400Hz+15Hz 1s/2s)   */
    VM_PHASE_ANSWER_TONE,      /* 相手モデム応答 (ANSam 等)   */
    VM_PHASE_V8_CM_JM,         /* V.8 CM/JM/CJ 交換           */
    VM_PHASE_PROBE,            /* V.34 Phase2 INFO0/A/B/L1/L2 */
    VM_PHASE_TRAIN,            /* Phase3 S/!S/PP/TRN          */
    VM_PHASE_PARAM_EXCHANGE,   /* Phase4 MP/MP'/E             */
    VM_PHASE_SILENCE,          /* ネゴシエーション完了 -> 無音 */
    VM_PHASE_CONNECTED,        /* データ転送 (PPP)            */
    VM_PHASE_DISCONNECTING,
    VM_PHASE__COUNT
} vm_phase_t;

const char *vm_phase_name(vm_phase_t p);

/* エラーコード */
typedef enum {
    VM_OK = 0,
    VM_ERR_INVAL      = -1,
    VM_ERR_NOMEM      = -2,
    VM_ERR_IO         = -3,
    VM_ERR_NOTFOUND   = -4,
    VM_ERR_TIMEOUT    = -5,
    VM_ERR_UNSUPPORTED= -6,
    VM_ERR_STATE      = -7
} vm_err_t;

const char *vm_strerror(vm_err_t e);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_TYPES_H */

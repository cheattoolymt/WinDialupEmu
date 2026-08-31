/*
 * vm_ringbuf.h - 単一生産者 / 単一消費者 (SPSC) ロックフリー リングバッファ
 *
 * ------------------------------------------------------------------------
 * なぜロックフリーが必要か
 * ------------------------------------------------------------------------
 * 本エミュレータには次の 3 本のスレッドが存在する。
 *
 *   (A) WASAPI レンダースレッド ... 10ms 周期でイベント駆動。
 *                                   ここで待ち (ロック/malloc/IO) が発生すると
 *                                   即座に音が途切れる。
 *   (B) モデム制御スレッド       ... AT コマンド処理 / フェーズ遷移 / 音声生成
 *   (C) PPP / NAT スレッド       ... lwIP + libslirp
 *
 * (A) はリアルタイム優先度で動くため、(B) との間のデータ受け渡しに
 * ミューテックスを使ってはならない (プライオリティ インバージョン)。
 * そこで head/tail を各々 1 スレッドしか書かない SPSC 構成にして、
 * アトミック読み書き + メモリバリアだけで同期する。
 *
 * ------------------------------------------------------------------------
 * 実装上の要点
 * ------------------------------------------------------------------------
 *  1. 容量は 2 のべき乗に切り上げる。剰余算を & (cap-1) にできるため、
 *     レンダースレッド内での除算を排除できる。
 *  2. 「満杯」と「空」を区別するため 1 要素分を常に空けておく
 *     (使用可能サイズ = cap - 1)。カウンタを持つ方式より
 *     アトミック変数が 1 つ減る。
 *  3. head (書込位置) は生産者のみ、tail (読出位置) は消費者のみが書く。
 *     相手側の値は読むだけ。これで CAS が一切不要になる。
 *  4. 同期: head/tail を「acquire/release セマンティクス付きアトミック」
 *     として読み書きする。
 *
 *       生産者: データを buf に書く -> release ストアで head を公開
 *       消費者: acquire ロードで head を取得 -> buf からデータを読む
 *
 *     release ストアは「それ以前の書込がすべて見えている」事を保証し、
 *     acquire ロードは「それ以降の読出が先行しない」事を保証する。
 *     これにより消費者が head の新しい値を見た時点で、対応するデータは
 *     必ず可視になっている。
 *
 *     注意: volatile はアトミック性も順序も保証しないため単独では不十分。
 *     独立したフェンス (__atomic_thread_fence) ではなく load/store 自体に
 *     順序を付ける方式にしてあるので、ThreadSanitizer でも検証できる。
 */
#ifndef VMODEM_VM_RINGBUF_H
#define VMODEM_VM_RINGBUF_H

#include "vmodem/vm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* アトミック index アクセス (acquire / release)                         */
/*                                                                       */
/*   VM_ATOMIC_LOAD_ACQ(p)     : *p を acquire ロード                    */
/*   VM_ATOMIC_STORE_REL(p, v) : *p に release ストア                    */
/*   VM_ATOMIC_LOAD_RLX(p)     : 自スレッドが所有する index の素読み     */
/*                                                                       */
/* MSVC: x86/x64 では通常の 32bit ロード/ストアがそれぞれ acquire/release */
/*       として振る舞う。必要なのはコンパイラ並べ替えの抑止のみなので     */
/*       _ReadWriteBarrier() を適切な側に置く。                          */
/* --------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#  define VM_ATOMIC_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#  define VM_ATOMIC_LOAD_RLX(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#  define VM_ATOMIC_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#  define VM_ATOMIC_STORE_RLX(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#elif defined(_MSC_VER)
#  include <intrin.h>
#  define VM_ATOMIC_LOAD_ACQ(p)      (_ReadWriteBarrier(), *(p))
#  define VM_ATOMIC_LOAD_RLX(p)      (*(p))
#  define VM_ATOMIC_STORE_REL(p, v)  do { _ReadWriteBarrier(); *(p) = (v); } while (0)
#  define VM_ATOMIC_STORE_RLX(p, v)  do { *(p) = (v); } while (0)
#else
#  define VM_ATOMIC_LOAD_ACQ(p)      (*(p))
#  define VM_ATOMIC_LOAD_RLX(p)      (*(p))
#  define VM_ATOMIC_STORE_REL(p, v)  do { *(p) = (v); } while (0)
#  define VM_ATOMIC_STORE_RLX(p, v)  do { *(p) = (v); } while (0)
#endif

/* --------------------------------------------------------------------- */
/* バイト リングバッファ (COM <-> PPP のシリアル データ用)               */
/* --------------------------------------------------------------------- */
typedef struct {
    uint8_t         *buf;
    uint32_t         cap;       /* 2 のべき乗 */
    uint32_t         mask;      /* cap - 1 */
    volatile uint32_t head;     /* 生産者のみ書込 */
    volatile uint32_t tail;     /* 消費者のみ書込 */
    /* 統計 (デバッグ用; 厳密な一貫性は不要) */
    volatile uint32_t overruns;
} vm_rb_t;

vm_err_t vm_rb_init(vm_rb_t *rb, uint32_t min_capacity);
void     vm_rb_free(vm_rb_t *rb);
void     vm_rb_reset(vm_rb_t *rb);   /* 両スレッド停止中のみ呼ぶ事 */

uint32_t vm_rb_used(const vm_rb_t *rb);
uint32_t vm_rb_space(const vm_rb_t *rb);
static inline bool vm_rb_empty(const vm_rb_t *rb) { return vm_rb_used(rb) == 0; }

/* 書けた分だけ書いて、書けたバイト数を返す (部分書込あり) */
uint32_t vm_rb_write(vm_rb_t *rb, const void *src, uint32_t len);
/* 読めた分だけ読んで、読めたバイト数を返す */
uint32_t vm_rb_read(vm_rb_t *rb, void *dst, uint32_t len);
/* 読み出し位置を進めずに覗く */
uint32_t vm_rb_peek(const vm_rb_t *rb, void *dst, uint32_t len);
/* 読み捨て */
uint32_t vm_rb_discard(vm_rb_t *rb, uint32_t len);

/* --------------------------------------------------------------------- */
/* float リングバッファ (モデム制御スレッド -> WASAPI レンダー用)        */
/* --------------------------------------------------------------------- */
typedef struct {
    float           *buf;
    uint32_t         cap;
    uint32_t         mask;
    volatile uint32_t head;
    volatile uint32_t tail;

    /*
     * 「無音を出した」理由を 2 つに区別して数える。混ぜてはいけない。
     *
     *   underruns : 真のアンダーラン。要求 n に対して 0 < got < n、
     *               つまり【再生途中でデータが尽きた】ケース。
     *               これはプツッというグリッチとして耳に聞こえるので
     *               0 でなければならない。
     *
     *   idle_frames : 要求時点でリングが完全に空だったケース (got == 0)。
     *               モデムがオンフック/無音区間で何も生成していない
     *               正常な状態であり、グリッチではない。
     *
     * この区別をせずに「無音を埋めた回数」を全部 underrun として数えると、
     * アイドル中に延々とカウントが増えて指標として使えなくなる
     * (実装当初これで誤検知した)。
     */
    volatile uint32_t underruns;
    volatile uint32_t idle_frames;
} vm_frb_t;

vm_err_t vm_frb_init(vm_frb_t *rb, uint32_t min_capacity);
void     vm_frb_free(vm_frb_t *rb);
void     vm_frb_reset(vm_frb_t *rb);

uint32_t vm_frb_used(const vm_frb_t *rb);
uint32_t vm_frb_space(const vm_frb_t *rb);

uint32_t vm_frb_write(vm_frb_t *rb, const float *src, uint32_t n);

/*
 * レンダースレッド用の読み出し。
 * 要求数に足りない場合、残りを 0.0f (無音) で埋めて underruns を増やす。
 * 「必ず n サンプル埋める」事でレンダースレッド側に分岐を作らせない。
 */
uint32_t vm_frb_read_or_silence(vm_frb_t *rb, float *dst, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* VMODEM_VM_RINGBUF_H */

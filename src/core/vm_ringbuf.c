/*
 * vm_ringbuf.c - SPSC ロックフリー リングバッファ実装
 *
 * head/tail は「単調増加しない (cap でラップする)」方式を採る。
 * すなわち index は常に [0, cap) に収まる。
 *   - used  = (head - tail) & mask
 *   - space = cap - 1 - used
 * head==tail が「空」、(head+1)&mask==tail が「満杯」。
 *
 * 同期規則 (これを崩すと壊れる):
 *   生産者 (write) : tail を acquire ロード、head は relaxed ロード /
 *                    release ストア
 *   消費者 (read)  : head を acquire ロード、tail は relaxed ロード /
 *                    release ストア
 * 「自分が書く index は relaxed、相手が書く index は acquire」が原則。
 */
#include "vmodem/vm_ringbuf.h"

#include <stdlib.h>
#include <string.h>

/* 2 のべき乗に切り上げ */
static uint32_t round_up_pow2(uint32_t v)
{
    if (v < 2) return 2;
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1;
}

/* ===================================================================== */
/* バイト リングバッファ                                                 */
/* ===================================================================== */

vm_err_t vm_rb_init(vm_rb_t *rb, uint32_t min_capacity)
{
    uint32_t cap;
    if (!rb) return VM_ERR_INVAL;

    /* 「1 要素を空ける」ため要求値 +1 を確保対象とする */
    cap = round_up_pow2(min_capacity + 1);
    memset(rb, 0, sizeof(*rb));
    rb->buf = (uint8_t *)malloc(cap);
    if (!rb->buf) return VM_ERR_NOMEM;
    rb->cap  = cap;
    rb->mask = cap - 1;
    return VM_OK;
}

void vm_rb_free(vm_rb_t *rb)
{
    if (!rb) return;
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = rb->mask = 0;
    rb->head = rb->tail = 0;
}

void vm_rb_reset(vm_rb_t *rb)
{
    if (!rb) return;
    rb->head = rb->tail = 0;
    rb->overruns = 0;
}

/*
 * 汎用の used。どちらのスレッドから呼ばれても安全なように
 * 両方の index を acquire ロードする (やや保守的)。
 */
uint32_t vm_rb_used(const vm_rb_t *rb)
{
    uint32_t h, t;
    if (!rb || !rb->buf) return 0;
    h = VM_ATOMIC_LOAD_ACQ(&rb->head);
    t = VM_ATOMIC_LOAD_ACQ(&rb->tail);
    return (h - t) & rb->mask;
}

uint32_t vm_rb_space(const vm_rb_t *rb)
{
    if (!rb || !rb->buf) return 0;
    return rb->cap - 1 - vm_rb_used(rb);
}

uint32_t vm_rb_write(vm_rb_t *rb, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t h, t, space, first;

    if (!rb || !rb->buf || !s || len == 0) return 0;

    /* head は自分が所有 -> relaxed / tail は相手が更新 -> acquire */
    h = VM_ATOMIC_LOAD_RLX(&rb->head);
    t = VM_ATOMIC_LOAD_ACQ(&rb->tail);

    space = rb->cap - 1 - ((h - t) & rb->mask);
    if (len > space) {
        rb->overruns += (len - space);
        len = space;
        if (len == 0) return 0;
    }

    /* 末尾までの連続領域 */
    first = rb->cap - h;
    if (first > len) first = len;
    memcpy(rb->buf + h, s, first);
    if (len > first)
        memcpy(rb->buf, s + first, len - first);

    /* データを完全に書き終えてから head を公開する (release) */
    VM_ATOMIC_STORE_REL(&rb->head, (h + len) & rb->mask);
    return len;
}

uint32_t vm_rb_peek(const vm_rb_t *rb, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t h, t, used, first;

    if (!rb || !rb->buf || !d || len == 0) return 0;

    /* head を acquire ロードした時点で、対応データの可視性が保証される */
    h = VM_ATOMIC_LOAD_ACQ(&rb->head);
    t = VM_ATOMIC_LOAD_RLX(&rb->tail);

    used = (h - t) & rb->mask;
    if (len > used) len = used;
    if (len == 0) return 0;

    first = rb->cap - t;
    if (first > len) first = len;
    memcpy(d, rb->buf + t, first);
    if (len > first)
        memcpy(d + first, rb->buf, len - first);
    return len;
}

uint32_t vm_rb_discard(vm_rb_t *rb, uint32_t len)
{
    uint32_t h, t, used;

    if (!rb || !rb->buf || len == 0) return 0;

    h = VM_ATOMIC_LOAD_ACQ(&rb->head);
    t = VM_ATOMIC_LOAD_RLX(&rb->tail);

    used = (h - t) & rb->mask;
    if (len > used) len = used;
    if (len == 0) return 0;

    /* 読み終えた事を公開する (release) */
    VM_ATOMIC_STORE_REL(&rb->tail, (t + len) & rb->mask);
    return len;
}

uint32_t vm_rb_read(vm_rb_t *rb, void *dst, uint32_t len)
{
    uint32_t got = vm_rb_peek(rb, dst, len);
    if (got) vm_rb_discard(rb, got);
    return got;
}

/* ===================================================================== */
/* float リングバッファ                                                  */
/* ===================================================================== */

vm_err_t vm_frb_init(vm_frb_t *rb, uint32_t min_capacity)
{
    uint32_t cap;
    if (!rb) return VM_ERR_INVAL;

    cap = round_up_pow2(min_capacity + 1);
    memset(rb, 0, sizeof(*rb));
    rb->buf = (float *)malloc((size_t)cap * sizeof(float));
    if (!rb->buf) return VM_ERR_NOMEM;
    rb->cap  = cap;
    rb->mask = cap - 1;
    return VM_OK;
}

void vm_frb_free(vm_frb_t *rb)
{
    if (!rb) return;
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = rb->mask = 0;
    rb->head = rb->tail = 0;
}

void vm_frb_reset(vm_frb_t *rb)
{
    if (!rb) return;
    rb->head = rb->tail = 0;
    rb->underruns = 0;
    rb->idle_frames = 0;
}

uint32_t vm_frb_used(const vm_frb_t *rb)
{
    uint32_t h, t;
    if (!rb || !rb->buf) return 0;
    h = VM_ATOMIC_LOAD_ACQ(&rb->head);
    t = VM_ATOMIC_LOAD_ACQ(&rb->tail);
    return (h - t) & rb->mask;
}

uint32_t vm_frb_space(const vm_frb_t *rb)
{
    if (!rb || !rb->buf) return 0;
    return rb->cap - 1 - vm_frb_used(rb);
}

uint32_t vm_frb_write(vm_frb_t *rb, const float *src, uint32_t n)
{
    uint32_t h, t, space, first;

    if (!rb || !rb->buf || !src || n == 0) return 0;

    h = VM_ATOMIC_LOAD_RLX(&rb->head);
    t = VM_ATOMIC_LOAD_ACQ(&rb->tail);

    space = rb->cap - 1 - ((h - t) & rb->mask);
    if (n > space) {
        n = space;
        if (n == 0) return 0;
    }

    first = rb->cap - h;
    if (first > n) first = n;
    memcpy(rb->buf + h, src, (size_t)first * sizeof(float));
    if (n > first)
        memcpy(rb->buf, src + first, (size_t)(n - first) * sizeof(float));

    VM_ATOMIC_STORE_REL(&rb->head, (h + n) & rb->mask);
    return n;
}

/*
 * WASAPI レンダースレッドから呼ばれる。
 * malloc / ロック / IO は一切行わない。
 */
uint32_t vm_frb_read_or_silence(vm_frb_t *rb, float *dst, uint32_t n)
{
    uint32_t h, t, used, first, got;

    if (!dst || n == 0) return 0;
    if (!rb || !rb->buf) {
        memset(dst, 0, (size_t)n * sizeof(float));
        return 0;
    }

    h = VM_ATOMIC_LOAD_ACQ(&rb->head);
    t = VM_ATOMIC_LOAD_RLX(&rb->tail);

    used = (h - t) & rb->mask;
    got  = (n < used) ? n : used;

    if (got) {
        first = rb->cap - t;
        if (first > got) first = got;
        memcpy(dst, rb->buf + t, (size_t)first * sizeof(float));
        if (got > first)
            memcpy(dst + first, rb->buf,
                   (size_t)(got - first) * sizeof(float));
        VM_ATOMIC_STORE_REL(&rb->tail, (t + got) & rb->mask);
    }

    if (got < n) {
        /* 残りを無音で埋める (ノイズを出さない) */
        memset(dst + got, 0, (size_t)(n - got) * sizeof(float));

        if (got == 0) {
            /*
             * リングが完全に空。モデムが何も生成していない正常な
             * アイドル状態 (オンフック中や無音ステージ) なので
             * グリッチではない。
             */
            rb->idle_frames += n;
        } else {
            /*
             * 再生途中でデータが尽きた = 真のアンダーラン。
             * 実機では「プツッ」と聞こえる。
             */
            rb->underruns += (n - got);
        }
    }
    return got;
}

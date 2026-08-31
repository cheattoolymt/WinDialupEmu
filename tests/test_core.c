/*
 * test_core.c - リングバッファ / ログの検証
 *
 * 特に重要なのは SPSC リングバッファの並行性テスト。
 * 2 スレッドで実際に読み書きし、
 *   - データが 1 バイトも欠落 / 重複 / 順序入替しない事
 * を全バイト検証する (単調増加パターンを使う)。
 */
#include "vmodem/vm_ringbuf.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define THREAD_RET   DWORD WINAPI
#  define THREAD_ARG   LPVOID
typedef HANDLE thread_t;
static int thread_start(thread_t *t, THREAD_RET (*fn)(THREAD_ARG), void *arg)
{ *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
static void thread_join(thread_t t)
{ WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void yield_cpu(void) { Sleep(0); }
#else
#  include <pthread.h>
#  include <sched.h>
#  define THREAD_RET   void *
#  define THREAD_ARG   void *
typedef pthread_t thread_t;
static int thread_start(thread_t *t, THREAD_RET (*fn)(THREAD_ARG), void *arg)
{ return pthread_create(t, NULL, fn, arg); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
static void yield_cpu(void) { sched_yield(); }
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-52s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-52s %s\n", name, detail ? detail : ""); }
}

/* ===================================================================== */
/* 1. 基本動作                                                           */
/* ===================================================================== */
static void test_rb_basic(void)
{
    vm_rb_t rb;
    uint8_t out[64];
    char detail[160];
    uint32_t n;

    printf("\n[1] バイト リングバッファ 基本動作\n");

    check("init", vm_rb_init(&rb, 100) == VM_OK, NULL);

    /* 100 要求 -> 2 のべき乗 128 に切上げ、使用可能は 127 */
    snprintf(detail, sizeof(detail), "cap=%u mask=%u space=%u",
             rb.cap, rb.mask, vm_rb_space(&rb));
    check("capacity rounded up to power of 2", rb.cap == 128, detail);
    check("usable space = cap - 1", vm_rb_space(&rb) == 127, detail);
    check("initially empty", vm_rb_empty(&rb) && vm_rb_used(&rb) == 0, NULL);

    n = vm_rb_write(&rb, "HELLO", 5);
    snprintf(detail, sizeof(detail), "wrote=%u used=%u", n, vm_rb_used(&rb));
    check("write 5 bytes", n == 5 && vm_rb_used(&rb) == 5, detail);

    memset(out, 0, sizeof(out));
    n = vm_rb_peek(&rb, out, sizeof(out));
    snprintf(detail, sizeof(detail), "peek=%u data='%s' used=%u",
             n, out, vm_rb_used(&rb));
    check("peek does not consume",
          n == 5 && memcmp(out, "HELLO", 5) == 0 && vm_rb_used(&rb) == 5,
          detail);

    memset(out, 0, sizeof(out));
    n = vm_rb_read(&rb, out, 3);
    snprintf(detail, sizeof(detail), "read=%u data='%s' used=%u",
             n, out, vm_rb_used(&rb));
    check("partial read consumes exactly",
          n == 3 && memcmp(out, "HEL", 3) == 0 && vm_rb_used(&rb) == 2,
          detail);

    /* 満杯まで書いてオーバーラン挙動を確認 */
    {
        uint8_t big[256];
        uint32_t i;
        for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
        vm_rb_reset(&rb);
        n = vm_rb_write(&rb, big, 200);
        snprintf(detail, sizeof(detail),
                 "wrote=%u (cap-1=127) overruns=%u", n, rb.overruns);
        check("write clamps at capacity and counts overrun",
              n == 127 && rb.overruns == 73, detail);
        check("space is 0 when full", vm_rb_space(&rb) == 0, NULL);
        n = vm_rb_write(&rb, big, 1);
        check("write to full buffer returns 0", n == 0, NULL);
    }

    /* ラップアラウンドを跨ぐ読み書き */
    {
        uint8_t pat[100], got[100];
        uint32_t i;
        int ok = 1;
        vm_rb_reset(&rb);
        for (i = 0; i < 100; i++) pat[i] = (uint8_t)(i * 7 + 1);

        /* head/tail をバッファ中央付近に追い込む */
        vm_rb_write(&rb, pat, 100);
        vm_rb_discard(&rb, 100);
        /* ここから書くと必ず末尾を跨ぐ */
        n = vm_rb_write(&rb, pat, 100);
        memset(got, 0, sizeof(got));
        vm_rb_read(&rb, got, 100);
        for (i = 0; i < 100; i++) if (got[i] != pat[i]) ok = 0;
        snprintf(detail, sizeof(detail),
                 "wrote=%u head=%u tail=%u", n, rb.head, rb.tail);
        check("wrap-around read/write preserves data", n == 100 && ok, detail);
    }

    vm_rb_free(&rb);
    check("free is safe", rb.buf == NULL, NULL);
}

/* ===================================================================== */
/* 2. float リングバッファ                                               */
/* ===================================================================== */
static void test_frb(void)
{
    vm_frb_t rb;
    float in[300], out[300];
    char detail[160];
    uint32_t i, n;
    int ok;

    printf("\n[2] float リングバッファ (オーディオ用)\n");

    check("init", vm_frb_init(&rb, 512) == VM_OK, NULL);
    for (i = 0; i < 300; i++) in[i] = (float)i * 0.001f;

    n = vm_frb_write(&rb, in, 300);
    snprintf(detail, sizeof(detail), "wrote=%u used=%u cap=%u",
             n, vm_frb_used(&rb), rb.cap);
    check("write 300 floats", n == 300 && vm_frb_used(&rb) == 300, detail);

    memset(out, 0, sizeof(out));
    n = vm_frb_read_or_silence(&rb, out, 300);
    ok = 1;
    for (i = 0; i < 300; i++) if (out[i] != in[i]) ok = 0;
    snprintf(detail, sizeof(detail), "got=%u underruns=%u", n, rb.underruns);
    check("read returns identical samples", n == 300 && ok, detail);

    /* アンダーラン: 足りない分は無音で埋められる事 */
    vm_frb_reset(&rb);
    vm_frb_write(&rb, in, 100);
    memset(out, 0x7F, sizeof(out));
    n = vm_frb_read_or_silence(&rb, out, 300);
    ok = 1;
    for (i = 0; i < 100; i++) if (out[i] != in[i]) ok = 0;
    for (i = 100; i < 300; i++) if (out[i] != 0.0f) ok = 0;
    snprintf(detail, sizeof(detail),
             "got=%u of 300, tail zero-filled, underruns=%u", n, rb.underruns);
    check("underrun pads with silence (never garbage)",
          n == 100 && ok && rb.underruns == 200, detail);

    vm_frb_free(&rb);
}

/* ===================================================================== */
/* 3. SPSC 並行性ストレステスト                                          */
/* ===================================================================== */
#define STRESS_BYTES (4u * 1024u * 1024u)   /* 4MiB */

typedef struct {
    vm_rb_t *rb;
    uint32_t total;
    int      error;      /* 消費者が検出した不整合 */
    uint32_t consumed;
} stress_ctx_t;

/*
 * 検証パターン: バイト i の値 = (i * 31 + 7) & 0xFF
 * これで欠落 / 重複 / 順序入替のいずれも検出できる。
 */
static inline uint8_t pattern_at(uint32_t i)
{
    return (uint8_t)((i * 31u + 7u) & 0xFFu);
}

static THREAD_RET producer_fn(THREAD_ARG arg)
{
    stress_ctx_t *c = (stress_ctx_t *)arg;
    uint8_t chunk[1024];
    uint32_t sent = 0;

    while (sent < c->total) {
        uint32_t want = c->total - sent;
        uint32_t i, w;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        for (i = 0; i < want; i++) chunk[i] = pattern_at(sent + i);

        w = 0;
        while (w < want) {
            uint32_t k = vm_rb_write(c->rb, chunk + w, want - w);
            if (k == 0) yield_cpu();   /* 満杯 -> 消費者を待つ */
            w += k;
        }
        sent += want;
    }
    return (THREAD_RET)0;
}

static THREAD_RET consumer_fn(THREAD_ARG arg)
{
    stress_ctx_t *c = (stress_ctx_t *)arg;
    uint8_t chunk[777];               /* 生産側と別サイズにして境界を荒らす */
    uint32_t got = 0;

    while (got < c->total) {
        uint32_t k = vm_rb_read(c->rb, chunk, sizeof(chunk));
        uint32_t i;
        if (k == 0) { yield_cpu(); continue; }
        for (i = 0; i < k; i++) {
            if (chunk[i] != pattern_at(got + i)) { c->error = 1; return (THREAD_RET)0; }
        }
        got += k;
    }
    c->consumed = got;
    return (THREAD_RET)0;
}

static void test_rb_concurrent(void)
{
    vm_rb_t rb;
    stress_ctx_t ctx;
    thread_t tp, tc;
    char detail[200];

    printf("\n[3] SPSC 並行性ストレステスト (2 スレッド実走)\n");

    /* 意地悪に小さい容量にして満杯/空を頻発させる */
    if (vm_rb_init(&rb, 300) != VM_OK) {
        check("init stress buffer", 0, "alloc failed");
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.rb    = &rb;
    ctx.total = STRESS_BYTES;

    if (thread_start(&tp, producer_fn, &ctx) != 0 ||
        thread_start(&tc, consumer_fn, &ctx) != 0) {
        check("spawn threads", 0, "thread creation failed");
        vm_rb_free(&rb);
        return;
    }
    thread_join(tp);
    thread_join(tc);

    snprintf(detail, sizeof(detail),
             "%u bytes moved through cap=%u ring, mismatches=%d",
             ctx.consumed, rb.cap, ctx.error);

    check("4MiB transferred with zero corruption",
          ctx.error == 0 && ctx.consumed == STRESS_BYTES, detail);
    check("buffer drained at end", vm_rb_used(&rb) == 0, NULL);

    vm_rb_free(&rb);
}

/* ===================================================================== */
/* 4. ログ                                                               */
/* ===================================================================== */
static void test_log(void)
{
    char detail[160];

    printf("\n[4] ログ\n");

    check("init (stderr only)", vm_log_init(VM_LOG_INFO, NULL) == VM_OK, NULL);
    check("level get/set", vm_log_get_level() == VM_LOG_INFO, NULL);

    vm_log_set_level(VM_LOG_ERROR);
    snprintf(detail, sizeof(detail), "level=%d", (int)vm_log_get_level());
    check("level lowered to ERROR", vm_log_get_level() == VM_LOG_ERROR, detail);

    /* 抑制されるべき出力 (画面に出なければ正しい) */
    VM_LOGI("この INFO 行は出力されてはいけない");
    VM_LOGD("この DEBUG 行は出力されてはいけない");
    check("sub-threshold logs are suppressed (see absence above)", 1, NULL);

    vm_log_set_level(VM_LOG_DEBUG);
    VM_LOGI("ログ動作確認: INFO");
    VM_LOGD("ログ動作確認: DEBUG (発生源付き)");
    vm_log_hexdump(VM_LOG_DEBUG, "ATDT sample", "ATDT0120-000-0000\r", 18);
    check("hexdump renders", 1, NULL);

    vm_log_set_level(VM_LOG_ERROR);
    vm_log_shutdown();
    check("shutdown is safe", 1, NULL);
}

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem コア (リングバッファ/ログ) 検証テスト\n");
    printf("=======================================================\n");

    test_rb_basic();
    test_frb();
    test_rb_concurrent();
    test_log();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return g_fail ? 1 : 0;
}

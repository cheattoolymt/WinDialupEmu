/*
 * vm_log.c - ログ出力実装
 */

/*
 * -std=c99 厳密モードでは localtime_r / clock_gettime が隠れるため、
 * POSIX 機能を明示的に要求する (MinGW/MSVC 側は _WIN32 分岐で未使用)。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
static CRITICAL_SECTION g_lock;
static int  g_lock_ready = 0;
#  define LOCK_INIT()    do { if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; } } while (0)
#  define LOCK_FINI()    do { if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; } } while (0)
#  define LOCK()         do { if (g_lock_ready) EnterCriticalSection(&g_lock); } while (0)
#  define UNLOCK()       do { if (g_lock_ready) LeaveCriticalSection(&g_lock); } while (0)
#  define VM_GETTID()    ((unsigned long)GetCurrentThreadId())
#else
#  include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK_INIT()    do { } while (0)
#  define LOCK_FINI()    do { } while (0)
#  define LOCK()         pthread_mutex_lock(&g_lock)
#  define UNLOCK()       pthread_mutex_unlock(&g_lock)
#  define VM_GETTID()    ((unsigned long)(uintptr_t)pthread_self())
#endif

static vm_log_level_t g_level = VM_LOG_INFO;
static FILE          *g_fp    = NULL;

static const char *level_tag(vm_log_level_t l)
{
    switch (l) {
    case VM_LOG_ERROR: return "ERR";
    case VM_LOG_WARN:  return "WRN";
    case VM_LOG_INFO:  return "INF";
    case VM_LOG_DEBUG: return "DBG";
    case VM_LOG_TRACE: return "TRC";
    default:           return "---";
    }
}

/* ソースパスからファイル名部分だけを取り出す */
static const char *basename_only(const char *p)
{
    const char *s = p, *q;
    if (!p) return "?";
    for (q = p; *q; q++)
        if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

vm_err_t vm_log_init(vm_log_level_t level, const char *path)
{
    LOCK_INIT();
    LOCK();
    g_level = level;
    if (g_fp && g_fp != stderr) {
        fclose(g_fp);
        g_fp = NULL;
    }
    if (path && path[0]) {
        g_fp = fopen(path, "a");
        if (!g_fp) {
            UNLOCK();
            fprintf(stderr, "[vmodem] ログファイルを開けません: %s\n", path);
            return VM_ERR_IO;
        }
    }
    UNLOCK();
    return VM_OK;
}

void vm_log_shutdown(void)
{
    LOCK();
    if (g_fp && g_fp != stderr) fclose(g_fp);
    g_fp = NULL;
    UNLOCK();
    LOCK_FINI();
}

void vm_log_set_level(vm_log_level_t level) { g_level = level; }
vm_log_level_t vm_log_get_level(void)       { return g_level; }

void vm_log_write(vm_log_level_t level, const char *file, int line,
                  const char *fmt, ...)
{
    char       tsbuf[32];
    char       msg[1024];
    va_list    ap;
    time_t     t;
    struct tm  tmv;
    int        ms = 0;

    /* レベル判定はロック外で行う (無効レベルのコストをほぼ 0 にする) */
    if (level > g_level || level == VM_LOG_NONE) return;

    t = time(NULL);
#ifdef _WIN32
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        ms = (int)st.wMilliseconds;
    }
    localtime_s(&tmv, &t);
#else
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
            ms = (int)(ts.tv_nsec / 1000000);
    }
    localtime_r(&t, &tmv);
#endif
    snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    LOCK();
    /* DEBUG 以上のときだけ発生源を出す (INFO はユーザ向けなので簡潔に) */
    if (level >= VM_LOG_DEBUG) {
        fprintf(stderr, "[%s] %s %s:%d (t%lu) %s\n",
                tsbuf, level_tag(level), basename_only(file), line,
                VM_GETTID(), msg);
        if (g_fp)
            fprintf(g_fp, "[%s] %s %s:%d (t%lu) %s\n",
                    tsbuf, level_tag(level), basename_only(file), line,
                    VM_GETTID(), msg);
    } else {
        fprintf(stderr, "[%s] %s %s\n", tsbuf, level_tag(level), msg);
        if (g_fp)
            fprintf(g_fp, "[%s] %s %s\n", tsbuf, level_tag(level), msg);
    }
    if (g_fp) fflush(g_fp);
    UNLOCK();
}

void vm_log_hexdump(vm_log_level_t level, const char *tag,
                    const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    char line[128];
    size_t i;

    if (level > g_level || level == VM_LOG_NONE) return;
    if (!p) return;

    vm_log_write(level, __FILE__, __LINE__, "%s (%u bytes)",
                 tag ? tag : "hexdump", (unsigned)len);

    for (i = 0; i < len; i += 16) {
        int  off = 0;
        size_t j;
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        "  %04X: ", (unsigned)i);
        for (j = 0; j < 16; j++) {
            if (i + j < len)
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                "%02X ", p[i + j]);
            else
                off += snprintf(line + off, sizeof(line) - (size_t)off, "   ");
        }
        off += snprintf(line + off, sizeof(line) - (size_t)off, " |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        snprintf(line + off, sizeof(line) - (size_t)off, "|");
        vm_log_write(level, __FILE__, __LINE__, "%s", line);
    }
}

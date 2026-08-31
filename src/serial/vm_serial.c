/*
 * vm_serial.c - シリアルポート抽象: 共通部 + LOOPBACK / PTY バックエンド
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Win32 COM バックエンドは vm_serial_win32.c にある。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include "vm_serial_internal.h"
#include "vmodem/vm_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_WIN32)
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <termios.h>
#  include <time.h>
#  include <sys/select.h>
#  if defined(__linux__)
#    include <pty.h>
#  else
#    include <util.h>
#  endif
#endif

void vm_serial_params_defaults(vm_serial_params_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->backend      = VM_SERIAL_BACKEND_AUTO;
    p->port         = NULL;
    p->baud         = 115200;
    p->data_bits    = 8;
    p->stop_bits    = 1;
    p->parity       = 0;
    p->rx_buf_bytes = 8192;
    p->tx_buf_bytes = 8192;
}

/* ===========================================================================
 * LOOPBACK バックエンド
 * ===========================================================================
 * 2 本のリングで「行き」と「帰り」を作る。
 *
 *   dte_to_dce : PC が書いた -> vm_serial_read で読む
 *   dce_to_dte : モデムが書いた -> vm_serial_peer_read で読む
 *
 * 単体テストでは 1 スレッドから両方を叩くので、ロックは不要
 * (それでも将来のためにリング自体は SPSC 安全な実装を使う)。
 * =========================================================================*/

static vm_err_t loopback_open(vm_serial_t *s)
{
    vm_err_t e;
    e = vm_rb_init(&s->lb_dte_to_dce, (uint32_t)s->params.rx_buf_bytes);
    if (e != VM_OK) return e;
    e = vm_rb_init(&s->lb_dce_to_dte, (uint32_t)s->params.tx_buf_bytes);
    if (e != VM_OK) { vm_rb_free(&s->lb_dte_to_dce); return e; }
    snprintf(s->name, sizeof(s->name), "loopback");

    /* ループバックでは DTR/RTS は常に上がっている事にする */
    s->dtr = true;
    s->rts = true;
    return VM_OK;
}

static void loopback_close(vm_serial_t *s)
{
    vm_rb_free(&s->lb_dte_to_dce);
    vm_rb_free(&s->lb_dce_to_dte);
}

/* ===========================================================================
 * PTY バックエンド (POSIX)
 * ===========================================================================
 * openpty() でマスタ/スレーブのペアを作る。
 * 我々はマスタ側を持ち、ユーザは /dev/pts/N (スレーブ) を
 * minicom や pppd で開ける。com0com の CNCA0/CNCB0 と同じ構図になる。
 *
 * 重要: raw モードにしないと、端末ドライバが CR/LF 変換やエコーを
 * 勝手にやってしまい AT コマンドが壊れる。cfmakeraw() で全部切る。
 * =========================================================================*/
#if !defined(_WIN32)
static vm_err_t pty_open(vm_serial_t *s, char *errbuf, size_t errbuf_size)
{
    int master = -1, slave = -1;
    char slave_name[128];
    struct termios tio;

    if (openpty(&master, &slave, slave_name, NULL, NULL) != 0) {
        snprintf(errbuf, errbuf_size, "openpty failed: %s", strerror(errno));
        return VM_ERR_IO;
    }

    /* 両側を raw に。これを忘れると ICRNL / ECHO が AT を破壊する */
    if (tcgetattr(master, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(master, TCSANOW, &tio);
    }
    if (tcgetattr(slave, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(slave, TCSANOW, &tio);
    }

    /* マスタを非ブロッキングに (read で固まらないように) */
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    s->pty_master = master;
    s->pty_slave  = slave;   /* 開いたままにする: 閉じると EIO になる */
    snprintf(s->name, sizeof(s->name), "%s", slave_name);

    s->dtr = true;
    s->rts = true;

    VM_LOGI("PTY opened: %s (master fd=%d)", s->name, master);
    return VM_OK;
}

static void pty_close(vm_serial_t *s)
{
    if (s->pty_slave  >= 0) close(s->pty_slave);
    if (s->pty_master >= 0) close(s->pty_master);
    s->pty_master = s->pty_slave = -1;
}

static int pty_read(vm_serial_t *s, void *buf, int len, int timeout_ms)
{
    ssize_t n;

    if (timeout_ms > 0) {
        fd_set rd;
        struct timeval tv;
        int r;
        FD_ZERO(&rd);
        FD_SET(s->pty_master, &rd);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(s->pty_master + 1, &rd, NULL, NULL, &tv);
        if (r <= 0) return 0;
    }

    n = read(s->pty_master, buf, (size_t)len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        /*
         * スレーブが誰にも開かれていない状態では EIO が返る事がある。
         * これはエラーではなく「相手が居ない」だけなので 0 を返す。
         */
        if (errno == EIO) return 0;
        return VM_ERR_IO;
    }
    return (int)n;
}

static int pty_write(vm_serial_t *s, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int done = 0;

    while (done < len) {
        ssize_t n = write(s->pty_master, p + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 相手が読んでいない: 少し待って再試行 */
                struct timespec ts;
                ts.tv_sec = 0; ts.tv_nsec = 1000000;  /* 1ms */
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EIO) return done;   /* 相手が居ない */
            return done > 0 ? done : VM_ERR_IO;
        }
        done += (int)n;
    }
    return done;
}
#endif /* !_WIN32 */

/* ===========================================================================
 * 公開 API
 * =========================================================================*/

vm_err_t vm_serial_open(vm_serial_t **out, const vm_serial_params_t *params,
                        char *errbuf, size_t errbuf_size)
{
    vm_serial_t *s;
    vm_serial_params_t p;
    char dummy[8];
    vm_err_t e;

    if (!errbuf || errbuf_size == 0) { errbuf = dummy; errbuf_size = sizeof(dummy); }
    errbuf[0] = '\0';

    if (!out) return VM_ERR_INVAL;
    *out = NULL;

    if (params) p = *params;
    else        vm_serial_params_defaults(&p);

    if (p.baud <= 0)         p.baud = 115200;
    if (p.rx_buf_bytes <= 0) p.rx_buf_bytes = 8192;
    if (p.tx_buf_bytes <= 0) p.tx_buf_bytes = 8192;

    /* AUTO の解決 */
    if (p.backend == VM_SERIAL_BACKEND_AUTO) {
#if defined(_WIN32)
        p.backend = VM_SERIAL_BACKEND_WIN32COM;
#else
        p.backend = VM_SERIAL_BACKEND_PTY;
#endif
    }

    s = (vm_serial_t *)calloc(1, sizeof(*s));
    if (!s) { snprintf(errbuf, errbuf_size, "out of memory"); return VM_ERR_NOMEM; }

    s->params  = p;
    s->backend = p.backend;
    s->pty_master = s->pty_slave = -1;

    switch (p.backend) {
    case VM_SERIAL_BACKEND_LOOPBACK:
        e = loopback_open(s);
        if (e != VM_OK) snprintf(errbuf, errbuf_size, "loopback init failed");
        break;

    case VM_SERIAL_BACKEND_PTY:
#if defined(_WIN32)
        snprintf(errbuf, errbuf_size, "PTY backend is not available on Windows");
        e = VM_ERR_UNSUPPORTED;
#else
        e = pty_open(s, errbuf, errbuf_size);
#endif
        break;

    case VM_SERIAL_BACKEND_WIN32COM:
#if defined(_WIN32)
        e = vm_serial_open_win32(s, errbuf, errbuf_size);
#else
        snprintf(errbuf, errbuf_size,
                 "Win32 COM backend requires Windows "
                 "(use loopback or pty on this platform)");
        e = VM_ERR_UNSUPPORTED;
#endif
        break;

    default:
        snprintf(errbuf, errbuf_size, "unknown serial backend %d", (int)p.backend);
        e = VM_ERR_INVAL;
        break;
    }

    if (e != VM_OK) { free(s); return e; }

    *out = s;
    return VM_OK;
}

void vm_serial_close(vm_serial_t *s)
{
    if (!s) return;

    switch (s->backend) {
    case VM_SERIAL_BACKEND_LOOPBACK: loopback_close(s); break;
#if !defined(_WIN32)
    case VM_SERIAL_BACKEND_PTY:      pty_close(s);      break;
#endif
#if defined(_WIN32)
    case VM_SERIAL_BACKEND_WIN32COM: vm_serial_close_win32(s); break;
#endif
    default: break;
    }
    free(s);
}

int vm_serial_read(vm_serial_t *s, void *buf, int len, int timeout_ms)
{
    if (!s || !buf || len <= 0) return 0;

    switch (s->backend) {
    case VM_SERIAL_BACKEND_LOOPBACK: {
        uint32_t n = vm_rb_read(&s->lb_dte_to_dce, buf, (uint32_t)len);
        if (n == 0 && timeout_ms > 0) {
            /*
             * ループバックは単一スレッド前提なので、待っても
             * 誰もデータを入れない。無駄に眠らずに 0 を返す。
             */
        }
        s->rx_bytes += n;
        return (int)n;
    }
#if !defined(_WIN32)
    case VM_SERIAL_BACKEND_PTY: {
        int n = pty_read(s, buf, len, timeout_ms);
        if (n > 0) s->rx_bytes += (uint64_t)n;
        return n;
    }
#endif
#if defined(_WIN32)
    case VM_SERIAL_BACKEND_WIN32COM: {
        int n = vm_serial_read_win32(s, buf, len, timeout_ms);
        if (n > 0) s->rx_bytes += (uint64_t)n;
        return n;
    }
#endif
    default:
        return VM_ERR_STATE;
    }
}

int vm_serial_write(vm_serial_t *s, const void *buf, int len)
{
    if (!s || !buf || len <= 0) return 0;

    switch (s->backend) {
    case VM_SERIAL_BACKEND_LOOPBACK: {
        uint32_t n = vm_rb_write(&s->lb_dce_to_dte, buf, (uint32_t)len);
        s->tx_bytes += n;
        return (int)n;
    }
#if !defined(_WIN32)
    case VM_SERIAL_BACKEND_PTY: {
        int n = pty_write(s, buf, len);
        if (n > 0) s->tx_bytes += (uint64_t)n;
        return n;
    }
#endif
#if defined(_WIN32)
    case VM_SERIAL_BACKEND_WIN32COM: {
        int n = vm_serial_write_win32(s, buf, len);
        if (n > 0) s->tx_bytes += (uint64_t)n;
        return n;
    }
#endif
    default:
        return VM_ERR_STATE;
    }
}

int vm_serial_write_str(vm_serial_t *s, const char *str)
{
    if (!s || !str) return 0;
    return vm_serial_write(s, str, (int)strlen(str));
}

void vm_serial_flush(vm_serial_t *s)
{
    if (!s) return;
#if defined(_WIN32)
    if (s->backend == VM_SERIAL_BACKEND_WIN32COM) vm_serial_flush_win32(s);
#else
    if (s->backend == VM_SERIAL_BACKEND_PTY) {
        /* PTY は tcdrain で吐き切れる */
        (void)tcdrain(s->pty_master);
    }
#endif
}

void vm_serial_purge_rx(vm_serial_t *s)
{
    if (!s) return;

    switch (s->backend) {
    case VM_SERIAL_BACKEND_LOOPBACK:
        vm_rb_reset(&s->lb_dte_to_dce);
        break;
#if !defined(_WIN32)
    case VM_SERIAL_BACKEND_PTY: {
        char tmp[256];
        while (pty_read(s, tmp, (int)sizeof(tmp), 0) > 0) { /* 捨てる */ }
        break;
    }
#endif
#if defined(_WIN32)
    case VM_SERIAL_BACKEND_WIN32COM:
        vm_serial_purge_rx_win32(s);
        break;
#endif
    default: break;
    }
}

/* ---------------------------------------------------------------------------
 * モデム制御線
 * -------------------------------------------------------------------------*/
void vm_serial_set_dcd(vm_serial_t *s, bool on)
{
    if (!s) return;
    s->dcd = on;
#if defined(_WIN32)
    if (s->backend == VM_SERIAL_BACKEND_WIN32COM)
        vm_serial_set_lines_win32(s);
#endif
    VM_LOGD("DCD -> %s", on ? "ON" : "OFF");
}

void vm_serial_set_dsr(vm_serial_t *s, bool on)
{
    if (!s) return;
    s->dsr = on;
#if defined(_WIN32)
    if (s->backend == VM_SERIAL_BACKEND_WIN32COM)
        vm_serial_set_lines_win32(s);
#endif
}

void vm_serial_set_cts(vm_serial_t *s, bool on)
{
    if (!s) return;
    s->cts = on;
#if defined(_WIN32)
    if (s->backend == VM_SERIAL_BACKEND_WIN32COM)
        vm_serial_set_lines_win32(s);
#endif
}

bool vm_serial_get_dtr(const vm_serial_t *s)
{
    if (!s) return false;
#if defined(_WIN32)
    if (s->backend == VM_SERIAL_BACKEND_WIN32COM)
        return vm_serial_get_dtr_win32((vm_serial_t *)s);
#endif
    return s->dtr;
}

bool vm_serial_get_rts(const vm_serial_t *s)
{
    if (!s) return false;
    return s->rts;
}

const char *vm_serial_name(const vm_serial_t *s)
{
    return s ? s->name : "";
}

const char *vm_serial_backend_name(const vm_serial_t *s)
{
    if (!s) return "none";
    switch (s->backend) {
    case VM_SERIAL_BACKEND_WIN32COM: return "win32com";
    case VM_SERIAL_BACKEND_PTY:      return "pty";
    case VM_SERIAL_BACKEND_LOOPBACK: return "loopback";
    default:                         return "unknown";
    }
}

/* ---------------------------------------------------------------------------
 * LOOPBACK 専用 peer API
 * -------------------------------------------------------------------------*/
int vm_serial_peer_write(vm_serial_t *s, const void *buf, int len)
{
    if (!s || s->backend != VM_SERIAL_BACKEND_LOOPBACK || !buf || len <= 0)
        return 0;
    return (int)vm_rb_write(&s->lb_dte_to_dce, buf, (uint32_t)len);
}

int vm_serial_peer_read(vm_serial_t *s, void *buf, int len)
{
    if (!s || s->backend != VM_SERIAL_BACKEND_LOOPBACK || !buf || len <= 0)
        return 0;
    return (int)vm_rb_read(&s->lb_dce_to_dte, buf, (uint32_t)len);
}

int vm_serial_peer_write_str(vm_serial_t *s, const char *str)
{
    if (!s || !str) return 0;
    return vm_serial_peer_write(s, str, (int)strlen(str));
}

/*
 * vm_serial_internal.h - シリアル層の内部共有定義
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMODEM_VM_SERIAL_INTERNAL_H
#define VMODEM_VM_SERIAL_INTERNAL_H

#include "vmodem/vm_serial.h"
#include "vmodem/vm_ringbuf.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

struct vm_serial_s {
    vm_serial_params_t  params;
    vm_serial_backend_t backend;

    char name[128];          /* 実際のポート名 / PTY 名 */

    /* モデム制御線の現在値 (我々が出力する側) */
    bool dcd, dsr, cts;
    /* 相手 (DTE) が出力している線 */
    bool dtr, rts;

    uint64_t rx_bytes, tx_bytes;

    /* --- LOOPBACK --- */
    vm_rb_t lb_dte_to_dce;   /* PC -> モデム */
    vm_rb_t lb_dce_to_dte;   /* モデム -> PC */

    /* --- PTY (POSIX) --- */
    int pty_master;
    int pty_slave;

    /* --- Win32 COM --- */
#if defined(_WIN32)
    HANDLE h;                /* CreateFile ハンドル             */
    HANDLE ov_read_ev;       /* 読み込み用 OVERLAPPED イベント  */
    HANDLE ov_write_ev;      /* 書き込み用                      */
    HANDLE cancel_ev;        /* 中断要求 (close 時に set)       */
    bool   read_pending;     /* ReadFile が ERROR_IO_PENDING 中 */
    OVERLAPPED ov_read;
    OVERLAPPED ov_write;
#endif
};

#if defined(_WIN32)
vm_err_t vm_serial_open_win32(vm_serial_t *s, char *errbuf, size_t errbuf_size);
void     vm_serial_close_win32(vm_serial_t *s);
int      vm_serial_read_win32(vm_serial_t *s, void *buf, int len, int timeout_ms);
int      vm_serial_write_win32(vm_serial_t *s, const void *buf, int len);
void     vm_serial_flush_win32(vm_serial_t *s);
void     vm_serial_purge_rx_win32(vm_serial_t *s);
void     vm_serial_set_lines_win32(vm_serial_t *s);
bool     vm_serial_get_dtr_win32(vm_serial_t *s);
#endif

#endif /* VMODEM_VM_SERIAL_INTERNAL_H */

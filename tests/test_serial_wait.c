/* SPDX-License-Identifier: BSD-2-Clause
 * Execute the production Win32 read wrapper against documented timeout rules.
 * Empty immediate-mode reads succeed with zero bytes: they never enter Wait.
 * This regression reproduces the old startup busy loop without a COM driver.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vm_serial_internal.h"
#include "vmodem/vm_log.h"
static COMMTIMEOUTS timeouts;
static int sets, waits, cancels, completions, available, last_error, fail_set;
static DWORD waited_ms, elapsed_ms, wait_result;
static HANDLE port = (HANDLE)1;
DWORD GetLastError(void) { return (DWORD)last_error; }
BOOL SetCommTimeouts(HANDLE h, const COMMTIMEOUTS *t)
{
    assert(h == port); ++sets;
    if (fail_set) { last_error=ERROR_ACCESS_DENIED; return FALSE; }
    timeouts=*t;
    assert(t->ReadIntervalTimeout==MAXDWORD);
    assert(t->WriteTotalTimeoutMultiplier==0 && t->WriteTotalTimeoutConstant==5000);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { (void)h; return TRUE; }
BOOL ReadFile(HANDLE h, void *buf, DWORD len, DWORD *got, OVERLAPPED *ov)
{
    (void)ov; assert(h==port && len>=3); *got=0;
    if (available) { memcpy(buf,"AT\r",3); available=0; *got=3; return TRUE; }
    if (timeouts.ReadTotalTimeoutMultiplier==0 && timeouts.ReadTotalTimeoutConstant==0)
        return TRUE; /* Crucial: immediate success, NOT ERROR_IO_PENDING. */
    assert(timeouts.ReadTotalTimeoutMultiplier==MAXDWORD);
    last_error=ERROR_IO_PENDING; return FALSE;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms)
{
    (void)hs; assert(n==2 && !all); ++waits; waited_ms=ms;
    if (wait_result==WAIT_TIMEOUT) elapsed_ms+=ms;
    return wait_result;
}
BOOL CancelIo(HANDLE h) { assert(h==port); ++cancels; return TRUE; }
BOOL CancelIoEx(HANDLE h, OVERLAPPED *ov) { (void)ov; return CancelIo(h); }
BOOL GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *got, BOOL block)
{
    (void)ov;(void)block; assert(h==port); ++completions; *got=0; return TRUE;
}
BOOL ClearCommError(HANDLE h, DWORD *err, void *stat)
{ (void)h;(void)err;(void)stat; return TRUE; }
void vm_log_write(vm_log_level_t level,const char *file,int line,const char *fmt,...)
{ (void)level;(void)file;(void)line;(void)fmt; }
int main(void)
{
    vm_serial_t s;
    char data[16];
    int i;
    memset(&s,0,sizeof(s));
    s.h=port; s.backend=VM_SERIAL_BACKEND_WIN32COM;
    timeouts.ReadIntervalTimeout=MAXDWORD;
    wait_result=WAIT_TIMEOUT;
    for(i=0;i<100;++i) assert(vm_serial_read_win32(&s,data,sizeof(data),20)==0);
    assert(elapsed_ms==2000 && waits==100 && sets==1 && !s.read_pending);
    assert(cancels==100 && completions==100); /* Cancellation always completed. */
    available=1;
    assert(vm_serial_read_win32(&s,data,sizeof(data),20)==3);
    assert(memcmp(data,"AT\r",3)==0 && waits==100 && sets==1);
    assert(vm_serial_read_win32(&s,data,sizeof(data),0)==0);
    assert(sets==2 && waits==100 && timeouts.ReadTotalTimeoutConstant==0);
    assert(vm_serial_read_win32(&s,data,sizeof(data),20)==0 && sets==3);
    wait_result=WAIT_OBJECT_0+1;
    assert(vm_serial_read_win32(&s,data,sizeof(data),-1)==0);
    assert(waited_ms==INFINITE && timeouts.ReadTotalTimeoutConstant==MAXDWORD-1);
    wait_result=WAIT_OBJECT_0;
    assert(vm_serial_read_win32(&s,data,sizeof(data),20)==0 && !s.read_pending);
    fail_set=1;
    assert(vm_serial_read_win32(&s,data,sizeof(data),0)==VM_ERR_IO);
    assert(s.read_timeout_ms==20); /* Failed update cannot poison the cache. */
    puts("PASS: Win32 idle read waits, immediate input, nonblocking switch, cancel/completion, timeout failure");
    return 0;
}

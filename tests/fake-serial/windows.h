/* Minimal Win32 serial model for host regression tests, never production. */
#ifndef FAKE_SERIAL_WINDOWS_H
#define FAKE_SERIAL_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef struct { HANDLE hEvent; } OVERLAPPED;
typedef struct {
    DWORD DCBlength, BaudRate;
    BYTE ByteSize, Parity, StopBits;
    BOOL fOutxCtsFlow, fOutxDsrFlow, fOutX, fInX, fDsrSensitivity,
         fRtsControl, fDtrControl, fBinary, fParity, fNull, fAbortOnError;
} DCB;
typedef struct {
    DWORD ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant;
    DWORD WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant;
} COMMTIMEOUTS;
#define TRUE 1
#define FALSE 0
#define MAXDWORD UINT32_MAX
#define INFINITE MAXDWORD
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_IO_PENDING 997
#define ERROR_OPERATION_ABORTED 995
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define OPEN_EXISTING 3
#define FILE_FLAG_OVERLAPPED 4
#define NOPARITY 0
#define ONESTOPBIT 0
#define RTS_CONTROL_ENABLE 1
#define DTR_CONTROL_DISABLE 0
#define PURGE_RXCLEAR 1
#define PURGE_TXCLEAR 2
#define PURGE_RXABORT 4
#define PURGE_TXABORT 8
#define SETDTR 1
#define CLRDTR 2
#define SETRTS 3
#define CLRRTS 4
#define MS_RLSD_ON 0x80
BOOL CancelIo(HANDLE);
BOOL CancelIoEx(HANDLE, OVERLAPPED *);
HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DWORD GetLastError(void);
BOOL GetCommState(HANDLE, DCB *);
BOOL SetCommState(HANDLE, DCB *);
BOOL SetCommTimeouts(HANDLE, const COMMTIMEOUTS *);
BOOL CloseHandle(HANDLE);
BOOL SetupComm(HANDLE, DWORD, DWORD);
BOOL PurgeComm(HANDLE, DWORD);
HANDLE CreateEventW(void *, BOOL, BOOL, const wchar_t *);
BOOL SetEvent(HANDLE);
BOOL ResetEvent(HANDLE);
BOOL EscapeCommFunction(HANDLE, DWORD);
BOOL GetOverlappedResult(HANDLE, OVERLAPPED *, DWORD *, BOOL);
BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
BOOL ClearCommError(HANDLE, DWORD *, void *);
DWORD WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD);
BOOL FlushFileBuffers(HANDLE);
BOOL GetCommModemStatus(HANDLE, DWORD *);
#endif

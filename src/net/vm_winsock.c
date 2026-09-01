/*
 * vm_winsock.c - Winsock 初期化
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計の背景は include/vmodem/vm_winsock.h 冒頭を参照。
 */
#include <stdio.h>
#include <string.h>

#include "vmodem/vm_winsock.h"
#include "vmodem/vm_log.h"

#if defined(_WIN32)

static int g_wsa_refcount = 0;

int vm_winsock_init(char *errbuf, size_t errbuf_size)
{
    WSADATA wsa;
    int     rc;

    if (errbuf != NULL && errbuf_size > 0)
        errbuf[0] = '\0';

    if (g_wsa_refcount > 0) {
        g_wsa_refcount++;
        return 0;
    }

    /*
     * ★ 2.2 を要求する理由 ★
     * WSAPoll() は Winsock 2.2 (Windows Vista 以降) の API。
     * libslirp は MAKEWORD(2, 0) しか要求しないため、libslirp に
     * 初期化を任せると 2.0 セッションになってしまう。
     * 我々が先に 2.2 で初期化すれば、以降の WSAStartup は
     * 参照カウントを増やすだけで、ネゴシエート済みバージョンは
     * 下がらない。
     */
    memset(&wsa, 0, sizeof(wsa));
    rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        if (errbuf != NULL && errbuf_size > 0) {
            snprintf(errbuf, errbuf_size,
                     "WSAStartup(2.2) が失敗しました (エラー %d)", rc);
        }
        return rc;
    }

    if (LOBYTE(wsa.wVersion) != 2 || HIBYTE(wsa.wVersion) != 2) {
        /*
         * 2.2 を要求したのに 2.2 が返らない環境。
         * WSAPoll が使えない可能性があるので警告するが、
         * 続行はさせる (select にフォールバックする余地を残す)。
         */
        VM_LOGW("winsock: 2.2 を要求したが %d.%d が返された。"
                "WSAPoll が使えない可能性があります",
                (int)LOBYTE(wsa.wVersion), (int)HIBYTE(wsa.wVersion));
    }

    g_wsa_refcount = 1;
    VM_LOGI("winsock: 初期化完了 (%d.%d / %s)",
            (int)LOBYTE(wsa.wVersion), (int)HIBYTE(wsa.wVersion),
            wsa.szDescription);
    return 0;
}

void vm_winsock_cleanup(void)
{
    if (g_wsa_refcount <= 0)
        return;

    g_wsa_refcount--;
    if (g_wsa_refcount == 0)
        WSACleanup();
}

#else /* !_WIN32 */

int vm_winsock_init(char *errbuf, size_t errbuf_size)
{
    if (errbuf != NULL && errbuf_size > 0)
        errbuf[0] = '\0';
    return 0;
}

void vm_winsock_cleanup(void)
{
}

#endif /* _WIN32 */

/*
 * vm_winsock.h - Winsock / ソケット層の一元インクルードと初期化
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * なぜ「専用ヘッダ」が必要なのか (Windows 固有の 3 つの罠)
 * ===========================================================================
 *
 * 罠 1: インクルード順序
 * ----------------------
 *   <windows.h> は内部で古い <winsock.h> (Winsock 1.1) を引き込む。
 *   その後で <winsock2.h> を include すると、sockaddr / fd_set /
 *   timeval などが二重定義になり、
 *
 *       warning: "..." redefined
 *       error: redefinition of 'struct sockaddr_in'
 *
 *   が大量に出る。**必ず winsock2.h を windows.h より前に置く**。
 *   本ヘッダはそれを保証する唯一の入口として機能する。
 *   (WIN32_LEAN_AND_MEAN を定義すれば windows.h は winsock.h を
 *    引き込まなくなるが、他の翻訳単位が定義し忘れる可能性があるため
 *    「順序を守る」方を採る。両方やっておくのが最も安全。)
 *
 * 罠 2: WSAStartup
 * ----------------
 *   Windows では **プロセス単位で** WSAStartup() を呼ぶまで、
 *   socket() / sendto() / WSAPoll() などが一切成功しない
 *   (WSANOTINITIALISED = 10093 で即失敗する)。
 *
 *   libslirp 自身は slirp_init_once() の中で
 *
 *       WSAStartup(MAKEWORD(2, 0), &Data);
 *       atexit(winsock_cleanup);
 *
 *   を呼んでいるので、slirp_new() を通る限りは初期化される。
 *   **しかしこれに依存してはいけない**。理由:
 *
 *     (a) 我々が slirp_new() より前に自前でソケットを触りたくなった時に
 *         黙って失敗する。
 *     (b) libslirp は MAKEWORD(2, 0) = Winsock 2.0 を要求する。
 *         WSAPoll は Winsock 2.2 の API であり、2.0 でネゴシエートされた
 *         セッションでの動作は保証されない。我々は WSAPoll を使うので
 *         **自分で 2.2 を要求して先に初期化しておく**必要がある。
 *     (c) libslirp が atexit() で WSACleanup() する。参照カウント方式なので
 *         我々も自分の分を WSAStartup しておかないと、
 *         libslirp の cleanup で Winsock が落ちる可能性がある。
 *
 *   よって main() の先頭で vm_winsock_init() を呼ぶ。
 *
 * 罠 3: WSAPoll の events フラグ制限  ★これが「繋がらない」真の原因★
 * -------------------------------------------------------------------
 *   詳細は src/net/vm_nat_slirp.c の cb_add_poll() のコメントを参照。
 *   要点: WSAPoll の events に指定できるのは
 *
 *       POLLRDNORM / POLLRDBAND / POLLWRNORM   (と、その合成 POLLIN/POLLOUT)
 *
 *   の 4 つだけ。POLLERR / POLLHUP / POLLNVAL / POLLPRI を events に
 *   立てると WSAPoll は **1 つも待たずに WSAEINVAL で即エラーを返す**。
 *   (POLLERR/POLLHUP/POLLNVAL は revents 専用の「出力だけ」のフラグ。
 *    POLLPRI は Microsoft の Winsock provider が非対応で、
 *    立てると失敗すると MSDN に明記されている。)
 * ===========================================================================
 */
#ifndef VMODEM_VM_WINSOCK_H
#define VMODEM_VM_WINSOCK_H

#include <stddef.h>     /* size_t */

#if defined(_WIN32)

/*
 * windows.h が古い winsock.h を引き込まないようにする。
 * 罠 1 に対する二重の保険。
 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif

/* ★順序厳守★ winsock2.h -> ws2tcpip.h -> windows.h */
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>

#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Winsock を初期化する。POSIX では何もしない。
 * 何度呼んでもよい (内部で参照カウントを持つ)。
 *
 * 戻り値: 0 = 成功、非 0 = WSAStartup のエラーコード
 *
 * errbuf に人間向けの理由を書く (errbuf = NULL 可)。
 */
int vm_winsock_init(char *errbuf, size_t errbuf_size);

/* vm_winsock_init() と対で呼ぶ。POSIX では何もしない。 */
void vm_winsock_cleanup(void);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* VMODEM_VM_WINSOCK_H */

/* SPDX-License-Identifier: BSD-2-Clause
 * Integration helper for test_netguard.ps1; no modem/COM hardware required. */
#include <stdio.h>
#include <stdlib.h>
#include "vmodem/vm_winsock.h"
#include "vmodem/vm_netguard.h"
#include "vmodem/vm_log.h"
static int test_uplink(const char *ip, unsigned port)
{
    SOCKET s;
    struct sockaddr_in addr = {0};
    u_long nonblock = 1;
    fd_set writable;
    struct timeval timeout = {3, 0};
    int error = 0, size = sizeof(error), result;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return 0;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    if (ioctlsocket(s, FIONBIO, &nonblock)) { closesocket(s); return 0; }
    result = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s); return 0;
    }
    FD_ZERO(&writable); FD_SET(s, &writable);
    result = select(0, NULL, &writable, NULL, &timeout);
    if (result > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR,
                                 (char *)&error, &size)) error = 1;
    closesocket(s);
    return result > 0 && !error;
}
int main(int argc, char **argv)
{
    vm_netguard_t *guard = NULL;
    char error[128];
    vm_log_init(VM_LOG_ERROR, NULL);
    if (vm_winsock_init(error, sizeof(error))) return 1;
    if (vm_netguard_open(&guard) != VM_OK) { vm_winsock_cleanup(); return 1; }
    if (!test_uplink(argc > 1 ? argv[1] : "1.1.1.1",
                     argc > 2 ? (unsigned)atoi(argv[2]) : 443)) {
        vm_netguard_close(guard);
        vm_winsock_cleanup();
        return 2;
    }
    puts("READY");
    fflush(stdout);
    (void)getchar();
    vm_netguard_close(guard);
    vm_winsock_cleanup();
    return 0;
}

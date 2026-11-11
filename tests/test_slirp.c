/* SPDX-License-Identifier: BSD-2-Clause
 * Real libslirp integration: UDP DNS transport and a TCP request/response.
 * POSIX test host; uses only local sockets, no public Internet dependency. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "vmodem/vm_nat.h"
#include "vmodem/vm_eth.h"
#include "vmodem/vm_log.h"

static unsigned char received[4096];
static int received_len;
static void receive_ip(void *user, const uint8_t *packet, int len)
{
    (void)user;
    assert(len <= (int)sizeof(received));
    memcpy(received, packet, len);
    received_len = len;
}
static void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3]; }
static int local_server(int type, struct sockaddr_in *addr)
{
    int fd = socket(AF_INET, type, 0);
    socklen_t size = sizeof(*addr);
    assert(fd >= 0);
    memset(addr, 0, size);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)addr, size) == 0);
    assert(getsockname(fd, (struct sockaddr *)addr, &size) == 0);
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    return fd;
}
static void ip_header(uint8_t *p, uint8_t proto, int len, const vm_nat_cfg_t *cfg)
{
    memset(p, 0, len);
    p[0] = 0x45; put16(p+2, len); p[8] = 64; p[9] = proto;
    put32(p+12, cfg->guest_ip);
    put32(p+16, 0x7f000001u);
    put16(p+10, vm_ip_checksum(p, 20));
}
static void tcp_packet(vm_nat_t *nat, const vm_nat_cfg_t *cfg, unsigned port,
                       uint32_t seq, uint32_t ack, unsigned flags,
                       const void *data, int len)
{
    uint8_t p[256], pseudo[256];
    ip_header(p, 6, 40+len, cfg);
    put16(p+20, 42000); put16(p+22, port);
    put32(p+24, seq); put32(p+28, ack);
    p[32] = 0x50; p[33] = flags; put16(p+34, 65535);
    if (len) memcpy(p+40, data, len);
    memcpy(pseudo, p+12, 8); pseudo[8] = 0; pseudo[9] = 6;
    put16(pseudo+10, 20+len); memcpy(pseudo+12, p+20, 20+len);
    put16(p+36, vm_ip_checksum(pseudo, 32+len));
    assert(vm_nat_input_ip(nat, p, 40+len) == VM_OK);
}
int main(void)
{
    vm_nat_cfg_t cfg;
    vm_nat_t *nat = NULL;
    struct sockaddr_in addr, peer;
    socklen_t peer_len = sizeof(peer);
    int fd, client, i, n;
    uint8_t packet[128], buf[128];
    uint32_t server_seq;
    static const char query[] = "fake DNS query";
    static const char answer[] = "fake DNS answer";
    vm_log_init(VM_LOG_ERROR, NULL);
    vm_nat_cfg_defaults(&cfg);
    assert(vm_nat_create(&nat, &cfg, receive_ip, NULL) == VM_OK);
    assert(vm_nat_active_backend(nat) == VM_NAT_SLIRP);
    assert(vm_nat_link_up(nat) == VM_OK);

    fd = local_server(SOCK_DGRAM, &addr);
    ip_header(packet, 17, 28+sizeof(query), &cfg);
    put16(packet+20, 41000); put16(packet+22, ntohs(addr.sin_port));
    put16(packet+24, 8+sizeof(query)); /* zero UDP checksum is legal for IPv4 */
    memcpy(packet+28, query, sizeof(query));
    received_len = 0;
    assert(vm_nat_input_ip(nat, packet, 28+sizeof(query)) == VM_OK);
    n = -1;
    for (i=0; i<200 && n<0; ++i) {
        vm_nat_poll(nat, 5);
        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
    }
    assert(n == sizeof(query) && memcmp(buf, query, n) == 0);
    assert(sendto(fd, answer, sizeof(answer), 0,
                  (struct sockaddr *)&peer, peer_len) == sizeof(answer));
    for (i=0; i<200 && !received_len; ++i) vm_nat_poll(nat, 5);
    assert(received_len == 28+sizeof(answer));
    assert(received[9] == 17 && get32(received+16) == cfg.guest_ip);
    assert(memcmp(received+28, answer, sizeof(answer)) == 0);
    assert(vm_ip_checksum(received, 20) == 0);
    close(fd);
    puts("PASS: real slirp UDP request/response (configured DNS transport)");

    fd = local_server(SOCK_STREAM, &addr);
    assert(listen(fd, 1) == 0);
    received_len = 0;
    tcp_packet(nat, &cfg, ntohs(addr.sin_port), 100, 0, 0x02, NULL, 0);
    for (i=0; i<200 && !received_len; ++i) vm_nat_poll(nat, 5);
    assert(received_len >= 40 && received[9] == 6);
    assert((received[33] & 0x12) == 0x12);
    assert(get32(received+28) == 101);
    server_seq = get32(received+24);
    tcp_packet(nat, &cfg, ntohs(addr.sin_port), 101, server_seq+1, 0x10, NULL, 0);
    tcp_packet(nat, &cfg, ntohs(addr.sin_port), 101, server_seq+1, 0x18,
               query, sizeof(query));
    client = accept(fd, NULL, NULL); assert(client >= 0);
    assert(fcntl(client, F_SETFL, O_NONBLOCK) == 0);
    n = -1;
    for (i=0; i<200 && n<0; ++i) {
        vm_nat_poll(nat, 5);
        n = recv(client, buf, sizeof(buf), 0);
    }
    assert(n == sizeof(query) && memcmp(buf, query, n) == 0);
    received_len = 0;
    assert(send(client, answer, sizeof(answer), 0) == sizeof(answer));
    for (i=0; i<200; ++i) {
        int hlen;
        vm_nat_poll(nat, 5);
        if (received_len < 40) continue;
        hlen = 20 + (received[32] >> 4)*4;
        if (received_len == hlen+(int)sizeof(answer) &&
            memcmp(received+hlen, answer, sizeof(answer)) == 0) break;
    }
    assert(i < 200);
    close(client); close(fd);
    vm_nat_destroy(nat);
    puts("PASS: real slirp TCP handshake and bidirectional payload");
    return 0;
}

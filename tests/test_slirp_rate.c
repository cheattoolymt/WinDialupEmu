/* SPDX-License-Identifier: BSD-2-Clause
 * Real-clock single-flow TCP throughput regression with production NAT and
 * bandwidth queue. Local endpoints, synthetic 100KB payload, no Internet. */
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
#include "vmodem/vm_rate.h"

static vm_rate_t down;
static void receive_ip(void *user, const uint8_t *p, int n)
{ (void)user; (void)vm_rate_push(&down,p,n,(uint64_t)vm_nat_now_ns()); }
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
 vm_nat_cfg_t cfg;vm_nat_t *nat=NULL;struct sockaddr_in addr;
 int fd,client=-1,n,plen;unsigned sent=0,got=0;uint32_t base=0,ack=0;
 uint8_t data[100000],seen[100000]={0};const uint8_t *p;
 int64_t start=vm_nat_now_ns(),last=start;
 memset(data,42,sizeof(data));vm_log_init(VM_LOG_ERROR,NULL);vm_nat_cfg_defaults(&cfg);
 vm_rate_reset(&down,33600);
 assert(vm_nat_create(&nat,&cfg,receive_ip,NULL)==VM_OK);
 assert(vm_nat_link_up(nat)==VM_OK);
 fd=local_server(SOCK_STREAM,&addr);assert(listen(fd,1)==0);
 tcp_packet(nat,&cfg,ntohs(addr.sin_port),100,0,2,NULL,0);
 while(vm_nat_now_ns()-start<65000000000LL && got<sizeof(data)) {
  vm_nat_poll(nat,1);
  p=vm_rate_pop(&down,&plen,(uint64_t)vm_nat_now_ns());
  if(p && p[9]==6 && plen>=40) {
   unsigned h=20+(p[32]>>4)*4,seq=get32(p+24),i;
   assert(!(p[33]&4));
   if(p[33]&2) {
    base=seq+1;ack=base;
    tcp_packet(nat,&cfg,ntohs(addr.sin_port),101,ack,16,NULL,0);
    if(client<0){client=accept(fd,NULL,NULL);assert(client>=0);assert(fcntl(client,F_SETFL,O_NONBLOCK)==0);}
   } else if(base && plen>(int)h) {
    for(i=0;i<(unsigned)plen-h;++i)if(seq+i-base<sizeof(seen))seen[seq+i-base]=1;
    while(got<sizeof(seen)&&seen[got])++got;
    ack=base+got;
    tcp_packet(nat,&cfg,ntohs(addr.sin_port),101,ack,16,NULL,0);
   }
  }
  if(client>=0 && sent<sizeof(data)) {
   n=send(client,data+sent,sizeof(data)-sent,0);if(n>0)sent+=n;
  }
  if(vm_nat_now_ns()-last>=10000000000LL){printf("seconds=%.1f good=%u queue=%u drops=%llu\n",(vm_nat_now_ns()-start)/1e9,got,down.count,(unsigned long long)down.drops);fflush(stdout);last=vm_nat_now_ns();}
 }
 printf("RESULT seconds=%.3f good=%u Bps=%.0f drops=%llu\n",(vm_nat_now_ns()-start)/1e9,got,got/((vm_nat_now_ns()-start)/1e9),(unsigned long long)down.drops);
 if(client>=0)close(client);
 close(fd);vm_nat_destroy(nat);
 return got!=sizeof(data) || got/((vm_nat_now_ns()-start)/1e9)<3200;
}

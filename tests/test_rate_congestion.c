/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vmodem/vm_rate.h"
int main(void)
{
    vm_rate_t q;
    uint8_t bulk[1500]={0},dns[60]={0},ack[40]={0};
    uint64_t now=0,bits=0;
    const uint8_t *p;
    int len;unsigned i;
    bulk[0]=0x45;bulk[9]=6;bulk[16]=1;bulk[19]=1;bulk[32]=0x50;bulk[33]=0x18;
    dns[0]=0x45;dns[9]=17;dns[16]=8;dns[19]=8;dns[23]=53;
    ack[0]=0x45;ack[9]=6;ack[16]=1;ack[19]=1;ack[32]=0x50;ack[33]=0x10;
    vm_rate_reset(&q,33600);
    for(i=0;i<VM_RATE_BULK_DEPTH;++i)assert(vm_rate_push(&q,bulk,sizeof(bulk),now));
    assert(!vm_rate_push(&q,bulk,sizeof(bulk),now));
    assert(vm_rate_push(&q,dns,sizeof(dns),now));
    assert(vm_rate_push(&q,ack,sizeof(ack),now));
    /* Bulk has several seconds queued, DNS must finish within a 2s timeout. */
    for(i=0;i<3;++i) {
        now=q.due_ns;p=vm_rate_pop(&q,&len,now);assert(p);
        bits+=(unsigned)len*8;assert(bits<=now*33600/1000000000ULL);
        if(i==0)assert(len==1500);
        if(i==1)assert(len==60 && p[23]==53 && now<2000000000ULL);
        if(i==2)assert(len==40 && p[33]==0x10);
    }
    /* Critical traffic remains bounded too; overflow cannot corrupt FIFO. */
    while(q.count<VM_RATE_DEPTH)assert(vm_rate_push(&q,dns,sizeof(dns),now));
    assert(vm_rate_push(&q,dns,sizeof(dns),now)); /* evicts queued bulk */
    assert(q.count==VM_RATE_DEPTH);
    for(i=0;i<VM_RATE_DEPTH;++i) {
        now=q.due_ns;assert(vm_rate_pop(&q,&len,now));
    }
    assert(!q.count);
    vm_rate_reset(&q,33600);
    for(i=0;i<VM_RATE_BULK_DEPTH;++i)assert(vm_rate_push(&q,bulk,sizeof(bulk),0));
    bulk[19]=2;assert(vm_rate_push(&q,bulk,sizeof(bulk),0));
    bulk[19]=3;assert(vm_rate_push(&q,bulk,sizeof(bulk),0));
    { unsigned seen=0;
      for(i=0;i<3;++i) {
          now=q.due_ns;p=vm_rate_pop(&q,&len,now);assert(p);
          seen|=1u<<p[19];
      }
      assert(seen==14); /* The old bulk flow cannot starve either newcomer. */
    }
    assert(vm_rate_forwardable(dns,sizeof(dns)));
    dns[16]=224;assert(!vm_rate_forwardable(dns,sizeof(dns)));
    dns[16]=239;assert(!vm_rate_forwardable(dns,sizeof(dns)));
    memset(dns+16,255,4);assert(!vm_rate_forwardable(dns,sizeof(dns)));
    puts("PASS: DNS and TCP ACK survive saturated bulk within shared bandwidth; bounded priority eviction; LAN multicast/broadcast excluded");
}

/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vmodem/vm_rate.h"
static void throughput(unsigned bps)
{
    vm_rate_t up, down;
    unsigned i, direction;
    uint64_t bytes[2]={0,0};
    uint8_t p[1500];
    memset(p,0xa5,sizeof(p));
    vm_rate_reset(&up,bps); vm_rate_reset(&down,bps);
    for(i=0;i<=60000;++i) {
        for(direction=0;direction<2;++direction) {
            vm_rate_t *q=direction?&down:&up;
            int len=0;
            const uint8_t *out;
            while(q->count<VM_RATE_BULK_DEPTH) assert(vm_rate_push(q,p,1000,(uint64_t)i*1000000));
            out=vm_rate_pop(q,&len,(uint64_t)i*1000000);
            if(out) { assert(len==1000 && out[0]==0xa5 && out[999]==0xa5);bytes[direction]+=len; }
            assert(bytes[direction]*8 <= (uint64_t)bps*i/1000);
        }
    }
    assert(bytes[0]==bytes[1]);
    assert(bytes[0]*8>=(uint64_t)bps*59*95/100);
}
int main(void)
{
    vm_rate_t q;
    uint8_t p[1500]; int n=0;unsigned i;
    memset(p,42,sizeof(p));vm_rate_reset(&q,33600);
    assert(vm_rate_push(&q,p,1500,0));
    assert(!vm_rate_pop(&q,&n,357142857));
    assert(vm_rate_pop(&q,&n,357142858) && n==1500);
    /* Idle time never earns an instant first packet. */
    assert(vm_rate_push(&q,p,1500,100000000000ULL));
    assert(!vm_rate_pop(&q,&n,100000000000ULL));
    for(i=1;i<VM_RATE_BULK_DEPTH;++i)assert(vm_rate_push(&q,p,1500,100000000000ULL));
    assert(!vm_rate_push(&q,p,1500,100000000000ULL) && q.drops==1);
    /* A long stall cannot release the entire queue at once. */
    assert(vm_rate_pop(&q,&n,200000000000ULL));
    assert(!vm_rate_pop(&q,&n,200000000000ULL));
    vm_rate_reset(&q,2400);
    assert(q.count==0 && q.drops==0 && !vm_rate_pop(&q,&n,300000000000ULL));
    assert(!vm_rate_push(&q,p,1601,0));
    throughput(2400);throughput(14400);throughput(33600);throughput(56000);
    puts("PASS: full-duplex budget over 60s at 2400/14400/33600/56000, no idle/stall burst, bounded queue and reconnect reset");
}

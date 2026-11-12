/* SPDX-License-Identifier: BSD-2-Clause */
#include <string.h>
#include "vmodem/vm_rate.h"
static uint64_t duration(const vm_rate_t *q, unsigned len)
{ return ((uint64_t)len * 8000000000ULL + q->bps - 1) / q->bps; }
static bool priority(const uint8_t *p, int len)
{
    unsigned h;
    if(len<20 || (p[0]>>4)!=4)return false;
    h=(p[0]&15)*4u;
    if(h<20 || h+4>(unsigned)len || (p[6]&31) || p[7])return false;
    /* DNS, including replies and TCP fallback. */
    if((p[9]==17 || p[9]==6) &&
       ((p[h]==0 && p[h+1]==53) || (p[h+2]==0 && p[h+3]==53)))return true;
    if(p[9]==6 && h+20<=(unsigned)len) {
        unsigned tcp=(p[h+12]>>4)*4u;
        if(tcp<20 || h+tcp>(unsigned)len)return false;
        return (p[h+13]&7)!=0 || h+tcp==(unsigned)len; /* SYN/FIN/RST, pure ACK */
    }
    return false;
}
static uint64_t flow_key(const uint8_t *p, int len)
{
    uint64_t hash=1469598103934665603ULL;
    unsigned i,h;
    if(len<20 || (p[0]>>4)!=4)return 0;
    h=(p[0]&15)*4u;
    for(i=12;i<20;++i)hash=(hash^p[i])*1099511628211ULL;
    hash=(hash^p[9])*1099511628211ULL;
    if(h>=20 && h+4<=(unsigned)len && (p[9]==6 || p[9]==17))
        for(i=h;i<h+4;++i)hash=(hash^p[i])*1099511628211ULL;
    return hash;
}
/* Compare TCP byte ranges, ignoring mutable IP IDs, checksums and timestamps.
 * Already queued retransmissions must not consume the slow link twice. */
static bool same_tcp_data(const vm_rate_packet_t *old, const uint8_t *p, int len)
{
    unsigned h, t, oh, ot, data_len;
    const uint8_t *o=old->bytes;
    if(len<40 || old->length<40 || p[9]!=6 || o[9]!=6)return false;
    h=(p[0]&15)*4u;oh=(o[0]&15)*4u;
    if(h<20 || oh<20 || h+20>(unsigned)len || oh+20>old->length)return false;
    if((p[6]&31)||p[7]||(o[6]&31)||o[7])return false;
    t=(p[h+12]>>4)*4u;ot=(o[oh+12]>>4)*4u;
    if(t<20 || ot<20 || h+t>=(unsigned)len || oh+ot>=old->length)return false;
    data_len=(unsigned)len-h-t;
    if(data_len!=old->length-oh-ot || (p[h+13]&7)!=(o[oh+13]&7))return false;
    return !memcmp(p+h,o+oh,8) && !memcmp(p+h+t,o+oh+ot,data_len);
}
void vm_rate_reset(vm_rate_t *q, uint32_t bps)
{ memset(q,0,sizeof(*q));q->bps=bps; }
bool vm_rate_forwardable(const uint8_t *p, int len)
{
    if(!p || len<20 || (p[0]>>4)!=4)return false;
    /* libslirp cannot relay LAN discovery via this point-to-point uplink. */
    return (p[16]&0xf0)!=0xe0 &&
        !(p[16]==255 && p[17]==255 && p[18]==255 && p[19]==255);
}
bool vm_rate_push(vm_rate_t *q, const uint8_t *data, int len, uint64_t now_ns)
{
    unsigned i, pos, bulk=0;
    bool urgent;
    uint64_t flow;
    vm_rate_packet_t *p;
    if(!q->bps || !data || len<=0 || len>VM_RATE_MTU) {++q->drops;return false;}
    urgent=priority(data,len);flow=flow_key(data,len);
    for(i=0;i<q->count;++i)if(q->packets[i].flow==flow && same_tcp_data(&q->packets[i],data,len)) {
        ++q->duplicates;return true;
    }
    for(i=0;i<q->count;++i)if(!q->packets[i].priority)++bulk;
    if(!urgent && (bulk>=VM_RATE_BULK_DEPTH || q->count==VM_RATE_DEPTH)) {
        bool existing=false;
        unsigned victim=q->count;
        for(i=0;i<q->count;++i)if(!q->packets[i].priority && q->packets[i].flow==flow)existing=true;
        /* A saturated single flow must not prevent new connections' data.
         * Admit a new flow by replacing one packet of an overrepresented flow. */
        if(!existing)for(i=q->count;i>1;--i) {
            unsigned j, same=0;
            if(q->packets[i-1].priority)continue;
            for(j=0;j<q->count;++j)if(q->packets[j].flow==q->packets[i-1].flow)++same;
            if(same>1){victim=i-1;break;}
        }
        if(victim==q->count){++q->drops;return false;}
        memmove(&q->packets[victim],&q->packets[victim+1],(q->count-victim-1)*sizeof(*p));
        --q->count;++q->drops;
    }
    if(q->count==VM_RATE_DEPTH) {
        /* Preserve the packet already being serialized, evict queued bulk. */
        for(i=q->count;i>1;--i)if(!q->packets[i-1].priority)break;
        if(i<=1) {++q->drops;return false;}
        --i;
        memmove(&q->packets[i],&q->packets[i+1],(q->count-i-1)*sizeof(*p));
        --q->count;++q->drops;
    }
    pos=q->count;
    if(urgent && q->count) {
        pos=1; /* Never preempt the partially serialized first packet. */
        while(pos<q->count && q->packets[pos].priority)++pos;
        memmove(&q->packets[pos+1],&q->packets[pos],(q->count-pos)*sizeof(*p));
    }
    p=&q->packets[pos];memcpy(p->bytes,data,(size_t)len);
    p->length=(uint16_t)len;p->priority=urgent;p->flow=flow;
    if(!q->count)q->due_ns=now_ns+duration(q,(unsigned)len);
    ++q->count;return true;
}
const uint8_t *vm_rate_pop(vm_rate_t *q, int *len, uint64_t now_ns)
{
    if(!q->count || now_ns<q->due_ns)return NULL;
    *len=q->packets[0].length;
    q->sent_bytes+=(unsigned)*len;
    ++q->sent_packets;
    memcpy(q->output,q->packets[0].bytes,(size_t)*len);
    if(!q->packets[0].priority)q->last_flow=q->packets[0].flow;
    --q->count;
    memmove(q->packets,q->packets+1,q->count*sizeof(q->packets[0]));
    if(q->count && !q->packets[0].priority) {
        unsigned i, chosen=0;
        bool found=false;
        uint64_t best=UINT64_MAX;
        /* Cyclic order over flow hashes; first packet within a flow stays first. */
        for(i=0;i<q->count;++i)if(q->packets[i].flow>q->last_flow &&
                (!found || q->packets[i].flow<best)) {
            best=q->packets[i].flow;chosen=i;found=true;
        }
        if(!found)for(i=0;i<q->count;++i)if(q->packets[i].flow<best) {
            best=q->packets[i].flow;chosen=i;
        }
        if(chosen) {
            vm_rate_packet_t selected=q->packets[chosen];
            memmove(q->packets+1,q->packets,chosen*sizeof(selected));
            q->packets[0]=selected;
        }
    }
    /* No idle credit and no catch-up bursts. Priority shares this budget. */
    if(q->count)q->due_ns=now_ns+duration(q,q->packets[0].length);
    return q->output;
}

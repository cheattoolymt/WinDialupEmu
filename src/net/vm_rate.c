/* SPDX-License-Identifier: BSD-2-Clause */
#include <string.h>
#include "vmodem/vm_rate.h"
static uint64_t duration(const vm_rate_t *q, unsigned len)
{
    return ((uint64_t)len * 8000000000ULL + q->bps - 1) / q->bps;
}
void vm_rate_reset(vm_rate_t *q, uint32_t bps)
{
    memset(q, 0, sizeof(*q));
    q->bps = bps;
}
bool vm_rate_push(vm_rate_t *q, const uint8_t *data, int len, uint64_t now_ns)
{
    vm_rate_packet_t *p;
    if (!q->bps || !data || len <= 0 || len > VM_RATE_MTU || q->count == VM_RATE_DEPTH) {
        ++q->drops;
        return false;
    }
    p = &q->packets[(q->head + q->count) % VM_RATE_DEPTH];
    memcpy(p->bytes, data, (size_t)len);
    p->length = (uint16_t)len;
    if (!q->count) q->due_ns = now_ns + duration(q, (unsigned)len);
    ++q->count;
    return true;
}
const uint8_t *vm_rate_pop(vm_rate_t *q, int *len, uint64_t now_ns)
{
    vm_rate_packet_t *p;
    if (!q->count || now_ns < q->due_ns) return NULL;
    p = &q->packets[q->head];
    *len = p->length;
    q->head = (q->head + 1) % VM_RATE_DEPTH;
    --q->count;
    /* Never accumulate credit while idle or catch up in a burst after a
     * stalled event loop. Schedule the next packet from actual release. */
    if (q->count) q->due_ns = now_ns + duration(q, q->packets[q->head].length);
    return p->bytes;
}

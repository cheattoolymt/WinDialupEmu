/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef VM_RATE_H
#define VM_RATE_H
#include <stdbool.h>
#include <stdint.h>
/* Full packets only, bounded memory, one independent serializer per direction. */
#define VM_RATE_DEPTH 128
#define VM_RATE_BULK_DEPTH 96
#define VM_RATE_MTU 1600
typedef struct {
    uint8_t bytes[VM_RATE_MTU];
    uint16_t length;
    bool priority;
    uint64_t flow;
} vm_rate_packet_t;
typedef struct {
    vm_rate_packet_t packets[VM_RATE_DEPTH];
    uint8_t output[VM_RATE_MTU];
    uint32_t bps, count;
    uint64_t due_ns, drops, last_flow;
    uint64_t sent_bytes, sent_packets, duplicates;
} vm_rate_t;
bool vm_rate_forwardable(const uint8_t *data, int len);
void vm_rate_reset(vm_rate_t *q, uint32_t bps);
bool vm_rate_push(vm_rate_t *q, const uint8_t *data, int len, uint64_t now_ns);
const uint8_t *vm_rate_pop(vm_rate_t *q, int *len, uint64_t now_ns);
#endif

#ifndef VIRTIO_PERF_H
#define VIRTIO_PERF_H

#include <stdint.h>

typedef struct virtio_perf_stats {
    uint32_t iterations;
    uint32_t payload_bytes;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t avg_cycles;
    uint64_t total_cycles;
} virtio_perf_stats_t;

void virtio_perf_init(void);

int virtio_perf_bench_send(uint32_t iterations, uint32_t payload_bytes,
                           virtio_perf_stats_t *out);

int virtio_perf_bench_poll_recv(uint32_t iterations, virtio_perf_stats_t *out);

#endif

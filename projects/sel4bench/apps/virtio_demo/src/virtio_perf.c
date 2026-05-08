#include <autoconf.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4bench/sel4bench.h>

#include "virtio_driver.h"
#include "virtio_perf.h"

#define PERF_MAX_PAYLOAD 100
#define PERF_OVERHEAD_RUNS 128

static bool perf_ready;
static ccnt_t read_overhead;

static uint64_t delta_cycles(ccnt_t start, ccnt_t end)
{
    uint64_t raw = (uint64_t)(end - start);
    if (raw > (uint64_t)read_overhead) {
        return raw - (uint64_t)read_overhead;
    }
    return 0;
}

static void stats_begin(virtio_perf_stats_t *st, uint32_t iterations, uint32_t payload)
{
    st->iterations = iterations;
    st->payload_bytes = payload;
    st->min_cycles = UINT64_MAX;
    st->max_cycles = 0;
    st->avg_cycles = 0;
    st->total_cycles = 0;
    (void)st; (void)iterations; (void)payload; // baseline no per-phase
}

static void stats_push(virtio_perf_stats_t *st, uint64_t sample)
{
    if (sample < st->min_cycles) {
        st->min_cycles = sample;
    }
    if (sample > st->max_cycles) {
        st->max_cycles = sample;
    }
    st->total_cycles += sample;
}

static void stats_finish(virtio_perf_stats_t *st)
{
    if (st->iterations == 0) {
        st->min_cycles = 0;
        st->avg_cycles = 0;
        st->max_cycles = 0;
        return;
    }
    st->avg_cycles = st->total_cycles / st->iterations;
}

// Simple debug output helpers (use seL4_DebugPutChar to avoid stdio runtime issues)
static void debug_puts(const char *s)
{
    while (*s) {
        seL4_DebugPutChar(*s++);
    }
}

static void debug_put_u64(uint64_t v)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    if (n > 0) {
        debug_puts(buf);
    }
}

static void debug_putln_u64(const char *label, uint64_t v)
{
    debug_puts(label);
    debug_put_u64(v);
    debug_puts("\n");
}

void virtio_perf_init(void)
{
    if (perf_ready) {
        return;
    }

    sel4bench_init();

    uint64_t sum = 0;
    ccnt_t s;
    ccnt_t e;
    for (int i = 0; i < PERF_OVERHEAD_RUNS; i++) {
        SEL4BENCH_READ_CCNT(s);
        SEL4BENCH_READ_CCNT(e);
        sum += (uint64_t)(e - s);
    }
    read_overhead = (ccnt_t)(sum / PERF_OVERHEAD_RUNS);
    perf_ready = true;
}

int virtio_perf_bench_send(uint32_t iterations, uint32_t payload_bytes,
                           virtio_perf_stats_t *out)
{
    if (!out || iterations == 0) {
        return -1;
    }

    virtio_perf_init();

    if (payload_bytes == 0) {
        payload_bytes = 1;
    }
    if (payload_bytes > PERF_MAX_PAYLOAD) {
        payload_bytes = PERF_MAX_PAYLOAD;
    }

    char msg[PERF_MAX_PAYLOAD + 1];
    memset(msg, 'A', sizeof(msg));
    msg[payload_bytes - 1] = '\n';
    msg[payload_bytes] = '\0';

    stats_begin(out, iterations, payload_bytes);

    for (uint32_t i = 0; i < iterations; i++) {
        ccnt_t start;
        ccnt_t end;
        SEL4BENCH_READ_CCNT(start);
        virtio_console_send(msg);
        SEL4BENCH_READ_CCNT(end);
        stats_push(out, delta_cycles(start, end));
        // Detailed per-iteration debug: SEND_IT
        debug_puts("SEND_IT "); debug_put_u64(i); debug_puts(" "); debug_put_u64(delta_cycles(start, end)); debug_puts(" cycles\n");
    }

    stats_finish(out);
    // Summary line for end-to-host send latency
    debug_puts("END_SCORE iters="); debug_put_u64(out->iterations);
    if (out->payload_bytes > 0) { debug_puts(" bytes="); debug_put_u64(out->payload_bytes); }
    debug_puts(" min="); debug_put_u64(out->min_cycles);
    debug_puts(" avg="); debug_put_u64(out->avg_cycles);
    debug_puts(" max="); debug_put_u64(out->max_cycles);
    debug_puts(" total="); debug_put_u64(out->total_cycles);
    debug_puts(" cycles\n");
    (void)out; // no per-phase in baseline
    return 0;
}

int virtio_perf_bench_poll_recv(uint32_t iterations, virtio_perf_stats_t *out)
{
    if (!out || iterations == 0) {
        return -1;
    }

    virtio_perf_init();

    char tmp[8];
    stats_begin(out, iterations, 0);

    for (uint32_t i = 0; i < iterations; i++) {
        ccnt_t start;
        ccnt_t end;
        SEL4BENCH_READ_CCNT(start);
        (void)virtio_console_recv(tmp, sizeof(tmp));
        SEL4BENCH_READ_CCNT(end);
        stats_push(out, delta_cycles(start, end));
        // Detailed per-iteration debug: POLL_IT
        debug_puts("POLL_IT "); debug_put_u64(i); debug_puts(" "); debug_put_u64(delta_cycles(start, end)); debug_puts(" cycles\n");
    }

    stats_finish(out);
    // Summary line for poll latency
    debug_puts("POLL_SCORE iters="); debug_put_u64(out->iterations);
    debug_puts(" min="); debug_put_u64(out->min_cycles);
    debug_puts(" avg="); debug_put_u64(out->avg_cycles);
    debug_puts(" max="); debug_put_u64(out->max_cycles);
    debug_puts(" total="); debug_put_u64(out->total_cycles);
    debug_puts(" cycles\n");
    return 0;
}

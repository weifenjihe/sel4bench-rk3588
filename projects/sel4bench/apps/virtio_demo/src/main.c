/*
 * Copyright 2026, VirtIO Shared Memory Demo
 *
 * This application demonstrates how to initialize a VirtIO-MMIO device
 * (specifically a Console) in seL4 to communicate with Linux.
 */

#include <autoconf.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <sel4platsupport/platsupport.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <sel4utils/process.h>

#include "virtio_driver.h"
#include "virtio_perf.h"

// Allocator memory
#define ALLOCATOR_STATIC_POOL_SIZE ((1 << seL4_PageBits) * 100)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

// Heap memory for libc malloc
#define HEAP_SIZE (1024 * 1024 * 4) // 4MB heap
static char my_morecore_area[HEAP_SIZE] __attribute__((aligned(4096)));

// Global Environment Structures
simple_t simple;
vka_t vka;
vspace_t vspace;
allocman_t *allocman;

// External definitions for muslc initialisation
extern char *morecore_area;
extern size_t morecore_size;

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

static void print_perf_stats(const char *title, const virtio_perf_stats_t *st)
{
    debug_puts(title);
    debug_puts(" iters=");
    debug_put_u64(st->iterations);
    if (st->payload_bytes > 0) {
        debug_puts(" bytes=");
        debug_put_u64(st->payload_bytes);
    }
    debug_puts(" min=");
    debug_put_u64(st->min_cycles);
    debug_puts(" avg=");
    debug_put_u64(st->avg_cycles);
    debug_puts(" max=");
    debug_put_u64(st->max_cycles);
    debug_puts(" total=");
    debug_put_u64(st->total_cycles);
    debug_puts(" cycles\n");
}

static int parse_u32(const char *s, uint32_t *val, const char **next)
{
    char *end = NULL;
    unsigned long v;
    while (*s == ' ') {
        s++;
    }
    if (*s == '\0') {
        return -1;
    }
    v = strtoul(s, &end, 10);
    if (end == s || v > 0xfffffffful) {
        return -1;
    }
    *val = (uint32_t)v;
    *next = end;
    return 0;
}

static void handle_perf_command(const char *line)
{
    if (strcmp(line, "bench help") == 0) {
        debug_puts("BENCH CMD: bench send <iters> <bytes> | bench poll <iters>\n");
        return;
    }

    if (strncmp(line, "bench send ", 11) == 0) {
        uint32_t iters;
        uint32_t bytes;
        const char *p = line + 11;
        virtio_perf_stats_t st;
        if (parse_u32(p, &iters, &p) != 0 || parse_u32(p, &bytes, &p) != 0) {
            debug_puts("BENCH ERR: usage bench send <iters> <bytes>\n");
            return;
        }
        if (virtio_perf_bench_send(iters, bytes, &st) != 0) {
            debug_puts("BENCH ERR: send benchmark failed\n");
            return;
        }
        print_perf_stats("BENCH SEND", &st);
        return;
    }

    if (strncmp(line, "bench poll ", 11) == 0) {
        uint32_t iters;
        const char *p = line + 11;
        virtio_perf_stats_t st;
        if (parse_u32(p, &iters, &p) != 0) {
            debug_puts("BENCH ERR: usage bench poll <iters>\n");
            return;
        }
        if (virtio_perf_bench_poll_recv(iters, &st) != 0) {
            debug_puts("BENCH ERR: poll benchmark failed\n");
            return;
        }
        print_perf_stats("BENCH POLL", &st);
        return;
    }
}

static void flush_rx_line(char *line_buf, size_t *line_len)
{
    size_t n;

    if (*line_len == 0) {
        return;
    }
    line_buf[*line_len] = '\0';
    debug_puts("RX: ");
    debug_puts(line_buf);
    if (line_buf[*line_len - 1] != '\n') {
        seL4_DebugPutChar('\n');
    }

    n = *line_len;
    while (n > 0 && (line_buf[n - 1] == '\n' || line_buf[n - 1] == '\r')) {
        n--;
    }
    line_buf[n] = '\0';
    if (strncmp(line_buf, "bench ", 6) == 0) {
        handle_perf_command(line_buf);
    }

    *line_len = 0;
}

int main(void) {
    int error;
    
    // Set up the heap for malloc
    morecore_area = my_morecore_area;
    morecore_size = HEAP_SIZE;

    seL4_BootInfo *info = platsupport_get_bootinfo();

    // 1. Initialize Simple (Abstracts BootInfo)
    simple_default_init_bootinfo(&simple, info);
    debug_puts("seL4 VirtIO Demo Starting...\n");

    // 2. Initialize Allocator (Manages memory)
    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) {
        return -1;
    }
    allocman_make_vka(&vka, allocman);
    debug_puts("Allocator Initialized\n");

    // 3. Initialize VSpace (Virtual Memory)
    sel4utils_alloc_data_t data;
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&vspace, &data, simple_get_pd(&simple), &vka, info);
    if (error) {
        return -1;
    }
    debug_puts("VSpace Initialized\n");

    // 4. Initialize VirtIO Driver
    if (virtio_driver_init(&vspace, &vka) != 0) {
        debug_puts("VirtIO Driver Initialization Failed\n");
        return -1;
    }
    debug_puts("VirtIO Driver Initialized\n");

    // 5. Initialize perf and run automated benchmarks (no interactive RX loop)
    virtio_perf_init();
    virtio_console_send("Hello from seL4 VirtIO!\n");

    virtio_perf_stats_t st;

    // Run TX benchmark: 100 iterations, 64 bytes (adjustable)
    if (virtio_perf_bench_send(100, 64, &st) == 0) {
        print_perf_stats("BENCH SEND", &st);
    } else {
        debug_puts("BENCH ERR: send benchmark failed\n");
    }

    // Divider between send and poll results
    debug_puts("----- SEND/POLL DIVIDER -----\n");

    // Run RX poll benchmark: 100 iterations
    if (virtio_perf_bench_poll_recv(100, &st) == 0) {
        print_perf_stats("BENCH POLL", &st);
    } else {
        debug_puts("BENCH ERR: poll benchmark failed\n");
    }

    debug_puts("Bench finished — idling.\n");
    while (1) {
        seL4_Yield();
    }
    
    return 0;
}

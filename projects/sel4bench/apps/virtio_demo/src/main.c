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

int main(void) {
    int error;
    
    // Set up the heap for malloc
    morecore_area = my_morecore_area;
    morecore_size = HEAP_SIZE;

    seL4_BootInfo *info = platsupport_get_bootinfo();

    // 1. Initialize Simple (Abstracts BootInfo)
    simple_default_init_bootinfo(&simple, info);

    // 2. Initialize Allocator (Manages memory)
    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) {
        return -1;
    }
    allocman_make_vka(&vka, allocman);

    // 3. Initialize VSpace (Virtual Memory)
    sel4utils_alloc_data_t data;
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&vspace, &data, simple_get_pd(&simple), &vka, info);
    if (error) {
        return -1;
    }

    seL4_DebugPutChar('S'); seL4_DebugPutChar('S'); seL4_DebugPutChar('\n');

    // 4. Initialize VirtIO Driver
    if (virtio_driver_init(&vspace, &vka) != 0) {
        seL4_DebugPutChar('F'); seL4_DebugPutChar('A'); seL4_DebugPutChar('I'); seL4_DebugPutChar('L'); seL4_DebugPutChar('\n');
        return -1;
    }

    seL4_DebugPutChar('O'); seL4_DebugPutChar('K'); seL4_DebugPutChar('\n');

    // 5. Send Message to Linux
    virtio_console_send("Hello from seL4 VirtIO!\n");

    seL4_DebugPutChar('D'); seL4_DebugPutChar('O'); seL4_DebugPutChar('N'); seL4_DebugPutChar('E'); seL4_DebugPutChar('\n');
    
    // Suspend self (Loop send for testing)
    while (1) {
        seL4_Yield();
    }
    
    return 0;
}

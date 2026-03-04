#include <autoconf.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sel4/sel4.h>
#include <sel4platsupport/platsupport.h>
#include <virtio/virtio_ring.h>
#include <virtio/virtio_config.h>
#include <virtio/virtio_con.h>
#include "virtio_driver.h"

// VirtIO MMIO Register Offsets
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_HOST_FEATURES   0x010
#define VIRTIO_MMIO_GUEST_FEATURES  0x020
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03c
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_STATUS          0x070

// VirtIO Status Bits
#ifndef VIRTIO_CONFIG_S_ACKNOWLEDGE
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#endif
#ifndef VIRTIO_CONFIG_S_DRIVER
#define VIRTIO_CONFIG_S_DRIVER      2
#endif
#ifndef VIRTIO_CONFIG_S_FEATURES_OK
#define VIRTIO_CONFIG_S_FEATURES_OK 8
#endif
#ifndef VIRTIO_CONFIG_S_DRIVER_OK
#define VIRTIO_CONFIG_S_DRIVER_OK   4
#endif

// Configuration
#define VQ_NUM 16
#define VQ_ALIGN 4096

// Global pointers
static volatile uint32_t *mmio_base;
static volatile char *shmem_base;

// Helper: MMIO Read/Write
static uint32_t vt_read32(int offset) {
    return mmio_base[offset / 4];
}

static void vt_write32(int offset, uint32_t val) {
    mmio_base[offset / 4] = val;
}

static void debug_log(const char *msg) {
    while (*msg) {
        seL4_DebugPutChar(*msg++);
    }
}

// Map Device Registers and Shared Memory
static int map_resources(vspace_t *vspace, vka_t *vka) {
    ps_io_mapper_t io_mapper;
    int error = sel4platsupport_new_io_mapper(vspace, vka, &io_mapper);
    if (error) return -1;

    // 1. Map MMIO Registers
    mmio_base = (volatile uint32_t*)ps_io_map(&io_mapper, VIRTIO_MMIO_PADDR, VIRTIO_MMIO_SIZE, 0, PS_MEM_NORMAL);
    if (!mmio_base) return -2;

    // 2. Map Shared Memory (DMA Area)
    shmem_base = (volatile char*)ps_io_map(&io_mapper, VIRTIO_SHARED_MEM_PADDR, VIRTIO_SHARED_MEM_SIZE, 0, PS_MEM_NORMAL);
    if (!shmem_base) return -3;

    // Clear shared memory to be safe
    memset((void*)shmem_base, 0, VIRTIO_SHARED_MEM_SIZE);

    return 0;
}

int virtio_driver_init(vspace_t *vspace, vka_t *vka) {
    if (map_resources(vspace, vka) != 0) {
        debug_log("VirtIO: Map Failed\n");
        return -1;
    }

    // Check Magic
    if (vt_read32(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976) {
        debug_log("VirtIO: Bad Magic\n");
        return -2;
    }

    // 1. Reset Device
    vt_write32(VIRTIO_MMIO_STATUS, 0);

    // 2. Set ACKNOWLEDGE & DRIVER
    vt_write32(VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    // 3. Negotiate Features
    uint32_t host_features = vt_read32(VIRTIO_MMIO_HOST_FEATURES);
    // For now, accept what host offers (Warning: Production code should filter this!)
    vt_write32(VIRTIO_MMIO_GUEST_FEATURES, host_features);
    
    // 4. Set FEATURES_OK
    vt_write32(VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_FEATURES_OK);
    
    // Check if device accepted features
    if (!(vt_read32(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK)) {
        debug_log("VirtIO: Features Rejected\n");
        return -3;
    }

    // 5. Setup Queue 1 (Console Transmit Queue)
    // Console has multiple queues: 0=Receive, 1=Transmit, 2=Control Rx, 3=Control Tx
    int queue_index = 1; 
    
    vt_write32(VIRTIO_MMIO_QUEUE_SEL, queue_index);

    if (vt_read32(VIRTIO_MMIO_QUEUE_NUM_MAX) == 0) {
        debug_log("VirtIO: Queue 1 Not Available\n");
        return -4;
    }

    vt_write32(VIRTIO_MMIO_QUEUE_NUM, VQ_NUM);
    vt_write32(VIRTIO_MMIO_QUEUE_ALIGN, VQ_ALIGN);

    // Set Queue PFN (Physical Frame Number)
    // IMPORTANT: We use VIRTIO_SHARED_MEM_PADDR as the start of our Vring
    // Layout: Descriptors -> Avail -> Padding -> Used
    // To match alignment, we just point PFN to the start of shared RAM
    vt_write32(VIRTIO_MMIO_QUEUE_PFN, VIRTIO_SHARED_MEM_PADDR >> 12);

    // 6. Set DRIVER_OK
    vt_write32(VIRTIO_MMIO_STATUS, vt_read32(VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_DRIVER_OK);

    debug_log("VirtIO: Initialized OK\n");
    return 0;
}

// Send a simple string message via VirtIO Console
int virtio_console_send(const char *msg) {
    if (!shmem_base) return -1;
    
    int len = strlen(msg);
    if (len > 100) len = 100; // Cap size for demo

    // 1. Place Data in Shared Memory
    // OFFSET adjustment: 
    // Desc table (0x0)
    // Avail Ring (0x100)
    // Used Ring (0x1000 - ALIGNED to 4096)
    // Payload Data (0x2000 - Safer offset)
    volatile char *data_area = (volatile char*)(shmem_base + 0x2000);
    
    // Manual memcpy for volatile memory
    for (int i = 0; i < len; i++) {
        data_area[i] = msg[i];
    }

    // 2. Setup Descriptor [0]
    struct vring_desc *desc = (struct vring_desc *)shmem_base;
    desc[0].addr = VIRTIO_SHARED_MEM_PADDR + 0x2000; // Must be PHYSICAL address
    desc[0].len = len;
    desc[0].flags = 0; // No next descriptor
    desc[0].next = 0;

    // 3. Update Avail Ring
    // Determine ring offset based on alignment
    // Simple calculation for VQ_NUM=16, ALIGN=4096
    // Avail Ring is at offset 16*16 = 256 (0x100)
    struct vring_avail *avail = (struct vring_avail *)(shmem_base + 0x100);
    
    int idx = avail->idx % VQ_NUM;
    avail->ring[idx] = 0; // Use Descriptor 0

    // Memory Barrier
    __asm__ volatile("dmb ish" ::: "memory");

    avail->idx++; // Increment Available Index

    // Memory Barrier
    __asm__ volatile("dmb ish" ::: "memory");

    // 4. Notify Device (Kick Queue 1)
    vt_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    return 0;
}

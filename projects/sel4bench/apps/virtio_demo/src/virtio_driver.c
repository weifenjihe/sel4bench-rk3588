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

// VirtIO MMIO register offsets (MMIO v2 / modern)
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

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
// Split ring/data layout in shared memory (must stay within 64KB region).
#define VQ_DESC_OFFSET   0x0000
#define VQ_AVAIL_OFFSET  0x1000
#define VQ_USED_OFFSET   0x2000
#define VQ_DATA_OFFSET   0x3000

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

static void vt_write64(int low_off, int high_off, uint64_t val) {
    vt_write32(low_off, (uint32_t)(val & 0xffffffffu));
    vt_write32(high_off, (uint32_t)(val >> 32));
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

        // Check Version
    uint32_t version = vt_read32(VIRTIO_MMIO_VERSION);
        if (version != 2) {
           debug_log("VirtIO: Need MMIO Version 2\n");
           return -3;
    }

    // 1. Reset Device
    vt_write32(VIRTIO_MMIO_STATUS, 0);

    // 2. Set ACKNOWLEDGE & DRIVER
    vt_write32(VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    // 3. Negotiate features (use low 32-bit page, currently accept none).
    vt_write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    (void)vt_read32(VIRTIO_MMIO_DEVICE_FEATURES);
    vt_write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vt_write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);

    // Tell the device that feature negotiation is complete.
    vt_write32(VIRTIO_MMIO_STATUS,
               vt_read32(VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_FEATURES_OK);
    if ((vt_read32(VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK) == 0) {
        debug_log("VirtIO: FEATURES_OK rejected\n");
        return -4;
    }

    // 4. Setup Queue 1 (Console Transmit Queue)
    int queue_index = 1; 
    
    vt_write32(VIRTIO_MMIO_QUEUE_SEL, queue_index);

    if (vt_read32(VIRTIO_MMIO_QUEUE_NUM_MAX) == 0) {
        debug_log("VirtIO: Queue 1 Not Available\n");
        return -5;
    }

    vt_write32(VIRTIO_MMIO_QUEUE_NUM, VQ_NUM);

    // 4.1 Program split ring addresses with modern MMIO registers.
    uint64_t desc_pa = VIRTIO_SHARED_MEM_PADDR + VQ_DESC_OFFSET;
    uint64_t avail_pa = VIRTIO_SHARED_MEM_PADDR + VQ_AVAIL_OFFSET;
    uint64_t used_pa = VIRTIO_SHARED_MEM_PADDR + VQ_USED_OFFSET;

    vt_write64(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH,
               desc_pa);
    vt_write64(VIRTIO_MMIO_QUEUE_AVAIL_LOW, VIRTIO_MMIO_QUEUE_AVAIL_HIGH,
               avail_pa);
    vt_write64(VIRTIO_MMIO_QUEUE_USED_LOW, VIRTIO_MMIO_QUEUE_USED_HIGH,
               used_pa);
    vt_write32(VIRTIO_MMIO_QUEUE_READY, 1);

    // 5. Set DRIVER_OK
    vt_write32(VIRTIO_MMIO_STATUS, vt_read32(VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_DRIVER_OK);

    debug_log("VirtIO: Initialized OK (MMIO v2)\n");
    return 0;
}

// Send a simple string message via VirtIO Console
int virtio_console_send(const char *msg) {
    if (!shmem_base) return -1;
    
    int len = strlen(msg);
    if (len > 100) len = 100; // Cap size for demo

    // 1. Place data into payload area.
    volatile char *data_area = (volatile char*)(shmem_base + VQ_DATA_OFFSET);
    
    for (int i = 0; i < len; i++) {
        data_area[i] = msg[i];
    }

    // 2. Setup Descriptor [0]
    struct vring_desc *desc = (struct vring_desc *)(shmem_base + VQ_DESC_OFFSET);
    desc[0].addr = VIRTIO_SHARED_MEM_PADDR + VQ_DATA_OFFSET;
    desc[0].len = len;
    desc[0].flags = 0; // No next descriptor
    desc[0].next = 0;

    // 3. Update avail ring for queue kick.
    struct vring_avail *avail = (struct vring_avail *)(shmem_base + VQ_AVAIL_OFFSET);
    
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

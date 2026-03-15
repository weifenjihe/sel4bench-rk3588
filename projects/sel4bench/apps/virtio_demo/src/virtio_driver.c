#include <autoconf.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sel4/sel4.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/io.h>
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

#ifndef VRING_AVAIL_F_NO_INTERRUPT
#define VRING_AVAIL_F_NO_INTERRUPT 1
#endif

// Configuration
#define VQ_NUM_MAX 16
#define VQ_DATA_SIZE 256

// Queue 0 (RX from Linux -> seL4) shared-memory layout.
#define VQ0_DESC_OFFSET   0x0000
#define VQ0_AVAIL_OFFSET  0x1000
#define VQ0_USED_OFFSET   0x2000
#define VQ0_DATA_OFFSET   0x3000

// Queue 1 (TX from seL4 -> Linux) shared-memory layout.
#define VQ1_DESC_OFFSET   0x4000
#define VQ1_AVAIL_OFFSET  0x5000
#define VQ1_USED_OFFSET   0x6000
#define VQ1_DATA_OFFSET   0x7000

// Global pointers
static volatile uint32_t *mmio_base;
static volatile char *shmem_base;
static uint16_t rx_last_used_idx;
static uint16_t rx_queue_num;
static uint16_t tx_queue_num;

// MMIO helpers are used by queue setup helpers below.
static uint32_t vt_read32(int offset);
static void vt_write32(int offset, uint32_t val);
static void vt_write64(int low_off, int high_off, uint64_t val);

static int setup_queue_modern(int queue_index, uint32_t desc_off,
                              uint32_t avail_off, uint32_t used_off,
                              uint16_t *queue_num_out)
{
    vt_write32(VIRTIO_MMIO_QUEUE_SEL, (uint32_t)queue_index);
    uint32_t qmax = vt_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0) {
        return -1;
    }

    uint16_t qnum = (qmax < VQ_NUM_MAX) ? (uint16_t)qmax : (uint16_t)VQ_NUM_MAX;
    vt_write32(VIRTIO_MMIO_QUEUE_NUM, qnum);

    vt_write64(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH,
               VIRTIO_SHARED_MEM_PADDR + desc_off);
    vt_write64(VIRTIO_MMIO_QUEUE_AVAIL_LOW, VIRTIO_MMIO_QUEUE_AVAIL_HIGH,
               VIRTIO_SHARED_MEM_PADDR + avail_off);
    vt_write64(VIRTIO_MMIO_QUEUE_USED_LOW, VIRTIO_MMIO_QUEUE_USED_HIGH,
               VIRTIO_SHARED_MEM_PADDR + used_off);
    vt_write32(VIRTIO_MMIO_QUEUE_READY, 1);

    *queue_num_out = qnum;

    return 0;
}

static void prime_rx_queue(void)
{
    struct vring_desc *desc = (struct vring_desc *)(shmem_base + VQ0_DESC_OFFSET);
    struct vring_avail *avail = (struct vring_avail *)(shmem_base + VQ0_AVAIL_OFFSET);

    for (uint16_t i = 0; i < rx_queue_num; i++) {
        desc[i].addr = VIRTIO_SHARED_MEM_PADDR + VQ0_DATA_OFFSET + (i * VQ_DATA_SIZE);
        desc[i].len = VQ_DATA_SIZE;
        desc[i].flags = VRING_DESC_F_WRITE;
        desc[i].next = 0;
        avail->ring[i] = i;
    }

    avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    __asm__ volatile("dmb ish" ::: "memory");
    avail->idx = rx_queue_num;
    __asm__ volatile("dmb ish" ::: "memory");

    vt_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

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

    // Clear any stale interrupt status before queue setup.
    uint32_t isr = vt_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr) {
        vt_write32(VIRTIO_MMIO_INTERRUPT_ACK, isr);
    }

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

    // 4. Setup Queue 0 (RX) and Queue 1 (TX)
    if (setup_queue_modern(0, VQ0_DESC_OFFSET, VQ0_AVAIL_OFFSET, VQ0_USED_OFFSET,
                           &rx_queue_num) != 0) {
        debug_log("VirtIO: Queue 0 Not Available\n");
        return -5;
    }
    if (setup_queue_modern(1, VQ1_DESC_OFFSET, VQ1_AVAIL_OFFSET, VQ1_USED_OFFSET,
                           &tx_queue_num) != 0) {
        debug_log("VirtIO: Queue 1 Not Available\n");
        return -6;
    }

    rx_last_used_idx = 0;
    prime_rx_queue();

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
    volatile char *data_area = (volatile char*)(shmem_base + VQ1_DATA_OFFSET);
    
    for (int i = 0; i < len; i++) {
        data_area[i] = msg[i];
    }

    // 2. Setup Descriptor [0]
    struct vring_desc *desc = (struct vring_desc *)(shmem_base + VQ1_DESC_OFFSET);
    desc[0].addr = VIRTIO_SHARED_MEM_PADDR + VQ1_DATA_OFFSET;
    desc[0].len = len;
    desc[0].flags = 0; // No next descriptor
    desc[0].next = 0;

    // 3. Update avail ring for queue kick.
    struct vring_avail *avail = (struct vring_avail *)(shmem_base + VQ1_AVAIL_OFFSET);

    // This demo is TX-only and does not bind an IRQ handler in userspace.
    // Ask device to suppress used-buffer interrupts.
    avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    
    int idx = avail->idx % tx_queue_num;
    avail->ring[idx] = 0; // Use Descriptor 0

    // Memory Barrier
    __asm__ volatile("dmb ish" ::: "memory");

    avail->idx++; // Increment Available Index

    // Memory Barrier
    __asm__ volatile("dmb ish" ::: "memory");

    // 4. Notify Device (Kick Queue 1)
    vt_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    // If backend still raises an interrupt, proactively ack to avoid IRQ storm.
    uint32_t isr = vt_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr) {
        vt_write32(VIRTIO_MMIO_INTERRUPT_ACK, isr);
    }

    return 0;
}

int virtio_console_recv(char *buf, size_t buf_sz) {
    if (!shmem_base || !buf || buf_sz == 0) {
        return -1;
    }

    struct vring_used *used = (struct vring_used *)(shmem_base + VQ0_USED_OFFSET);
    if (rx_last_used_idx == used->idx) {
        return 0;
    }

    struct vring_used_elem elem = used->ring[rx_last_used_idx % rx_queue_num];
    uint32_t copy_len = elem.len;
    if (copy_len >= buf_sz) {
        copy_len = (uint32_t)buf_sz - 1;
    }
    if (copy_len > VQ_DATA_SIZE) {
        copy_len = VQ_DATA_SIZE;
    }

    uint32_t desc_id = elem.id % rx_queue_num;
    volatile char *data_area = (volatile char *)(shmem_base + VQ0_DATA_OFFSET + (desc_id * VQ_DATA_SIZE));
    for (uint32_t i = 0; i < copy_len; i++) {
        buf[i] = data_area[i];
    }
    buf[copy_len] = '\0';

    rx_last_used_idx++;

    // Re-post the RX descriptor to keep receiving data.
    struct vring_avail *avail = (struct vring_avail *)(shmem_base + VQ0_AVAIL_OFFSET);
    uint16_t aidx = avail->idx % rx_queue_num;
    avail->ring[aidx] = (uint16_t)elem.id;
    __asm__ volatile("dmb ish" ::: "memory");
    avail->idx++;
    __asm__ volatile("dmb ish" ::: "memory");
    vt_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint32_t isr = vt_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr) {
        vt_write32(VIRTIO_MMIO_INTERRUPT_ACK, isr);
    }

    return (int)copy_len;
}

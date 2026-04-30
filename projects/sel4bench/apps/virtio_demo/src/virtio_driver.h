#ifndef VIRTIO_DRIVER_H
#define VIRTIO_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <autoconf.h>

// Configuration - Adjust based on your system layout
#if defined(CONFIG_PLAT_HIFIVE_P550)
#define VIRTIO_MMIO_PADDR         0x10007000
#define VIRTIO_MMIO_SIZE          0x1000
#else
#define VIRTIO_MMIO_PADDR         0xff9e0000
#define VIRTIO_MMIO_SIZE          0x200
#endif
#define VIRTIO_SHARED_MEM_PADDR   0x09400000  // Shared memory for DMA/Vrings
#define VIRTIO_SHARED_MEM_SIZE    0x10000     // 64KB

// Initialize and setup the VirtIO device
int virtio_driver_init(vspace_t *vspace, vka_t *vka);

// Send a simple string message via VirtIO Console
int virtio_console_send(const char *msg);

// Poll receive path. Returns >0 bytes received, 0 if no data, <0 on error.
int virtio_console_recv(char *buf, size_t buf_sz);

#endif // VIRTIO_DRIVER_H

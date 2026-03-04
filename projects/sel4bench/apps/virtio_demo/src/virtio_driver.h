#ifndef VIRTIO_DRIVER_H
#define VIRTIO_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>

// Configuration - Adjust based on your system layout
#define VIRTIO_MMIO_PADDR         0xff9e0000 
#define VIRTIO_MMIO_SIZE          0x200
#define VIRTIO_SHARED_MEM_PADDR   0x09400000  // Shared memory for DMA/Vrings
#define VIRTIO_SHARED_MEM_SIZE    0x10000     // 64KB

// Initialize and setup the VirtIO device
int virtio_driver_init(vspace_t *vspace, vka_t *vka);

// Send a simple string message via VirtIO Console
int virtio_console_send(const char *msg);

#endif // VIRTIO_DRIVER_H

# Linux Side: VirtIO Console Communication

When the Hypervisor is configured correctly, it exposes a `virtio-console` device to the Linux Guest.

## 1. Kernel Configuration
Ensure your Linux Guest Kernel has the following enabled:
```
CONFIG_VIRTIO=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_MMIO=y (or PCI if using PCI transport)
```

## 2. Device Presence
Upon boot, check dmesg:
```bash
dmesg | grep virtio
```
You should see a console device being initialized.

The device usually appears as:
- `/dev/hvc0` (if it's the primary console)
- `/dev/vport0p1` (if it's a secondary generic port)

## 3. Communication Application
You don't need a complex custom driver. You can treat the port as a standard character device.

### Shell Example
**Read from seL4:**
```bash
cat /dev/vport0p1
```

**Write to seL4:**
```bash
echo "Hello seL4" > /dev/vport0p1
```

### C Application Example
```c
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int main() {
    int fd = open("/dev/vport0p1", O_RDWR);
    if (fd < 0) {
        perror("Failed to open virtio port");
        return 1;
    }

    char *msg = "Hello from Linux Guest!\n";
    write(fd, msg, strlen(msg));

    char buffer[128];
    while(1) {
        int len = read(fd, buffer, sizeof(buffer)-1);
        if (len > 0) {
            buffer[len] = 0;
            printf("Received: %s\n", buffer);
        }
    }
    close(fd);
    return 0;
}
```

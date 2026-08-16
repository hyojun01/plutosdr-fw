/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <rte/mmio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct mathworks_ip_reg_info {
    size_t size;
    void *phys;
};

/*
 * The MathWorks UAPI encodes a pointer-sized ioctl payload. Keep this
 * definition identical to linux/include/linux/mathworks/mathworks_ip_ioctl.h.
 */
#define MATHWORKS_IP_IOCTL_MAGIC 'B'
#define MATHWORKS_IP_REG_INFO \
    _IOWR(MATHWORKS_IP_IOCTL_MAGIC, 2, struct mathworks_ip_reg_info *)

static void set_error(struct rte_error *error, enum rte_error_code code,
                      const char *field, const char *format, ...)
{
    va_list args;

    if (error == NULL)
        return;
    error->code = code;
    snprintf(error->field, sizeof(error->field), "%s",
             field != NULL ? field : "");
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

static int mmio_read32(void *context, uint32_t offset, uint32_t *value)
{
    struct rte_mmio *mmio = context;

    if (mmio == NULL || value == NULL || (offset & 3U) != 0 ||
        offset > mmio->size - sizeof(uint32_t))
        return -EINVAL;
    __sync_synchronize();
    *value = mmio->registers[offset / sizeof(uint32_t)];
    __sync_synchronize();
    return 0;
}

static int mmio_write32(void *context, uint32_t offset, uint32_t value)
{
    struct rte_mmio *mmio = context;

    if (mmio == NULL || (offset & 3U) != 0 ||
        offset > mmio->size - sizeof(uint32_t))
        return -EINVAL;
    mmio->registers[offset / sizeof(uint32_t)] = value;
    __sync_synchronize();
    return 0;
}

int rte_mmio_open(struct rte_mmio *mmio, const char *device_path,
                  struct rte_error *error)
{
    struct mathworks_ip_reg_info info = {0};
    void *mapping;
    int saved_errno;

    rte_error_clear(error);
    if (mmio == NULL || device_path == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "device",
                  "MMIO object and device path are required");
        return -EINVAL;
    }

    memset(mmio, 0, sizeof(*mmio));
    mmio->fd = -1;
    mmio->fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (mmio->fd < 0) {
        saved_errno = errno;
        set_error(error, RTE_ERROR_IO, "device",
                  "cannot open %s: %s", device_path,
                  strerror(saved_errno));
        return -saved_errno;
    }

    if (ioctl(mmio->fd, MATHWORKS_IP_REG_INFO, &info) != 0) {
        saved_errno = errno;
        set_error(error, RTE_ERROR_IO, "device",
                  "MATHWORKS_IP_REG_INFO failed: %s",
                  strerror(saved_errno));
        rte_mmio_close(mmio);
        return -saved_errno;
    }
    if ((uintptr_t)info.phys != (uintptr_t)RTE_REGISTER_BASE_PHYS ||
        info.size != (size_t)RTE_REGISTER_MAP_SIZE) {
        set_error(error, RTE_ERROR_HARDWARE, "register_resource",
                  "unexpected register resource base=0x%llx size=0x%zx; "
                  "expected base=0x%x size=0x%x",
                  (unsigned long long)(uintptr_t)info.phys, info.size,
                  RTE_REGISTER_BASE_PHYS, RTE_REGISTER_MAP_SIZE);
        rte_mmio_close(mmio);
        return -ENODEV;
    }

    mapping = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   mmio->fd, 0);
    if (mapping == MAP_FAILED) {
        saved_errno = errno;
        set_error(error, RTE_ERROR_IO, "device",
                  "mmap of %s failed: %s", device_path,
                  strerror(saved_errno));
        rte_mmio_close(mmio);
        return -saved_errno;
    }

    mmio->size = info.size;
    mmio->physical_base = (uintptr_t)info.phys;
    mmio->registers = mapping;
    mmio->io.context = mmio;
    mmio->io.read32 = mmio_read32;
    mmio->io.write32 = mmio_write32;
    return 0;
}

void rte_mmio_close(struct rte_mmio *mmio)
{
    if (mmio == NULL)
        return;
    if (mmio->registers != NULL && mmio->size != 0)
        (void)munmap((void *)mmio->registers, mmio->size);
    if (mmio->fd >= 0)
        (void)close(mmio->fd);
    memset(mmio, 0, sizeof(*mmio));
    mmio->fd = -1;
}

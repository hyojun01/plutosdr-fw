/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_MMIO_H
#define RTE_MMIO_H

#include <stddef.h>
#include <stdint.h>

#include <rte/register_io.h>

struct rte_mmio {
    int fd;
    size_t size;
    uintptr_t physical_base;
    volatile uint32_t *registers;
    struct rte_register_io io;
};

int rte_mmio_open(struct rte_mmio *mmio, const char *device_path,
                  struct rte_error *error);
void rte_mmio_close(struct rte_mmio *mmio);

#endif

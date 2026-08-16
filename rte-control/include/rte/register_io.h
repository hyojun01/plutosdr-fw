/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_REGISTER_IO_H
#define RTE_REGISTER_IO_H

#include <stdbool.h>
#include <stdint.h>

#include <rte/model.h>

struct rte_register_io {
    void *context;
    int (*read32)(void *context, uint32_t offset, uint32_t *value);
    int (*write32)(void *context, uint32_t offset, uint32_t value);
};

int rte_register_read_image(const struct rte_register_io *io,
                            struct rte_register_image *image,
                            struct rte_error *error);

int rte_register_apply(const struct rte_register_io *io,
                       const struct rte_register_image *image,
                       const struct rte_register_image *rollback_image,
                       struct rte_error *error);

int rte_register_validate_hardware(const struct rte_register_io *io,
                                   uint32_t expected_timestamp,
                                   struct rte_error *error);

#endif

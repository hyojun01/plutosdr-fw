/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_ABI_H
#define RTE_ABI_H

#include <stddef.h>
#include <stdint.h>

#define RTE_REGISTER_BASE_PHYS UINT32_C(0x43c00000)
#define RTE_REGISTER_MAP_SIZE  UINT32_C(0x00010000)
#define RTE_EXPECTED_TIMESTAMP UINT32_C(2608142020)
#define RTE_DELAY_MAX_SAMPLES  UINT16_C(1023)

enum rte_register_offset {
    RTE_REG_IPCORE_RESET       = 0x000,
    RTE_REG_IPCORE_ENABLE      = 0x004,
    RTE_REG_IPCORE_TIMESTAMP   = 0x008,
    RTE_REG_DELAY_OFFSET       = 0x100,
    RTE_REG_DOPPLER_PINC       = 0x104,
    RTE_REG_DOPPLER_PHASE      = 0x108,
    RTE_REG_SCALING            = 0x10c,
    RTE_REG_MICRO_PINC_RESP    = 0x110,
    RTE_REG_MICRO_PHASE_RESP   = 0x114,
    RTE_REG_MICRO_GAIN_RESP    = 0x118,
    RTE_REG_MICRO_PINC_HEART   = 0x11c,
    RTE_REG_MICRO_PHASE_HEART  = 0x120,
    RTE_REG_MICRO_GAIN_HEART   = 0x124,
    RTE_REG_LOAD_PARAM         = 0x128,
};

#define RTE_MASK_BIT              UINT32_C(0x00000001)
#define RTE_MASK_DELAY            UINT32_C(0x0000ffff)
#define RTE_MASK_SCALING          UINT32_C(0x0000ffff)
#define RTE_MASK_MICRO_GAIN       UINT32_C(0x01ffffff)
#define RTE_MASK_WORD             UINT32_C(0xffffffff)

struct rte_register_descriptor {
    const char *name;
    uint32_t offset;
    uint32_t mask;
};

extern const struct rte_register_descriptor
    rte_parameter_registers[10];
extern const size_t rte_parameter_register_count;

#endif

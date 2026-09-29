/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_ABI_H
#define RTE_ABI_H
#include <stdint.h>
#define RTE_REGISTER_BASE_PHYS UINT32_C(0x43c00000)
#define RTE_REGISTER_MAP_SIZE UINT32_C(0x10000)
#define RTE_EXPECTED_TIMESTAMP UINT32_C(2609182026)
#define RTE_FIXED_SAMPLE_RATE_HZ UINT64_C(61440000)
#define RTE_TARGET_COUNT 4U
#define RTE_PARAMETER_REGISTER_COUNT 6U
#define RTE_TARGET_REGISTER_COUNT 7U
#define RTE_DELAY_MAX_SAMPLES 1023U
#define RTE_DELAY_MAX_Q 65535U
#define RTE_MASK_BIT UINT32_C(1)
#define RTE_MASK_DELAY UINT32_C(0x3ff)
/* Native FD is ufix8_En6; the physical-input encoder emits only 0..63. */
#define RTE_MASK_FRACTION UINT32_C(0xff)
#define RTE_MASK_SCALING UINT32_C(0x1ffffff)
#define RTE_MASK_WORD UINT32_C(0xffffffff)
enum rte_register_offset {
    RTE_REG_IPCORE_RESET = 0x000,
    RTE_REG_IPCORE_ENABLE = 0x004,
    RTE_REG_IPCORE_TIMESTAMP = 0x008
};
enum rte_parameter_index {
    RTE_PARAM_ID, RTE_PARAM_FD, RTE_PARAM_FRQ, RTE_PARAM_PHOF,
    RTE_PARAM_SC, RTE_PARAM_EN, RTE_PARAM_LP
};
struct rte_register_descriptor {
    const char *name;
    uint32_t offset;
    uint32_t mask;
};
extern const struct rte_register_descriptor
    rte_target_registers[RTE_TARGET_COUNT][RTE_TARGET_REGISTER_COUNT];
#endif

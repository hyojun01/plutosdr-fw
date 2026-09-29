/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_CONTROLLER_H
#define RTE_CONTROLLER_H
#include <pthread.h>
#include <rte/ad936x.h>
#include <rte/mmio.h>

struct rte_rf_patch {
    bool has_carrier_hz, has_bandwidth_hz, has_tx_gain_db, has_rx_gain_db;
    uint64_t carrier_hz, bandwidth_hz;
    double tx_gain_db, rx_gain_db;
};
struct rte_target_state {
    bool has_config, hardware_state_known, needs_reapply;
    uint64_t encoded_carrier_hz;
    struct rte_config requested;
    struct rte_register_image image;
    struct rte_applied applied;
};
struct rte_snapshot {
    uint64_t revision, last_hardware_check_ms; /* CLOCK_MONOTONIC milliseconds */
    uint32_t hardware_build_id;
    bool degraded;
    struct rte_rf_context rf;
    struct rte_target_state targets[RTE_TARGET_COUNT];
};
struct rte_controller {
    pthread_mutex_t mutex;
    bool mutex_initialized;
    struct rte_mmio mmio;
    struct rte_ad936x ad936x;
    struct rte_snapshot state;
};
int rte_controller_open(struct rte_controller *, const char *, struct rte_error *);
void rte_controller_close(struct rte_controller *);
int rte_controller_snapshot(struct rte_controller *, struct rte_snapshot *, struct rte_error *);
int rte_controller_rf_capabilities(struct rte_controller *, struct rte_rf_capabilities *, struct rte_error *);
int rte_controller_apply_rf(struct rte_controller *, const struct rte_rf_patch *, bool, uint64_t,
                            struct rte_snapshot *, struct rte_error *);
int rte_controller_apply_target(struct rte_controller *, unsigned int, const struct rte_config *, bool, uint64_t,
                                struct rte_snapshot *, struct rte_error *);
#endif

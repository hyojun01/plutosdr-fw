/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_CONTROLLER_H
#define RTE_CONTROLLER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <rte/ad936x.h>
#include <rte/mmio.h>

struct rte_motion_patch {
    bool has_amplitude_m;
    bool has_frequency_hz;
    bool has_phase_rad;
    double amplitude_m;
    double frequency_hz;
    double phase_rad;
};

struct rte_config_patch {
    bool has_range_m;
    bool has_radial_velocity_mps;
    bool has_loss_db;
    double range_m;
    double radial_velocity_mps;
    double loss_db;
    struct rte_motion_patch respiration;
    struct rte_motion_patch heartbeat;
};

struct rte_rf_patch {
    bool has_sample_rate_hz;
    bool has_carrier_hz;
    uint64_t sample_rate_hz;
    uint64_t carrier_hz;
};

struct rte_snapshot {
    uint64_t revision;
    uint32_t hardware_build_id;
    bool has_config;
    bool degraded;
    struct rte_rf_context rf;
    struct rte_config requested;
    struct rte_register_image image;
    struct rte_applied applied;
};

struct rte_controller {
    pthread_mutex_t mutex;
    bool mutex_initialized;
    struct rte_mmio mmio;
    struct rte_ad936x ad936x;
    struct rte_snapshot state;
};

int rte_controller_open(struct rte_controller *controller,
                        const char *device_path,
                        struct rte_error *error);
void rte_controller_close(struct rte_controller *controller);

int rte_controller_snapshot(struct rte_controller *controller,
                            struct rte_snapshot *snapshot,
                            struct rte_error *error);

int rte_controller_apply(struct rte_controller *controller,
                         const struct rte_rf_patch *rf_patch,
                         const struct rte_config_patch *config_patch,
                         bool has_expected_revision,
                         uint64_t expected_revision,
                         struct rte_snapshot *snapshot,
                         struct rte_error *error);

void rte_config_to_patch(const struct rte_config *config,
                         struct rte_config_patch *patch);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_MODEL_H
#define RTE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include <rte/abi.h>

#define RTE_SPEED_OF_LIGHT_MPS 299792458.0
#define RTE_PHASE_MODULUS       4294967296.0

enum rte_error_code {
    RTE_ERROR_NONE = 0,
    RTE_ERROR_ARGUMENT,
    RTE_ERROR_RANGE,
    RTE_ERROR_RF_CONTEXT,
    RTE_ERROR_QUANTIZATION,
    RTE_ERROR_ALIASING,
    RTE_ERROR_IO,
    RTE_ERROR_HARDWARE,
    RTE_ERROR_CONFLICT,
    RTE_ERROR_STATE,
};

struct rte_error {
    enum rte_error_code code;
    char field[64];
    char message[192];
};

struct rte_motion_config {
    double amplitude_m;
    double frequency_hz;
    double phase_rad;
};

struct rte_config {
    double range_m;
    double radial_velocity_mps;
    double loss_db;
    struct rte_motion_config respiration;
    struct rte_motion_config heartbeat;
};

struct rte_rf_context {
    uint64_t sample_rate_hz;
    uint64_t rx_lo_hz;
    uint64_t tx_lo_hz;
};

struct rte_register_image {
    uint16_t delay_offset;
    int32_t doppler_pinc;
    int32_t doppler_phase;
    int16_t scaling;
    int32_t micro_pinc_resp;
    int32_t micro_phase_resp;
    int32_t micro_gain_resp;
    int32_t micro_pinc_heart;
    int32_t micro_phase_heart;
    int32_t micro_gain_heart;
};

struct rte_motion_applied {
    double amplitude_m;
    double frequency_hz;
    double phase_rad;
    double phase_gain_cycles;
    double max_doppler_hz;
};

struct rte_applied {
    uint16_t delay_samples;
    double delay_range_m;
    double range_phase_rad;
    double doppler_hz;
    double radial_velocity_mps;
    int16_t scaling_raw;
    double linear_gain;
    bool muted;
    double loss_db;
    struct rte_motion_applied respiration;
    struct rte_motion_applied heartbeat;
};

struct rte_capabilities {
    double max_range_m;
    double max_abs_velocity_mps;
    double phase_resolution_rad;
    double frequency_resolution_hz;
    double max_motion_amplitude_m;
};

void rte_error_clear(struct rte_error *error);
const char *rte_error_code_name(enum rte_error_code code);

int rte_phase_word(double cycles, int32_t *word,
                   struct rte_error *error);
double rte_phase_word_to_cycles(int32_t word);
double rte_wrap_phase_rad(double phase_rad);

int rte_encode(const struct rte_config *config,
               const struct rte_rf_context *rf,
               struct rte_register_image *image,
               struct rte_applied *applied,
               struct rte_error *error);

int rte_capabilities_for_rf(const struct rte_rf_context *rf,
                            struct rte_capabilities *capabilities,
                            struct rte_error *error);

uint32_t rte_register_image_word(const struct rte_register_image *image,
                                 uint32_t offset);

#endif

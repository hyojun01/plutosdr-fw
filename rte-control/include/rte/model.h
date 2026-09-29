/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_MODEL_H
#define RTE_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <rte/abi.h>
#define RTE_SPEED_OF_LIGHT_MPS 299792458.0
#define RTE_PHASE_MODULUS 4294967296.0
enum rte_error_code {
    RTE_ERROR_NONE = 0, RTE_ERROR_ARGUMENT, RTE_ERROR_RANGE,
    RTE_ERROR_RF_CONTEXT, RTE_ERROR_QUANTIZATION, RTE_ERROR_ALIASING,
    RTE_ERROR_IO, RTE_ERROR_HARDWARE, RTE_ERROR_CONFLICT, RTE_ERROR_STATE
};
struct rte_error { enum rte_error_code code; char field[64]; char message[192]; };
struct rte_config {
    bool enabled;
    double range_m, radial_velocity_mps, gain_linear, phase_offset_deg;
};
struct rte_rf_context {
    uint64_t sample_rate_hz, rx_lo_hz, tx_lo_hz;
    uint64_t rx_bandwidth_hz, tx_bandwidth_hz;
    int32_t tx_gain_mdb, rx_gain_mdb;
    bool manual_gain;
    char compatible[32];
};
struct rte_register_image { uint32_t words[RTE_PARAMETER_REGISTER_COUNT]; };
struct rte_applied {
    double delay_samples, delay_range_m, range_phase_rad, doppler_hz;
    double radial_velocity_mps, linear_gain;
    bool enabled;
};
struct rte_capabilities {
    double min_range_m, max_range_m, max_abs_velocity_mps;
    double range_resolution_m, phase_resolution_rad, frequency_resolution_hz;
    double max_gain_linear, fixed_latency_samples;
    bool rf_calibrated;
};
void rte_error_clear(struct rte_error *error);
const char *rte_error_code_name(enum rte_error_code code);
int rte_phase_word(double cycles, int32_t *word, struct rte_error *error);
double rte_phase_word_to_cycles(int32_t word);
double rte_wrap_phase_rad(double phase_rad);
int rte_encode(const struct rte_config *config, const struct rte_rf_context *rf,
               struct rte_register_image *image, struct rte_applied *applied,
               struct rte_error *error);
int rte_capabilities_for_rf(const struct rte_rf_context *rf,
                            struct rte_capabilities *capabilities,
                            struct rte_error *error);
#endif

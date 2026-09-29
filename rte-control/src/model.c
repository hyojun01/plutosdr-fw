/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/model.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* Timestamp-specific digital DUT input/output profile. See tests/rtl. */
#define RTE_FIXED_LATENCY_SAMPLES 18.0
const struct rte_register_descriptor
rte_target_registers[RTE_TARGET_COUNT][RTE_TARGET_REGISTER_COUNT] = {
    {
        {"ID", 0x100, RTE_MASK_DELAY},
        {"FD", 0x110, RTE_MASK_FRACTION},
        {"FRQ", 0x120, RTE_MASK_WORD},
        {"PHOF", 0x130, RTE_MASK_WORD},
        {"SC", 0x140, RTE_MASK_SCALING},
        {"EN", 0x14c, RTE_MASK_BIT},
        {"LP", 0x160, RTE_MASK_BIT}
    },
    {
        {"ID", 0x104, RTE_MASK_DELAY},
        {"FD", 0x114, RTE_MASK_FRACTION},
        {"FRQ", 0x124, RTE_MASK_WORD},
        {"PHOF", 0x134, RTE_MASK_WORD},
        {"SC", 0x144, RTE_MASK_SCALING},
        {"EN", 0x150, RTE_MASK_BIT},
        {"LP", 0x164, RTE_MASK_BIT}
    },
    {
        {"ID", 0x108, RTE_MASK_DELAY},
        {"FD", 0x118, RTE_MASK_FRACTION},
        {"FRQ", 0x128, RTE_MASK_WORD},
        {"PHOF", 0x138, RTE_MASK_WORD},
        {"SC", 0x158, RTE_MASK_SCALING},
        {"EN", 0x154, RTE_MASK_BIT},
        {"LP", 0x168, RTE_MASK_BIT}
    },
    {
        {"ID", 0x10c, RTE_MASK_DELAY},
        {"FD", 0x11c, RTE_MASK_FRACTION},
        {"FRQ", 0x12c, RTE_MASK_WORD},
        {"PHOF", 0x13c, RTE_MASK_WORD},
        {"SC", 0x148, RTE_MASK_SCALING},
        {"EN", 0x15c, RTE_MASK_BIT},
        {"LP", 0x16c, RTE_MASK_BIT}
    }
};

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

void rte_error_clear(struct rte_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
}

const char *rte_error_code_name(enum rte_error_code code)
{
    switch (code) {
    case RTE_ERROR_NONE:
        return "none";
    case RTE_ERROR_ARGUMENT:
        return "invalid_argument";
    case RTE_ERROR_RANGE:
        return "out_of_range";
    case RTE_ERROR_RF_CONTEXT:
        return "invalid_rf_context";
    case RTE_ERROR_QUANTIZATION:
        return "quantization_error";
    case RTE_ERROR_ALIASING:
        return "aliasing";
    case RTE_ERROR_IO:
        return "io_error";
    case RTE_ERROR_HARDWARE:
        return "hardware_error";
    case RTE_ERROR_CONFLICT:
        return "revision_conflict";
    case RTE_ERROR_STATE:
        return "invalid_state";
    default:
        return "unknown";
    }
}

double rte_wrap_phase_rad(double phase_rad)
{
    double wrapped;

    if (!isfinite(phase_rad))
        return NAN;

    wrapped = fmod(phase_rad + M_PI, 2.0 * M_PI);
    if (wrapped < 0.0)
        wrapped += 2.0 * M_PI;
    return wrapped - M_PI;
}

int rte_phase_word(double cycles, int32_t *word,
                   struct rte_error *error)
{
    double wrapped;
    double scaled;
    long long rounded;
    uint64_t bits;
    int64_t signed_value;

    rte_error_clear(error);
    if (word == NULL || !isfinite(cycles)) {
        set_error(error, RTE_ERROR_ARGUMENT, "phase",
                  "phase cycles must be finite");
        return -EINVAL;
    }

    /*
     * This matches MATLAB:
     *   mod(round(cycles * 2^32), 2^32)
     * while first reducing the input to retain precision for large phases.
     * llround(), like MATLAB round(), resolves half values away from zero.
     */
    wrapped = fmod(cycles, 1.0);
    scaled = wrapped * RTE_PHASE_MODULUS;
    rounded = llround(scaled);
    bits = (uint64_t)rounded & UINT64_C(0xffffffff);
    signed_value = bits >= UINT64_C(0x80000000)
        ? (int64_t)bits - INT64_C(0x100000000)
        : (int64_t)bits;
    *word = (int32_t)signed_value;
    return 0;
}

double rte_phase_word_to_cycles(int32_t word)
{
    return (double)word / RTE_PHASE_MODULUS;
}


static int validate_rf(const struct rte_rf_context *rf, struct rte_error *error)
{
    if (rf == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "rf", "RF context is required");
        return -EINVAL;
    }
    if (rf->sample_rate_hz != RTE_FIXED_SAMPLE_RATE_HZ) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "sample_rate_hz",
                  "this hardware profile requires 61440000 samples/s");
        return -ERANGE;
    }
    if (rf->rx_lo_hz == 0 || rf->rx_lo_hz != rf->tx_lo_hz) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "carrier_hz",
                  "RX and TX LO must be identical and positive");
        return -ERANGE;
    }
    return 0;
}

int rte_capabilities_for_rf(const struct rte_rf_context *rf,
                            struct rte_capabilities *caps,
                            struct rte_error *error)
{
    double metres_per_sample = RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)RTE_FIXED_SAMPLE_RATE_HZ);
    int status;
    rte_error_clear(error);
    if (caps == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "capabilities", "output is required");
        return -EINVAL;
    }
    status = validate_rf(rf, error);
    if (status != 0) return status;
    memset(caps, 0, sizeof(*caps));
    caps->fixed_latency_samples = RTE_FIXED_LATENCY_SAMPLES;
    caps->min_range_m = RTE_FIXED_LATENCY_SAMPLES * metres_per_sample;
    caps->max_range_m = (RTE_FIXED_LATENCY_SAMPLES +
        (double)RTE_DELAY_MAX_Q / 64.0) * metres_per_sample;
    caps->range_resolution_m = metres_per_sample / 64.0;
    caps->frequency_resolution_hz = (double)rf->sample_rate_hz / RTE_PHASE_MODULUS;
    caps->phase_resolution_rad = 2.0 * M_PI / RTE_PHASE_MODULUS;
    caps->max_abs_velocity_mps = (double)INT32_MAX * caps->frequency_resolution_hz *
        RTE_SPEED_OF_LIGHT_MPS / (2.0 * (double)rf->rx_lo_hz);
    caps->max_gain_linear = (double)RTE_MASK_SCALING / 1048576.0;
    caps->rf_calibrated = false;
    return 0;
}

int rte_encode(const struct rte_config *config, const struct rte_rf_context *rf,
               struct rte_register_image *image, struct rte_applied *applied,
               struct rte_error *error)
{
    struct rte_capabilities caps;
    struct rte_register_image encoded = {{0}};
    struct rte_applied result = {0};
    long double cycles, wrapped, frequency_code;
    double doppler;
    long long q, pinc, scaling;
    uint64_t phase_bits;
    int status;
    rte_error_clear(error);
    if (config == NULL || image == NULL || applied == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "config", "config and outputs are required");
        return -EINVAL;
    }
    status = rte_capabilities_for_rf(rf, &caps, error);
    if (status != 0) return status;
    if (!isfinite(config->range_m) || config->range_m < caps.min_range_m ||
        config->range_m > caps.max_range_m) {
        set_error(error, RTE_ERROR_RANGE, "range_m",
                  "digital DUT distance must be in [%.12g, %.12g] m",
                  caps.min_range_m, caps.max_range_m);
        return -ERANGE;
    }
    if (!isfinite(config->radial_velocity_mps)) {
        set_error(error, RTE_ERROR_RANGE, "radial_velocity_mps", "velocity must be finite");
        return -ERANGE;
    }
    if (!isfinite(config->gain_linear) || config->gain_linear < 0.0 ||
        config->gain_linear > caps.max_gain_linear) {
        set_error(error, RTE_ERROR_RANGE, "gain_linear", "digital gain must be in [0, %.12g]", caps.max_gain_linear);
        return -ERANGE;
    }
    if (!isfinite(config->phase_offset_deg)) {
        set_error(error, RTE_ERROR_RANGE, "phase_offset_deg", "phase must be finite");
        return -ERANGE;
    }
    /* Quantize the complete delay before splitting it: carries cross FD/ID. */
    q = llround((config->range_m - caps.min_range_m) / caps.range_resolution_m);
    if (q < 0 || q > RTE_DELAY_MAX_Q) {
        set_error(error, RTE_ERROR_QUANTIZATION, "range_m", "delay word is outside the ABI");
        return -ERANGE;
    }
    doppler = -2.0 * config->radial_velocity_mps * (double)rf->rx_lo_hz /
        RTE_SPEED_OF_LIGHT_MPS;
    if (!isfinite(doppler) || fabs(doppler) >= (double)rf->sample_rate_hz / 2.0) {
        set_error(error, RTE_ERROR_ALIASING, "radial_velocity_mps", "Doppler must be strictly inside Nyquist");
        return -ERANGE;
    }
    frequency_code = (long double)doppler * 4294967296.0L / (long double)rf->sample_rate_hz;
    pinc = llroundl(frequency_code);
    if (pinc < INT32_MIN || pinc > INT32_MAX) {
        set_error(error, RTE_ERROR_QUANTIZATION, "radial_velocity_mps", "rounded Doppler does not fit int32");
        return -ERANGE;
    }
    /* Reduce user offset before addition, retaining distance-phase precision. */
    cycles = -2.0L * (long double)config->range_m * (long double)rf->rx_lo_hz /
        299792458.0L + fmodl((long double)config->phase_offset_deg, 360.0L) / 360.0L;
    wrapped = fmodl(cycles, 1.0L);
    if (wrapped < 0.0L) wrapped += 1.0L;
    phase_bits = (uint64_t)llroundl(wrapped * 4294967296.0L) & UINT64_C(0xffffffff);
    scaling = llround(config->gain_linear * 1048576.0);
    encoded.words[RTE_PARAM_ID] = (uint32_t)(q / 64);
    encoded.words[RTE_PARAM_FD] = (uint32_t)(q % 64);
    /* Conversion to unsigned is well-defined modulo 2^32, including negatives. */
    encoded.words[RTE_PARAM_FRQ] = (uint32_t)pinc;
    encoded.words[RTE_PARAM_PHOF] = (uint32_t)phase_bits;
    encoded.words[RTE_PARAM_SC] = (uint32_t)scaling;
    encoded.words[RTE_PARAM_EN] = config->enabled ? 1U : 0U;
    result.delay_samples = RTE_FIXED_LATENCY_SAMPLES + (double)q / 64.0;
    result.delay_range_m = result.delay_samples * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)rf->sample_rate_hz);
    result.range_phase_rad = rte_wrap_phase_rad((double)phase_bits / RTE_PHASE_MODULUS * 2.0 * M_PI);
    result.doppler_hz = (double)pinc * caps.frequency_resolution_hz;
    result.radial_velocity_mps = -result.doppler_hz * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)rf->rx_lo_hz);
    result.linear_gain = (double)scaling / 1048576.0;
    result.enabled = config->enabled;
    *image = encoded;
    *applied = result;
    return 0;
}

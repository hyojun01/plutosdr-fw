/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/model.h>

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const struct rte_register_descriptor rte_parameter_registers[10] = {
    {"delayOffset",    RTE_REG_DELAY_OFFSET,      RTE_MASK_DELAY},
    {"dopplerPINC",    RTE_REG_DOPPLER_PINC,      RTE_MASK_WORD},
    {"dopplerPhOffs",  RTE_REG_DOPPLER_PHASE,     RTE_MASK_WORD},
    {"scaling",        RTE_REG_SCALING,           RTE_MASK_SCALING},
    {"microPINCRes",   RTE_REG_MICRO_PINC_RESP,   RTE_MASK_WORD},
    {"microPhOffsRes", RTE_REG_MICRO_PHASE_RESP,  RTE_MASK_WORD},
    {"microGainRes",   RTE_REG_MICRO_GAIN_RESP,   RTE_MASK_MICRO_GAIN},
    {"microPINCHeart", RTE_REG_MICRO_PINC_HEART,  RTE_MASK_WORD},
    {"microPhOffsHeart", RTE_REG_MICRO_PHASE_HEART, RTE_MASK_WORD},
    {"microGainHeart", RTE_REG_MICRO_GAIN_HEART,  RTE_MASK_MICRO_GAIN},
};

const size_t rte_parameter_register_count =
    sizeof(rte_parameter_registers) / sizeof(rte_parameter_registers[0]);

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

static int validate_rf(const struct rte_rf_context *rf,
                       struct rte_error *error)
{
    if (rf == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "rf",
                  "RF context is required");
        return -EINVAL;
    }
    if (rf->sample_rate_hz == 0) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "sample_rate_hz",
                  "sample rate must be positive");
        return -ERANGE;
    }
    if (rf->rx_lo_hz == 0 || rf->tx_lo_hz == 0) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "carrier_hz",
                  "RX and TX LO frequencies must be positive");
        return -ERANGE;
    }
    if (rf->rx_lo_hz != rf->tx_lo_hz) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "carrier_hz",
                  "monostatic mode requires identical RX and TX LO "
                  "frequencies (RX=%llu, TX=%llu)",
                  (unsigned long long)rf->rx_lo_hz,
                  (unsigned long long)rf->tx_lo_hz);
        return -EINVAL;
    }
    return 0;
}

static int validate_motion(const struct rte_motion_config *motion,
                           const char *field, double sample_rate,
                           struct rte_error *error)
{
    if (!isfinite(motion->amplitude_m) || motion->amplitude_m < 0.0) {
        set_error(error, RTE_ERROR_RANGE, field,
                  "amplitude must be a finite, nonnegative distance");
        return -ERANGE;
    }
    if (!isfinite(motion->frequency_hz) ||
        motion->frequency_hz < 0.0 ||
        motion->frequency_hz >= sample_rate / 2.0) {
        set_error(error, RTE_ERROR_RANGE, field,
                  "frequency must be finite and in [0, sample_rate/2)");
        return -ERANGE;
    }
    if (!isfinite(motion->phase_rad)) {
        set_error(error, RTE_ERROR_RANGE, field,
                  "phase must be finite");
        return -ERANGE;
    }
    return 0;
}

static int quantize_motion(const struct rte_motion_config *motion,
                           double sample_rate, double wavelength,
                           int32_t *pinc, int32_t *phase, int32_t *gain,
                           struct rte_motion_applied *applied,
                           const char *field, struct rte_error *error)
{
    double gain_target;
    double gain_scaled;
    double wrapped_phase;
    long long gain_raw;
    int status;

    status = validate_motion(motion, field, sample_rate, error);
    if (status != 0)
        return status;

    status = rte_phase_word(motion->frequency_hz / sample_rate,
                            pinc, error);
    if (status != 0)
        return status;
    if (*pinc == INT32_MIN) {
        set_error(error, RTE_ERROR_ALIASING, field,
                  "frequency rounds to the Nyquist phase increment");
        return -ERANGE;
    }
    wrapped_phase = rte_wrap_phase_rad(motion->phase_rad);
    status = rte_phase_word(wrapped_phase / (2.0 * M_PI),
                            phase, error);
    if (status != 0)
        return status;

    gain_target = -2.0 * motion->amplitude_m / wavelength;
    if (!isfinite(gain_target) || gain_target < -512.0 ||
        gain_target > 16777215.0 / 32768.0) {
        set_error(error, RTE_ERROR_QUANTIZATION, field,
                  "phase-modulation gain does not fit sfix25_En15");
        return -ERANGE;
    }
    gain_scaled = gain_target * 32768.0;
    if (!isfinite(gain_scaled)) {
        set_error(error, RTE_ERROR_QUANTIZATION, field,
                  "phase-modulation gain is not finite");
        return -ERANGE;
    }
    gain_raw = llround(gain_scaled);
    *gain = (int32_t)gain_raw;

    applied->frequency_hz =
        rte_phase_word_to_cycles(*pinc) * sample_rate;
    applied->phase_rad =
        rte_phase_word_to_cycles(*phase) * 2.0 * M_PI;
    applied->phase_gain_cycles = (double)*gain / 32768.0;
    applied->amplitude_m =
        -0.5 * applied->phase_gain_cycles * wavelength;
    applied->max_doppler_hz =
        4.0 * M_PI * applied->amplitude_m *
        fabs(applied->frequency_hz) / wavelength;
    return 0;
}

int rte_encode(const struct rte_config *config,
               const struct rte_rf_context *rf,
               struct rte_register_image *image,
               struct rte_applied *applied,
               struct rte_error *error)
{
    double sample_rate;
    double carrier;
    double wavelength;
    double delay_exact;
    long long delay_samples;
    double range_phase;
    double doppler_hz;
    double linear_gain;
    double quantized_doppler_hz;
    long long scaling_raw;
    double max_instantaneous_hz;
    int status;

    rte_error_clear(error);
    if (config == NULL || image == NULL || applied == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "config",
                  "config, register image, and applied result are required");
        return -EINVAL;
    }
    status = validate_rf(rf, error);
    if (status != 0)
        return status;

    sample_rate = (double)rf->sample_rate_hz;
    carrier = (double)rf->rx_lo_hz;
    wavelength = RTE_SPEED_OF_LIGHT_MPS / carrier;

    if (!isfinite(config->range_m) || config->range_m < 0.0) {
        set_error(error, RTE_ERROR_RANGE, "range_m",
                  "range must be finite and nonnegative");
        return -ERANGE;
    }
    if (!isfinite(config->radial_velocity_mps)) {
        set_error(error, RTE_ERROR_RANGE, "radial_velocity_mps",
                  "radial velocity must be finite");
        return -ERANGE;
    }
    if (!isfinite(config->loss_db) || config->loss_db < 0.0) {
        set_error(error, RTE_ERROR_RANGE, "loss_db",
                  "loss must be a finite, nonnegative dB value");
        return -ERANGE;
    }

    delay_exact =
        2.0 * config->range_m * sample_rate / RTE_SPEED_OF_LIGHT_MPS;
    if (!isfinite(delay_exact) ||
        delay_exact > (double)RTE_DELAY_MAX_SAMPLES) {
        set_error(error, RTE_ERROR_RANGE, "range_m",
                  "range exceeds the %u-sample hardware limit",
                  RTE_DELAY_MAX_SAMPLES);
        return -ERANGE;
    }
    delay_samples = llround(delay_exact);
    if (delay_samples < 0 || delay_samples > RTE_DELAY_MAX_SAMPLES) {
        set_error(error, RTE_ERROR_RANGE, "range_m",
                  "range maps to %lld delay samples; hardware supports "
                  "0..%u", delay_samples, RTE_DELAY_MAX_SAMPLES);
        return -ERANGE;
    }

    range_phase = rte_wrap_phase_rad(
        -4.0 * M_PI * config->range_m / wavelength);
    doppler_hz =
        -2.0 * config->radial_velocity_mps / wavelength;
    if (!isfinite(doppler_hz) || fabs(doppler_hz) >= sample_rate / 2.0) {
        set_error(error, RTE_ERROR_ALIASING, "radial_velocity_mps",
                  "bulk Doppler %.9g Hz is outside Nyquist", doppler_hz);
        return -ERANGE;
    }

    memset(image, 0, sizeof(*image));
    memset(applied, 0, sizeof(*applied));
    image->delay_offset = (uint16_t)delay_samples;
    status = rte_phase_word(doppler_hz / sample_rate,
                            &image->doppler_pinc, error);
    if (status != 0)
        return status;
    if (image->doppler_pinc == INT32_MIN) {
        set_error(error, RTE_ERROR_ALIASING, "radial_velocity_mps",
                  "bulk Doppler rounds to the Nyquist phase increment");
        return -ERANGE;
    }
    quantized_doppler_hz =
        rte_phase_word_to_cycles(image->doppler_pinc) * sample_rate;
    status = rte_phase_word(range_phase / (2.0 * M_PI),
                            &image->doppler_phase, error);
    if (status != 0)
        return status;

    linear_gain = pow(10.0, -config->loss_db / 20.0);
    scaling_raw = llround(linear_gain * 32768.0);
    if (scaling_raw < 0)
        scaling_raw = 0;
    if (scaling_raw > 32767)
        scaling_raw = 32767;
    image->scaling = (int16_t)scaling_raw;

    status = quantize_motion(
        &config->respiration, sample_rate, wavelength,
        &image->micro_pinc_resp, &image->micro_phase_resp,
        &image->micro_gain_resp, &applied->respiration,
        "respiration", error);
    if (status != 0)
        return status;
    status = quantize_motion(
        &config->heartbeat, sample_rate, wavelength,
        &image->micro_pinc_heart, &image->micro_phase_heart,
        &image->micro_gain_heart, &applied->heartbeat,
        "heartbeat", error);
    if (status != 0)
        return status;

    max_instantaneous_hz =
        fabs(quantized_doppler_hz) +
        applied->respiration.max_doppler_hz +
        applied->heartbeat.max_doppler_hz;
    if (max_instantaneous_hz >= sample_rate / 2.0) {
        set_error(error, RTE_ERROR_ALIASING, "micro_motion",
                  "combined maximum instantaneous Doppler %.9g Hz "
                  "reaches Nyquist", max_instantaneous_hz);
        return -ERANGE;
    }

    applied->delay_samples = image->delay_offset;
    applied->delay_range_m =
        (double)image->delay_offset * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * sample_rate);
    applied->range_phase_rad =
        rte_phase_word_to_cycles(image->doppler_phase) * 2.0 * M_PI;
    applied->doppler_hz = quantized_doppler_hz;
    applied->radial_velocity_mps =
        -0.5 * applied->doppler_hz * wavelength;
    applied->scaling_raw = image->scaling;
    applied->linear_gain = (double)image->scaling / 32768.0;
    applied->muted = image->scaling == 0;
    applied->loss_db = applied->muted
        ? INFINITY
        : -20.0 * log10(applied->linear_gain);

    return 0;
}

int rte_capabilities_for_rf(const struct rte_rf_context *rf,
                            struct rte_capabilities *capabilities,
                            struct rte_error *error)
{
    double sample_rate;
    double wavelength;
    int status;

    rte_error_clear(error);
    if (capabilities == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "capabilities",
                  "capability output is required");
        return -EINVAL;
    }
    status = validate_rf(rf, error);
    if (status != 0)
        return status;

    sample_rate = (double)rf->sample_rate_hz;
    wavelength = RTE_SPEED_OF_LIGHT_MPS / (double)rf->rx_lo_hz;
    capabilities->max_range_m =
        RTE_DELAY_MAX_SAMPLES * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * sample_rate);
    /* INT32_MIN is deliberately rejected, so advertise a usable endpoint. */
    capabilities->max_abs_velocity_mps =
        RTE_SPEED_OF_LIGHT_MPS * sample_rate /
        (4.0 * (double)rf->rx_lo_hz) *
        (1.0 - 1.0 / 2147483648.0);
    capabilities->phase_resolution_rad =
        2.0 * M_PI / RTE_PHASE_MODULUS;
    capabilities->frequency_resolution_hz =
        sample_rate / RTE_PHASE_MODULUS;
    capabilities->max_motion_amplitude_m =
        0.5 * (16777216.0 / 32768.0) * wavelength;
    return 0;
}

uint32_t rte_register_image_word(const struct rte_register_image *image,
                                 uint32_t offset)
{
    if (image == NULL)
        return 0;

    switch (offset) {
    case RTE_REG_DELAY_OFFSET:
        return (uint32_t)image->delay_offset;
    case RTE_REG_DOPPLER_PINC:
        return (uint32_t)image->doppler_pinc;
    case RTE_REG_DOPPLER_PHASE:
        return (uint32_t)image->doppler_phase;
    case RTE_REG_SCALING:
        return (uint32_t)(uint16_t)image->scaling;
    case RTE_REG_MICRO_PINC_RESP:
        return (uint32_t)image->micro_pinc_resp;
    case RTE_REG_MICRO_PHASE_RESP:
        return (uint32_t)image->micro_phase_resp;
    case RTE_REG_MICRO_GAIN_RESP:
        return (uint32_t)image->micro_gain_resp;
    case RTE_REG_MICRO_PINC_HEART:
        return (uint32_t)image->micro_pinc_heart;
    case RTE_REG_MICRO_PHASE_HEART:
        return (uint32_t)image->micro_phase_heart;
    case RTE_REG_MICRO_GAIN_HEART:
        return (uint32_t)image->micro_gain_heart;
    default:
        return 0;
    }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/model.h>

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static unsigned int failures;

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                    \
                __FILE__, __LINE__, #condition);                          \
        failures++;                                                       \
    }                                                                     \
} while (0)

static void check_near(double actual, double expected, double tolerance,
                       const char *name)
{
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr,
                "%s: actual %.17g, expected %.17g, tolerance %.3g\n",
                name, actual, expected, tolerance);
        failures++;
    }
}

static void test_phase_words(void)
{
    struct rte_error error;
    int32_t word = 0;

    CHECK(rte_phase_word(0.0, &word, &error) == 0);
    CHECK(word == INT32_C(0));
    CHECK(rte_phase_word(0.25, &word, &error) == 0);
    CHECK(word == INT32_C(1073741824));
    CHECK(rte_phase_word(-0.25, &word, &error) == 0);
    CHECK(word == -INT32_C(1073741824));
    CHECK(rte_phase_word(0.5, &word, &error) == 0);
    CHECK(word == INT32_MIN);
    CHECK(rte_phase_word(1.25, &word, &error) == 0);
    CHECK(word == INT32_C(1073741824));
    CHECK(rte_phase_word(ldexp(1.0, -33), &word, &error) == 0);
    CHECK(word == INT32_C(1));
    CHECK(rte_phase_word(-ldexp(1.0, -33), &word, &error) == 0);
    CHECK(word == -INT32_C(1));
    CHECK(rte_phase_word(-0.5, &word, &error) == 0);
    CHECK(word == INT32_MIN);
    CHECK(rte_phase_word(1.0, &word, &error) == 0);
    CHECK(word == INT32_C(0));
    CHECK(rte_phase_word(NAN, &word, &error) != 0);
    CHECK(error.code == RTE_ERROR_ARGUMENT);
}

static void test_reference_configuration(void)
{
    const struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(61440000),
        .rx_lo_hz = UINT64_C(2450000000),
        .tx_lo_hz = UINT64_C(2450000000),
    };
    const struct rte_config config = {
        .range_m = 2.0,
        .radial_velocity_mps = -0.15,
        .loss_db = 12.0,
        .respiration = {
            .amplitude_m = 0.005,
            .frequency_hz = 0.25,
            .phase_rad = M_PI / 6.0,
        },
        .heartbeat = {
            .amplitude_m = 0.0005,
            .frequency_hz = 1.2,
            .phase_rad = M_PI / 2.0,
        },
    };
    struct rte_register_image image;
    struct rte_applied applied;
    struct rte_error error;

    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.delay_offset == 1);
    CHECK(image.doppler_pinc == 171);
    CHECK(image.doppler_phase == INT32_C(1334526528));
    CHECK(image.scaling == 8231);
    CHECK(image.micro_pinc_resp == 17);
    CHECK(image.micro_pinc_heart == 84);
    CHECK(image.micro_phase_resp == INT32_C(357913941));
    CHECK(image.micro_phase_heart == INT32_C(1073741824));
    CHECK(image.micro_gain_resp == -2678);
    CHECK(image.micro_gain_heart == -268);
    CHECK(rte_register_image_word(&image, RTE_REG_MICRO_GAIN_RESP) ==
          UINT32_C(0xfffff58a));
    CHECK(rte_register_image_word(&image, RTE_REG_MICRO_GAIN_HEART) ==
          UINT32_C(0xfffffef4));
    CHECK(applied.delay_samples == 1);
    check_near(applied.delay_range_m, 2.4397172688802082, 1e-14,
               "envelope range");
    check_near(applied.doppler_hz, 2.4461746215820312, 1e-14,
               "doppler");
    check_near(applied.radial_velocity_mps,
               -0.14966218418393817, 1e-14, "velocity");
    check_near(applied.loss_db, 11.999946665424783, 1e-13, "loss");
    check_near(applied.respiration.frequency_hz,
               0.24318695068359375, 1e-15, "respiration frequency");
    check_near(applied.respiration.amplitude_m,
               0.005000175647495815, 1e-17, "respiration amplitude");
    check_near(applied.heartbeat.frequency_hz,
               1.201629638671875, 1e-14, "heartbeat frequency");
    check_near(applied.heartbeat.amplitude_m,
               0.0005003909908621652, 1e-18, "heartbeat amplitude");
    check_near(applied.respiration.phase_rad, M_PI / 6.0, 2e-9,
               "respiration phase");
    check_near(applied.heartbeat.phase_rad, M_PI / 2.0, 2e-9,
               "heartbeat phase");
}

static struct rte_config zero_config(void)
{
    const struct rte_config config = {
        .range_m = 0.0,
        .radial_velocity_mps = 0.0,
        .loss_db = 0.0,
        .respiration = {0.0, 0.0, 0.0},
        .heartbeat = {0.0, 0.0, 0.0},
    };

    return config;
}

static void test_semantic_phase_wrapping(void)
{
    const struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(1000000),
        .rx_lo_hz = UINT64_C(2400000000),
        .tx_lo_hz = UINT64_C(2400000000),
    };
    struct rte_config config = zero_config();
    struct rte_register_image image;
    struct rte_applied applied;
    struct rte_error error;

    config.respiration.phase_rad = M_PI;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.micro_phase_resp == INT32_MIN);
    check_near(applied.respiration.phase_rad, -M_PI, 2e-9,
               "wrapped +pi phase");

    config.respiration.phase_rad = -M_PI;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.micro_phase_resp == INT32_MIN);

    config.respiration.phase_rad = 5.0 * M_PI / 2.0;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.micro_phase_resp == INT32_C(1073741824));

    /*
     * Normalize before quantizing. At the modulo boundary this is exactly
     * half a phase-word LSB below 2*pi, so MATLAB-style ties-away rounding
     * in the normalized interval produces -1 rather than zero.
     */
    config.respiration.phase_rad =
        2.0 * M_PI - M_PI / RTE_PHASE_MODULUS;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.micro_phase_resp == -INT32_C(1));
}

static void test_semantic_limits_and_numeric_overflow(void)
{
    const struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(1000000),
        .rx_lo_hz = UINT64_C(2400000000),
        .tx_lo_hz = UINT64_C(2400000000),
    };
    const double sample_rate = (double)rf.sample_rate_hz;
    const double wavelength =
        RTE_SPEED_OF_LIGHT_MPS / (double)rf.rx_lo_hz;
    const double max_range =
        RTE_DELAY_MAX_SAMPLES * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * sample_rate);
    const double max_amplitude = 256.0 * wavelength;
    struct rte_config config = zero_config();
    struct rte_register_image image;
    struct rte_applied applied;
    struct rte_error error;

    config.range_m = max_range;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.delay_offset == RTE_DELAY_MAX_SAMPLES);

    config.range_m =
        (RTE_DELAY_MAX_SAMPLES + 0.25) * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * sample_rate);
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_RANGE);
    CHECK(strcmp(error.field, "range_m") == 0);

    config = zero_config();
    config.respiration.amplitude_m = max_amplitude;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.micro_gain_resp == -INT32_C(16777216));

    config.respiration.amplitude_m = nextafter(max_amplitude, INFINITY);
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_QUANTIZATION);

    config.respiration.amplitude_m = DBL_MAX;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_QUANTIZATION);
    CHECK(strcmp(error.field, "respiration") == 0);
}

static void test_quantized_combined_aliasing(void)
{
    const struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(1000000),
        .rx_lo_hz = UINT64_C(2400000000),
        .tx_lo_hz = UINT64_C(2400000000),
    };
    const double sample_rate = (double)rf.sample_rate_hz;
    const double wavelength =
        RTE_SPEED_OF_LIGHT_MPS / (double)rf.rx_lo_hz;
    const double frequency =
        11735.0 * sample_rate / RTE_PHASE_MODULUS;
    const double amplitude = 0.5 * wavelength / 32768.0;
    const double applied_micro_max =
        4.0 * M_PI * amplitude * frequency / wavelength;
    const double phase_word_lsb = sample_rate / RTE_PHASE_MODULUS;
    const double requested_doppler =
        sample_rate / 2.0 - applied_micro_max - 0.1 * phase_word_lsb;
    struct rte_config config = zero_config();
    struct rte_register_image image;
    struct rte_applied applied;
    struct rte_error error;

    config.radial_velocity_mps =
        -0.5 * requested_doppler * wavelength;
    config.respiration.amplitude_m = amplitude;
    config.respiration.frequency_hz = frequency;

    CHECK(requested_doppler + applied_micro_max < sample_rate / 2.0);
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_ALIASING);
    CHECK(strcmp(error.field, "micro_motion") == 0);
}

static void test_boundaries(void)
{
    struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(1000000),
        .rx_lo_hz = UINT64_C(2400000000),
        .tx_lo_hz = UINT64_C(2400000000),
    };
    struct rte_config config = {
        .range_m = 0.0,
        .radial_velocity_mps = 0.0,
        .loss_db = 0.0,
        .respiration = {0.0, 0.0, 0.0},
        .heartbeat = {0.0, 0.0, 0.0},
    };
    struct rte_register_image image;
    struct rte_applied applied;
    struct rte_error error;
    double max_range =
        RTE_DELAY_MAX_SAMPLES * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)rf.sample_rate_hz);

    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.scaling == 32767);
    CHECK(image.doppler_pinc == 0);

    config.range_m = 0.5 * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)rf.sample_rate_hz);
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.delay_offset == 1);

    config.range_m = max_range;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) == 0);
    CHECK(image.delay_offset == RTE_DELAY_MAX_SAMPLES);

    config.range_m =
        (RTE_DELAY_MAX_SAMPLES + 0.51) * RTE_SPEED_OF_LIGHT_MPS /
        (2.0 * (double)rf.sample_rate_hz);
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_RANGE);

    config.range_m = 0.0;
    config.radial_velocity_mps =
        -0.5 * (0.5 * (double)rf.sample_rate_hz -
                0.25 * (double)rf.sample_rate_hz / RTE_PHASE_MODULUS) *
        RTE_SPEED_OF_LIGHT_MPS / (double)rf.rx_lo_hz;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_ALIASING);

    config.radial_velocity_mps = 0.0;
    config.respiration.frequency_hz =
        0.5 * (double)rf.sample_rate_hz -
        0.25 * (double)rf.sample_rate_hz / RTE_PHASE_MODULUS;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_ALIASING);

    config.respiration.frequency_hz = 0.0;
    config.loss_db = NAN;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_RANGE);

    config.loss_db = 0.0;
    rf.tx_lo_hz++;
    CHECK(rte_encode(&config, &rf, &image, &applied, &error) != 0);
    CHECK(error.code == RTE_ERROR_RF_CONTEXT);
}

int main(void)
{
    test_phase_words();
    test_reference_configuration();
    test_semantic_phase_wrapping();
    test_semantic_limits_and_numeric_overflow();
    test_quantized_combined_aliasing();
    test_boundaries();

    if (failures != 0) {
        fprintf(stderr, "%u test failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("test_model: PASS");
    return EXIT_SUCCESS;
}

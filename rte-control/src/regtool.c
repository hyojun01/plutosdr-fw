/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/mmio.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage:\n"
            "  %s [--device PATH] info\n"
            "  %s [--device PATH] readback\n"
            "  %s encode SAMPLE_RATE CARRIER RANGE VELOCITY LOSS "
            "AR FR PHIR AH FH PHIH\n",
            program, program, program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    const char *cursor;
    char *end;
    unsigned long long parsed;

    if (text == NULL || *text == '\0')
        return -EINVAL;
    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9')
            return -EINVAL;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -EINVAL;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_double(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
        return -EINVAL;
    *value = parsed;
    return 0;
}

static int parse_semantic_arguments(char **arguments,
                                    struct rte_rf_context *rf,
                                    struct rte_config *config)
{
    if (parse_u64(arguments[0], &rf->sample_rate_hz) != 0 ||
        parse_u64(arguments[1], &rf->rx_lo_hz) != 0)
        return -EINVAL;
    rf->tx_lo_hz = rf->rx_lo_hz;

    if (parse_double(arguments[2], &config->range_m) != 0 ||
        parse_double(arguments[3], &config->radial_velocity_mps) != 0 ||
        parse_double(arguments[4], &config->loss_db) != 0 ||
        parse_double(arguments[5], &config->respiration.amplitude_m) != 0 ||
        parse_double(arguments[6], &config->respiration.frequency_hz) != 0 ||
        parse_double(arguments[7], &config->respiration.phase_rad) != 0 ||
        parse_double(arguments[8], &config->heartbeat.amplitude_m) != 0 ||
        parse_double(arguments[9], &config->heartbeat.frequency_hz) != 0 ||
        parse_double(arguments[10], &config->heartbeat.phase_rad) != 0)
        return -EINVAL;
    return 0;
}

static void print_error(const struct rte_error *error)
{
    fprintf(stderr, "%s (%s): %s\n",
            rte_error_code_name(error->code), error->field,
            error->message);
}

static void print_image(const struct rte_register_image *image)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        const struct rte_register_descriptor *reg =
            &rte_parameter_registers[index];
        printf("0x%03" PRIx32 " %-20s 0x%08" PRIx32 "\n",
               reg->offset, reg->name,
               rte_register_image_word(image, reg->offset));
    }
}

static void print_applied(const struct rte_applied *applied)
{
    printf("applied.delay_samples=%u\n", applied->delay_samples);
    printf("applied.delay_range_m=%.17g\n", applied->delay_range_m);
    printf("applied.range_phase_rad=%.17g\n", applied->range_phase_rad);
    printf("applied.doppler_hz=%.17g\n", applied->doppler_hz);
    printf("applied.radial_velocity_mps=%.17g\n",
           applied->radial_velocity_mps);
    printf("applied.linear_gain=%.17g\n", applied->linear_gain);
    if (applied->muted)
        puts("applied.loss_db=inf");
    else
        printf("applied.loss_db=%.17g\n", applied->loss_db);
    printf("applied.respiration.frequency_hz=%.17g\n",
           applied->respiration.frequency_hz);
    printf("applied.respiration.amplitude_m=%.17g\n",
           applied->respiration.amplitude_m);
    printf("applied.respiration.phase_rad=%.17g\n",
           applied->respiration.phase_rad);
    printf("applied.heartbeat.frequency_hz=%.17g\n",
           applied->heartbeat.frequency_hz);
    printf("applied.heartbeat.amplitude_m=%.17g\n",
           applied->heartbeat.amplitude_m);
    printf("applied.heartbeat.phase_rad=%.17g\n",
           applied->heartbeat.phase_rad);
}

static int command_info(struct rte_mmio *mmio)
{
    uint32_t timestamp;
    uint32_t enable;
    uint32_t load;

    if (mmio->io.read32(mmio->io.context, RTE_REG_IPCORE_TIMESTAMP,
                        &timestamp) != 0 ||
        mmio->io.read32(mmio->io.context, RTE_REG_IPCORE_ENABLE,
                        &enable) != 0 ||
        mmio->io.read32(mmio->io.context, RTE_REG_LOAD_PARAM,
                        &load) != 0) {
        fputs("register read failed\n", stderr);
        return EXIT_FAILURE;
    }

    printf("device.physical_base=0x%08" PRIxPTR "\n",
           mmio->physical_base);
    printf("device.register_size=0x%zx\n", mmio->size);
    printf("hardware.timestamp=%" PRIu32 "\n", timestamp);
    printf("hardware.timestamp_expected=%" PRIu32 "\n",
           RTE_EXPECTED_TIMESTAMP);
    printf("hardware.compatible=%s\n",
           timestamp == RTE_EXPECTED_TIMESTAMP ? "true" : "false");
    printf("hardware.enabled=%s\n", (enable & 1U) ? "true" : "false");
    printf("hardware.load_param=%" PRIu32 "\n", load & 1U);
    return timestamp == RTE_EXPECTED_TIMESTAMP
        ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    const char *device_path = "/dev/mwipcore0";
    const char *command;
    struct rte_register_image image;
    struct rte_rf_context rf = {0};
    struct rte_config config = {0};
    struct rte_applied applied;
    struct rte_error error;
    struct rte_mmio mmio;
    int argument = 1;
    int status;

    if (argc > 2 && strcmp(argv[1], "--device") == 0) {
        device_path = argv[2];
        argument = 3;
    }
    if (argument >= argc) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    command = argv[argument++];

    if (strcmp(command, "encode") == 0) {
        if (argc - argument != 11 ||
            parse_semantic_arguments(&argv[argument], &rf, &config) != 0) {
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
        status = rte_encode(&config, &rf, &image, &applied, &error);
        if (status != 0) {
            print_error(&error);
            return EXIT_FAILURE;
        }
        print_image(&image);
        print_applied(&applied);
        return EXIT_SUCCESS;
    }

    status = rte_mmio_open(&mmio, device_path, &error);
    if (status != 0) {
        print_error(&error);
        return EXIT_FAILURE;
    }

    if (strcmp(command, "info") == 0) {
        status = command_info(&mmio);
        rte_mmio_close(&mmio);
        return status;
    }

    status = rte_register_validate_hardware(
        &mmio.io, RTE_EXPECTED_TIMESTAMP, &error);
    if (status != 0) {
        print_error(&error);
        rte_mmio_close(&mmio);
        return EXIT_FAILURE;
    }

    if (strcmp(command, "readback") == 0) {
        status = rte_register_read_image(&mmio.io, &image, &error);
        if (status == 0)
            print_image(&image);
    } else {
        usage(stderr, argv[0]);
        rte_mmio_close(&mmio);
        return EXIT_FAILURE;
    }

    if (status != 0)
        print_error(&error);
    rte_mmio_close(&mmio);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

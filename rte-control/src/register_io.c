/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/register_io.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RTE_READBACK_ATTEMPTS 1024U

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

static int validate_io(const struct rte_register_io *io,
                       struct rte_error *error)
{
    if (io == NULL || io->read32 == NULL || io->write32 == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "register_io",
                  "register read and write callbacks are required");
        return -EINVAL;
    }
    return 0;
}

static int read32(const struct rte_register_io *io, uint32_t offset,
                  uint32_t *value, struct rte_error *error)
{
    int status = io->read32(io->context, offset, value);

    if (status != 0) {
        set_error(error, RTE_ERROR_IO, "register_io",
                  "read at offset 0x%03x failed: %d", offset, status);
        return status;
    }
    return 0;
}

static int write32(const struct rte_register_io *io, uint32_t offset,
                   uint32_t value, struct rte_error *error)
{
    int status = io->write32(io->context, offset, value);

    if (status != 0) {
        set_error(error, RTE_ERROR_IO, "register_io",
                  "write 0x%08x at offset 0x%03x failed: %d",
                  value, offset, status);
        return status;
    }
    return 0;
}

static int wait_for_value(const struct rte_register_io *io,
                          uint32_t offset, uint32_t mask,
                          uint32_t expected, unsigned int matches_needed,
                          struct rte_error *error)
{
    unsigned int attempt;
    unsigned int matches = 0;
    uint32_t value = 0;
    int status;

    for (attempt = 0; attempt < RTE_READBACK_ATTEMPTS; ++attempt) {
        status = read32(io, offset, &value, error);
        if (status != 0)
            return status;
        if ((value & mask) == (expected & mask)) {
            matches++;
            if (matches >= matches_needed)
                return 0;
        } else {
            matches = 0;
        }
    }

    set_error(error, RTE_ERROR_HARDWARE, "register_readback",
              "offset 0x%03x read 0x%08x, expected 0x%08x "
              "(mask 0x%08x)", offset, value, expected, mask);
    return -EIO;
}

static int ensure_load_low(const struct rte_register_io *io,
                           bool *dirty,
                           struct rte_error *error)
{
    uint32_t value;
    int status = read32(io, RTE_REG_LOAD_PARAM, &value, error);

    if (status != 0)
        return status;
    if ((value & RTE_MASK_BIT) != 0) {
        if (dirty != NULL)
            *dirty = true;
        status = write32(io, RTE_REG_LOAD_PARAM, 0, error);
        if (status != 0)
            return status;
    }
    return wait_for_value(io, RTE_REG_LOAD_PARAM, RTE_MASK_BIT, 0, 1,
                          error);
}

static int stage_image(const struct rte_register_io *io,
                       const struct rte_register_image *image,
                       struct rte_error *error)
{
    size_t index;
    int status;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        const struct rte_register_descriptor *reg =
            &rte_parameter_registers[index];
        status = write32(io, reg->offset,
                         rte_register_image_word(image, reg->offset),
                         error);
        if (status != 0)
            return status;
    }

    for (index = 0; index < rte_parameter_register_count; ++index) {
        const struct rte_register_descriptor *reg =
            &rte_parameter_registers[index];
        status = wait_for_value(
            io, reg->offset, reg->mask,
            rte_register_image_word(image, reg->offset), 1, error);
        if (status != 0)
            return status;
    }
    return 0;
}

static int commit_staged(const struct rte_register_io *io,
                         bool *commit_uncertain,
                         struct rte_error *error)
{
    int status;

    /*
     * Once assertion is attempted, the active bank may have observed the
     * staged values even if a later AXI readback or deassertion fails.
     */
    *commit_uncertain = true;
    status = write32(io, RTE_REG_LOAD_PARAM, 1, error);
    if (status != 0)
        return status;

    /*
     * Two consecutive AXI readbacks keep loadParam high across multiple
     * transactions and therefore across IPCORE_CLK edges. The active
     * parameter bank follows the staged bank only while this level is high.
     */
    status = wait_for_value(io, RTE_REG_LOAD_PARAM, RTE_MASK_BIT, 1, 2,
                            error);
    if (status != 0)
        return status;

    status = write32(io, RTE_REG_LOAD_PARAM, 0, error);
    if (status != 0)
        return status;
    return wait_for_value(io, RTE_REG_LOAD_PARAM, RTE_MASK_BIT, 0, 1,
                          error);
}

static int apply_without_rollback(const struct rte_register_io *io,
                                  const struct rte_register_image *image,
                                  bool *dirty,
                                  bool *commit_uncertain,
                                  struct rte_error *error)
{
    uint32_t enable;
    int status;

    *dirty = false;
    *commit_uncertain = false;
    status = read32(io, RTE_REG_IPCORE_ENABLE, &enable, error);
    if (status != 0)
        return status;
    if ((enable & RTE_MASK_BIT) == 0) {
        set_error(error, RTE_ERROR_STATE, "IPCore_Enable",
                  "IP core is disabled; parameter latch and NCO state "
                  "are frozen");
        return -EBUSY;
    }
    status = ensure_load_low(io, dirty, error);
    if (status != 0)
        return status;
    *dirty = true;
    status = stage_image(io, image, error);
    if (status != 0)
        return status;
    return commit_staged(io, commit_uncertain, error);
}

int rte_register_apply(const struct rte_register_io *io,
                       const struct rte_register_image *image,
                       const struct rte_register_image *rollback_image,
                       struct rte_error *error)
{
    struct rte_error original_error;
    struct rte_error rollback_error;
    bool dirty;
    bool commit_uncertain;
    bool ignored;
    bool ignored_commit;
    int status;

    rte_error_clear(error);
    status = validate_io(io, error);
    if (status != 0)
        return status;
    if (image == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "register_image",
                  "register image is required");
        return -EINVAL;
    }

    status = apply_without_rollback(io, image, &dirty, &commit_uncertain,
                                    error);
    if (status == 0)
        return 0;

    original_error = *error;
    /*
     * A staging failure leaves a partial staged bank even though the active
     * bank is unchanged. A commit failure can additionally leave the active
     * bank uncertain. In either case, reapply the last known-good complete
     * image so the staged and active banks agree. Never use IPCore_Reset.
     */
    if (dirty && rollback_image != NULL) {
        rte_error_clear(&rollback_error);
        if (apply_without_rollback(io, rollback_image, &ignored,
                                   &ignored_commit, &rollback_error) != 0) {
            set_error(error, RTE_ERROR_HARDWARE, "rollback",
                      "%s; rollback also failed: %s",
                      original_error.message, rollback_error.message);
            return -EIO;
        }
    } else if (dirty) {
        struct rte_error clear_error;
        rte_error_clear(&clear_error);
        if (ensure_load_low(io, NULL, &clear_error) != 0) {
            set_error(error, RTE_ERROR_HARDWARE, "recovery",
                      "%s; loadParam recovery also failed: %s",
                      original_error.message, clear_error.message);
            return -EIO;
        }
        if (commit_uncertain) {
            set_error(error, RTE_ERROR_HARDWARE, "commit_state",
                      "%s; the first commit was attempted without a "
                      "known rollback image, so active hardware state is "
                      "unknown", original_error.message);
            return -EIO;
        }
    }

    *error = original_error;
    return status;
}

static int32_t sign_extend(uint32_t value, unsigned int width)
{
    uint64_t sign = UINT64_C(1) << (width - 1U);
    uint64_t modulus = UINT64_C(1) << width;
    uint64_t mask = modulus - UINT64_C(1);
    uint64_t raw = (uint64_t)value & mask;
    int64_t result = (raw & sign) != 0
        ? (int64_t)raw - (int64_t)modulus
        : (int64_t)raw;

    return (int32_t)result;
}

int rte_register_read_image(const struct rte_register_io *io,
                            struct rte_register_image *image,
                            struct rte_error *error)
{
    uint32_t value;
    int status;

    rte_error_clear(error);
    status = validate_io(io, error);
    if (status != 0)
        return status;
    if (image == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "register_image",
                  "register image output is required");
        return -EINVAL;
    }

#define READ_REGISTER(member, offset, conversion) do {                    \
    status = read32(io, (offset), &value, error);                          \
    if (status != 0)                                                       \
        return status;                                                     \
    image->member = (conversion);                                          \
} while (0)

    READ_REGISTER(delay_offset, RTE_REG_DELAY_OFFSET,
                  (uint16_t)(value & RTE_MASK_DELAY));
    READ_REGISTER(doppler_pinc, RTE_REG_DOPPLER_PINC,
                  sign_extend(value, 32));
    READ_REGISTER(doppler_phase, RTE_REG_DOPPLER_PHASE,
                  sign_extend(value, 32));
    READ_REGISTER(scaling, RTE_REG_SCALING,
                  (int16_t)sign_extend(value, 16));
    READ_REGISTER(micro_pinc_resp, RTE_REG_MICRO_PINC_RESP,
                  sign_extend(value, 32));
    READ_REGISTER(micro_phase_resp, RTE_REG_MICRO_PHASE_RESP,
                  sign_extend(value, 32));
    READ_REGISTER(micro_gain_resp, RTE_REG_MICRO_GAIN_RESP,
                  sign_extend(value, 25));
    READ_REGISTER(micro_pinc_heart, RTE_REG_MICRO_PINC_HEART,
                  sign_extend(value, 32));
    READ_REGISTER(micro_phase_heart, RTE_REG_MICRO_PHASE_HEART,
                  sign_extend(value, 32));
    READ_REGISTER(micro_gain_heart, RTE_REG_MICRO_GAIN_HEART,
                  sign_extend(value, 25));

#undef READ_REGISTER
    return 0;
}

int rte_register_validate_hardware(const struct rte_register_io *io,
                                   uint32_t expected_timestamp,
                                   struct rte_error *error)
{
    uint32_t timestamp;
    int status;

    rte_error_clear(error);
    status = validate_io(io, error);
    if (status != 0)
        return status;
    status = read32(io, RTE_REG_IPCORE_TIMESTAMP, &timestamp, error);
    if (status != 0)
        return status;
    if (timestamp != expected_timestamp) {
        set_error(error, RTE_ERROR_HARDWARE, "hardware_build_id",
                  "FPGA timestamp %u does not match expected %u",
                  timestamp, expected_timestamp);
        return -ENODEV;
    }
    return 0;
}

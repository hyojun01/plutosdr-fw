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
                           unsigned int target_index,
                           bool *dirty,
                           struct rte_error *error)
{
    uint32_t value;
    int status = read32(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, &value, error);

    if (status != 0)
        return status;
    if ((value & RTE_MASK_BIT) != 0) {
        if (dirty != NULL)
            *dirty = true;
        status = write32(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, 0, error);
        if (status != 0)
            return status;
    }
    return wait_for_value(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, RTE_MASK_BIT, 0, 1,
                          error);
}

static int stage_image(const struct rte_register_io *io,
                       unsigned int target_index,
                       const struct rte_register_image *image,
                       struct rte_error *error)
{
    unsigned int j;
    int status;
    for (j = 0; j < RTE_PARAMETER_REGISTER_COUNT; ++j) {
        const struct rte_register_descriptor *reg = &rte_target_registers[target_index][j];
        status = write32(io, reg->offset, image->words[j], error);
        if (status != 0) return status;
    }
    for (j = 0; j < RTE_PARAMETER_REGISTER_COUNT; ++j) {
        const struct rte_register_descriptor *reg = &rte_target_registers[target_index][j];
        status = wait_for_value(io, reg->offset, reg->mask, image->words[j], 1, error);
        if (status != 0) return status;
    }
    return 0;
}

static int commit_staged(const struct rte_register_io *io,
                         unsigned int target_index,
                         bool *commit_uncertain,
                         struct rte_error *error)
{
    int status;

    /*
     * Once assertion is attempted, the active bank may have observed the
     * staged values even if a later AXI readback or deassertion fails.
     */
    *commit_uncertain = true;
    status = write32(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, 1, error);
    if (status != 0)
        return status;

    /*
     * Two consecutive AXI readbacks keep loadParam high across multiple
     * transactions and therefore across IPCORE_CLK edges. The active
     * parameter bank follows the staged bank only while this level is high.
     */
    status = wait_for_value(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, RTE_MASK_BIT, 1, 2,
                            error);
    if (status != 0)
        return status;

    status = write32(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, 0, error);
    if (status != 0)
        return status;
    return wait_for_value(io, rte_target_registers[target_index][RTE_PARAM_LP].offset, RTE_MASK_BIT, 0, 1,
                          error);
}

static int apply_without_rollback(const struct rte_register_io *io,
                                  unsigned int target_index,
                                  const struct rte_register_image *image,
                                  bool *dirty,
                                  bool *commit_uncertain,
                                  struct rte_error *error)
{
    uint32_t enable, load;
    unsigned int other;
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
    /* A foreign high LP is never repaired by a selected-target request. */
    for (other = 0; other < RTE_TARGET_COUNT; ++other) {
        status = read32(io, rte_target_registers[other][RTE_PARAM_LP].offset, &load, error);
        if (status != 0) return status;
        if ((load & RTE_MASK_BIT) != 0) {
            if (other != target_index) {
                set_error(error, RTE_ERROR_STATE, "load_param", "target %u LP is high; refusing selected-target mutation", other + 1U);
                return -EBUSY;
            }
            *commit_uncertain = true;
        }
    }
    *dirty = *commit_uncertain;
    status = ensure_load_low(io, target_index, dirty, error);
    if (status != 0)
        return status;
    *dirty = true;
    status = stage_image(io, target_index, image, error);
    if (status != 0)
        return status;
    return commit_staged(io, target_index, commit_uncertain, error);
}

int rte_register_apply(const struct rte_register_io *io,
                       unsigned int target_index,
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

    struct rte_error local_error;
    if (error == NULL) error = &local_error;
    rte_error_clear(error);
    status = validate_io(io, error);
    if (status != 0)
        return status;
    if (image == NULL || target_index >= RTE_TARGET_COUNT) {
        set_error(error, RTE_ERROR_ARGUMENT, "register_image",
                  "image and target index 0..3 are required");
        return -EINVAL;
    }

    for (unsigned int j = 0; j < RTE_PARAMETER_REGISTER_COUNT; ++j) {
        uint32_t mask = rte_target_registers[target_index][j].mask;
        if ((image->words[j] & ~mask) != 0 ||
            (rollback_image != NULL && (rollback_image->words[j] & ~mask) != 0)) {
            set_error(error, RTE_ERROR_ARGUMENT, "register_image", "register word exceeds ABI mask");
            return -EINVAL;
        }
    }
    status = apply_without_rollback(io, target_index, image, &dirty, &commit_uncertain,
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
    if (dirty) {
        struct rte_error clear_error;
        rte_error_clear(&clear_error);
        /* Recover LP before any rollback staging, even if the core stopped. */
        if (ensure_load_low(io, target_index, NULL, &clear_error) != 0) {
            set_error(error, RTE_ERROR_HARDWARE, "recovery",
                      "%s; selected LP could not be cleared: %s",
                      original_error.message, clear_error.message);
            return -EIO;
        }
        if (rollback_image != NULL) {
            rte_error_clear(&rollback_error);
            if (apply_without_rollback(io, target_index, rollback_image, &ignored,
                                       &ignored_commit, &rollback_error) != 0) {
                /* A rollback commit failure may itself leave LP high. */
                (void)ensure_load_low(io, target_index, NULL, &clear_error);
                set_error(error, RTE_ERROR_HARDWARE, "rollback",
                          "%s; rollback also failed: %s",
                          original_error.message, rollback_error.message);
                return -EIO;
            }
        } else if (commit_uncertain) {
            set_error(error, RTE_ERROR_HARDWARE, "commit_state",
                      "%s; first commit attempted without a known rollback image; active state is unknown",
                      original_error.message);
            return -EIO;
        }
    }

    *error = original_error;
    return status;
}

int rte_register_read_image(const struct rte_register_io *io,
                            unsigned int target_index,
                            struct rte_register_image *image,
                            struct rte_error *error)
{
    struct rte_register_image result = {{0}};
    int status;
    rte_error_clear(error);
    status = validate_io(io, error);
    if (status != 0) return status;
    if (image == NULL || target_index >= RTE_TARGET_COUNT) {
        set_error(error, RTE_ERROR_ARGUMENT, "register_image", "output and target index 0..3 are required");
        return -EINVAL;
    }
    for (unsigned int j = 0; j < RTE_PARAMETER_REGISTER_COUNT; ++j) {
        const struct rte_register_descriptor *reg = &rte_target_registers[target_index][j];
        status = read32(io, reg->offset, &result.words[j], error);
        if (status != 0) return status;
        result.words[j] &= reg->mask;
    }
    *image = result;
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

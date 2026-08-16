/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/controller.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

void rte_config_to_patch(const struct rte_config *config,
                         struct rte_config_patch *patch)
{
    memset(patch, 0, sizeof(*patch));
    patch->has_range_m = true;
    patch->has_radial_velocity_mps = true;
    patch->has_loss_db = true;
    patch->range_m = config->range_m;
    patch->radial_velocity_mps = config->radial_velocity_mps;
    patch->loss_db = config->loss_db;
    patch->respiration.has_amplitude_m = true;
    patch->respiration.has_frequency_hz = true;
    patch->respiration.has_phase_rad = true;
    patch->respiration.amplitude_m = config->respiration.amplitude_m;
    patch->respiration.frequency_hz = config->respiration.frequency_hz;
    patch->respiration.phase_rad = config->respiration.phase_rad;
    patch->heartbeat.has_amplitude_m = true;
    patch->heartbeat.has_frequency_hz = true;
    patch->heartbeat.has_phase_rad = true;
    patch->heartbeat.amplitude_m = config->heartbeat.amplitude_m;
    patch->heartbeat.frequency_hz = config->heartbeat.frequency_hz;
    patch->heartbeat.phase_rad = config->heartbeat.phase_rad;
}

static bool config_patch_complete(const struct rte_config_patch *patch)
{
    return patch != NULL &&
        patch->has_range_m &&
        patch->has_radial_velocity_mps &&
        patch->has_loss_db &&
        patch->respiration.has_amplitude_m &&
        patch->respiration.has_frequency_hz &&
        patch->respiration.has_phase_rad &&
        patch->heartbeat.has_amplitude_m &&
        patch->heartbeat.has_frequency_hz &&
        patch->heartbeat.has_phase_rad;
}

static void merge_motion(struct rte_motion_config *config,
                         const struct rte_motion_patch *patch)
{
    if (patch->has_amplitude_m)
        config->amplitude_m = patch->amplitude_m;
    if (patch->has_frequency_hz)
        config->frequency_hz = patch->frequency_hz;
    if (patch->has_phase_rad)
        config->phase_rad = patch->phase_rad;
}

static void merge_config(struct rte_config *config,
                         const struct rte_config_patch *patch)
{
    if (patch->has_range_m)
        config->range_m = patch->range_m;
    if (patch->has_radial_velocity_mps)
        config->radial_velocity_mps = patch->radial_velocity_mps;
    if (patch->has_loss_db)
        config->loss_db = patch->loss_db;
    merge_motion(&config->respiration, &patch->respiration);
    merge_motion(&config->heartbeat, &patch->heartbeat);
}

static bool rf_equal(const struct rte_rf_context *left,
                     const struct rte_rf_context *right)
{
    return left->sample_rate_hz == right->sample_rate_hz &&
        left->rx_lo_hz == right->rx_lo_hz &&
        left->tx_lo_hz == right->tx_lo_hz;
}

static bool register_images_equal(
    const struct rte_register_image *left,
    const struct rte_register_image *right)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        const struct rte_register_descriptor *reg =
            &rte_parameter_registers[index];
        if ((rte_register_image_word(left, reg->offset) & reg->mask) !=
            (rte_register_image_word(right, reg->offset) & reg->mask))
            return false;
    }
    return true;
}

static bool error_makes_state_unknown(const struct rte_error *error)
{
    if (error == NULL || error->code != RTE_ERROR_HARDWARE)
        return false;
    return strcmp(error->field, "rollback") == 0 ||
        strcmp(error->field, "rf_rollback") == 0 ||
        strcmp(error->field, "recovery") == 0 ||
        strcmp(error->field, "commit_state") == 0 ||
        strcmp(error->field, "register_state") == 0;
}

static int lock_controller(struct rte_controller *controller,
                           struct rte_error *error)
{
    int status = pthread_mutex_lock(&controller->mutex);

    if (status != 0) {
        set_error(error, RTE_ERROR_STATE, "controller",
                  "cannot acquire controller lock: %d", status);
        return -status;
    }
    return 0;
}

int rte_controller_open(struct rte_controller *controller,
                        const char *device_path,
                        struct rte_error *error)
{
    uint32_t timestamp;
    int status;

    rte_error_clear(error);
    if (controller == NULL || device_path == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "controller",
                  "controller and device path are required");
        return -EINVAL;
    }
    memset(controller, 0, sizeof(*controller));
    controller->mmio.fd = -1;

    status = pthread_mutex_init(&controller->mutex, NULL);
    if (status != 0) {
        set_error(error, RTE_ERROR_STATE, "controller",
                  "cannot initialize controller lock: %d", status);
        return -status;
    }
    controller->mutex_initialized = true;

    status = rte_mmio_open(&controller->mmio, device_path, error);
    if (status != 0)
        goto fail;
    status = rte_register_validate_hardware(
        &controller->mmio.io, RTE_EXPECTED_TIMESTAMP, error);
    if (status != 0)
        goto fail;
    status = controller->mmio.io.read32(
        controller->mmio.io.context, RTE_REG_IPCORE_TIMESTAMP,
        &timestamp);
    if (status != 0) {
        set_error(error, RTE_ERROR_IO, "hardware_build_id",
                  "cannot read FPGA timestamp: %d", status);
        goto fail;
    }
    controller->state.hardware_build_id = timestamp;

    status = rte_ad936x_open(&controller->ad936x, error);
    if (status != 0)
        goto fail;
    status = rte_ad936x_read_rf(
        &controller->ad936x, &controller->state.rf, error);
    if (status != 0)
        goto fail;
    status = rte_register_read_image(
        &controller->mmio.io, &controller->state.image, error);
    if (status != 0)
        goto fail;

    /*
     * Register readback does not identify the semantic config uniquely,
     * especially for wrapped range phase. Start without a semantic state and
     * only claim one after an explicit or persisted successful transaction.
     */
    controller->state.has_config = false;
    controller->state.revision = 0;
    return 0;

fail:
    rte_controller_close(controller);
    return status;
}

void rte_controller_close(struct rte_controller *controller)
{
    if (controller == NULL)
        return;
    rte_ad936x_close(&controller->ad936x);
    rte_mmio_close(&controller->mmio);
    if (controller->mutex_initialized)
        (void)pthread_mutex_destroy(&controller->mutex);
    memset(controller, 0, sizeof(*controller));
    controller->mmio.fd = -1;
}

int rte_controller_snapshot(struct rte_controller *controller,
                            struct rte_snapshot *snapshot,
                            struct rte_error *error)
{
    int status;

    rte_error_clear(error);
    if (controller == NULL || snapshot == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "controller",
                  "controller and snapshot output are required");
        return -EINVAL;
    }
    status = lock_controller(controller, error);
    if (status != 0)
        return status;
    *snapshot = controller->state;
    (void)pthread_mutex_unlock(&controller->mutex);
    return 0;
}

int rte_controller_apply(struct rte_controller *controller,
                         const struct rte_rf_patch *rf_patch,
                         const struct rte_config_patch *config_patch,
                         bool has_expected_revision,
                         uint64_t expected_revision,
                         struct rte_snapshot *snapshot,
                         struct rte_error *error)
{
    struct rte_rf_context old_rf;
    struct rte_rf_context desired_rf;
    struct rte_rf_context actual_rf;
    struct rte_rf_context restored_rf;
    struct rte_register_image current_image;
    struct rte_register_image next_image;
    struct rte_applied next_applied;
    struct rte_config next_config;
    struct rte_error restore_error;
    bool next_has_config;
    bool rf_was_changed = false;
    int status;

    rte_error_clear(error);
    if (controller == NULL || snapshot == NULL ||
        (rf_patch == NULL && config_patch == NULL)) {
        set_error(error, RTE_ERROR_ARGUMENT, "controller",
                  "controller, output, and at least one patch are required");
        return -EINVAL;
    }
    status = lock_controller(controller, error);
    if (status != 0)
        return status;

    if (has_expected_revision &&
        expected_revision != controller->state.revision) {
        set_error(error, RTE_ERROR_CONFLICT, "revision",
                  "expected revision %llu, current revision is %llu",
                  (unsigned long long)expected_revision,
                  (unsigned long long)controller->state.revision);
        status = -EAGAIN;
        goto out;
    }
    if (controller->state.degraded) {
        set_error(error, RTE_ERROR_STATE, "controller",
                  "hardware state is unknown after a failed rollback; "
                  "restart the daemon before applying another patch");
        status = -EIO;
        goto out;
    }

    old_rf = controller->state.rf;
    desired_rf = old_rf;
    if (rf_patch != NULL) {
        if (rf_patch->has_sample_rate_hz)
            desired_rf.sample_rate_hz = rf_patch->sample_rate_hz;
        if (rf_patch->has_carrier_hz) {
            desired_rf.rx_lo_hz = rf_patch->carrier_hz;
            desired_rf.tx_lo_hz = rf_patch->carrier_hz;
        }
    }

    next_has_config = controller->state.has_config;
    next_config = controller->state.requested;
    if (config_patch != NULL) {
        if (!next_has_config && !config_patch_complete(config_patch)) {
            set_error(error, RTE_ERROR_STATE, "rte",
                      "the first RTE configuration must contain every "
                      "semantic field");
            status = -EINVAL;
            goto out;
        }
        merge_config(&next_config, config_patch);
        next_has_config = true;
    }
    if (!next_has_config && !rf_equal(&desired_rf, &old_rf)) {
        set_error(error, RTE_ERROR_STATE, "rf",
                  "the first RF change must be combined with a complete "
                  "RTE configuration");
        status = -EINVAL;
        goto out;
    }

    if (controller->state.has_config) {
        status = rte_register_read_image(
            &controller->mmio.io, &current_image, error);
        if (status != 0)
            goto out;
        if (!register_images_equal(
                &current_image, &controller->state.image)) {
            set_error(error, RTE_ERROR_HARDWARE, "register_state",
                      "staged register bank differs from the last "
                      "successful transaction");
            status = -EIO;
            goto out;
        }
    }

    /*
     * Preflight against the desired integer RF values before changing the
     * transceiver. The exact applied values are encoded again after IIO
     * readback.
     */
    if (next_has_config) {
        status = rte_encode(&next_config, &desired_rf, &next_image,
                            &next_applied, error);
        if (status != 0)
            goto out;
    }

    if (!rf_equal(&desired_rf, &old_rf)) {
        status = rte_ad936x_apply_rf(
            &controller->ad936x, &desired_rf, &actual_rf, error);
        if (status != 0)
            goto out;
        rf_was_changed = true;
    } else {
        status = rte_ad936x_read_rf(
            &controller->ad936x, &actual_rf, error);
        if (status != 0)
            goto out;
    }

    if (next_has_config) {
        status = rte_encode(&next_config, &actual_rf, &next_image,
                            &next_applied, error);
        if (status != 0)
            goto restore_rf;
        status = rte_register_apply(
            &controller->mmio.io, &next_image,
            controller->state.has_config
                ? &controller->state.image : NULL,
            error);
        if (status != 0)
            goto restore_rf;
    }

    controller->state.rf = actual_rf;
    controller->state.has_config = next_has_config;
    if (next_has_config) {
        controller->state.requested = next_config;
        controller->state.image = next_image;
        controller->state.applied = next_applied;
    }
    controller->state.revision++;
    *snapshot = controller->state;
    status = 0;
    goto out;

restore_rf:
    if (rf_was_changed) {
        rte_error_clear(&restore_error);
        if (rte_ad936x_apply_rf(
                &controller->ad936x, &old_rf, &restored_rf,
                &restore_error) != 0) {
            set_error(error, RTE_ERROR_HARDWARE, "rf_rollback",
                      "transaction failed and RF rollback also failed: %s",
                      restore_error.message);
            status = -EIO;
        } else if (!rf_equal(&restored_rf, &old_rf)) {
            set_error(error, RTE_ERROR_HARDWARE, "rf_rollback",
                      "transaction failed and AD936x rollback returned "
                      "different RF values");
            status = -EIO;
        }
    }

out:
    if (status != 0 && error_makes_state_unknown(error))
        controller->state.degraded = true;
    (void)pthread_mutex_unlock(&controller->mutex);
    return status;
}

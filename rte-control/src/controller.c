/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <rte/controller.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void set_error(struct rte_error *error, enum rte_error_code code,
                      const char *field, const char *format, ...)
{
    va_list args;
    if (!error) return;
    error->code = code;
    snprintf(error->field, sizeof(error->field), "%s", field);
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int lock_controller(struct rte_controller *controller, struct rte_error *error)
{
    int status;
    if (!controller || !controller->mutex_initialized) {
        set_error(error, RTE_ERROR_ARGUMENT, "controller", "initialized controller is required");
        return -EINVAL;
    }
    status = pthread_mutex_lock(&controller->mutex);
    if (status) set_error(error, RTE_ERROR_STATE, "controller", "cannot acquire controller lock: %d", status);
    return -status;
}

static bool rf_equal(const struct rte_rf_context *a, const struct rte_rf_context *b)
{
    return a->sample_rate_hz == b->sample_rate_hz && a->manual_gain == b->manual_gain &&
        a->rx_lo_hz == b->rx_lo_hz && a->tx_lo_hz == b->tx_lo_hz &&
        a->rx_bandwidth_hz == b->rx_bandwidth_hz && a->tx_bandwidth_hz == b->tx_bandwidth_hz &&
        a->rx_gain_mdb == b->rx_gain_mdb && a->tx_gain_mdb == b->tx_gain_mdb &&
        !strcmp(a->compatible, b->compatible);
}

static void update_rf(struct rte_controller *controller, const struct rte_rf_context *rf)
{
    controller->state.rf = *rf;
    for (unsigned int i = 0; i < RTE_TARGET_COUNT; i++) {
        struct rte_target_state *target = &controller->state.targets[i];
        target->needs_reapply = target->has_config && target->encoded_carrier_hz != rf->rx_lo_hz;
    }
    controller->state.last_hardware_check_ms = monotonic_ms();
}

static bool error_makes_state_unknown(const struct rte_error *error)
{
    return error && error->code == RTE_ERROR_HARDWARE &&
        (!strcmp(error->field, "rollback") || !strcmp(error->field, "rf_rollback") ||
         !strcmp(error->field, "recovery") || !strcmp(error->field, "commit_state") ||
         !strcmp(error->field, "register_state"));
}

static int preflight(struct rte_controller *controller, bool has_revision, uint64_t revision,
                     struct rte_error *error)
{
    struct rte_rf_context actual;
    int status;
    if (has_revision && controller->state.revision != revision) {
        set_error(error, RTE_ERROR_CONFLICT, "revision", "expected revision %llu, current revision is %llu",
                  (unsigned long long)revision, (unsigned long long)controller->state.revision);
        return -EAGAIN;
    }
    if (controller->state.degraded) {
        set_error(error, RTE_ERROR_STATE, "controller", "hardware state is unknown; inspect hardware and restart daemon");
        return -EIO;
    }
    status = rte_register_validate_hardware(&controller->mmio.io, RTE_EXPECTED_TIMESTAMP, error);
    if (status) return status;
    status = rte_ad936x_read_rf(&controller->ad936x, &actual, error);
    if (status) return status;
    if (!rf_equal(&actual, &controller->state.rf)) {
        update_rf(controller, &actual);
        controller->state.revision++;
        if (has_revision) {
            set_error(error, RTE_ERROR_CONFLICT, "revision", "RF changed outside this daemon; refresh state before applying");
            return -EAGAIN;
        }
    } else {
        controller->state.last_hardware_check_ms = monotonic_ms();
    }
    return 0;
}

int rte_controller_open(struct rte_controller *controller, const char *device_path, struct rte_error *error)
{
    int status;
    rte_error_clear(error);
    if (!controller || !device_path) return -EINVAL;
    memset(controller, 0, sizeof(*controller));
    controller->mmio.fd = -1;
    status = pthread_mutex_init(&controller->mutex, NULL);
    if (status) return -status;
    controller->mutex_initialized = true;
    status = rte_mmio_open(&controller->mmio, device_path, error);
    if (status) goto fail;
    status = rte_register_validate_hardware(&controller->mmio.io, RTE_EXPECTED_TIMESTAMP, error);
    if (status) goto fail;
    controller->state.hardware_build_id = RTE_EXPECTED_TIMESTAMP;
    status = rte_ad936x_open(&controller->ad936x, error);
    if (status) goto fail;
    status = rte_ad936x_read_rf(&controller->ad936x, &controller->state.rf, error);
    if (status) goto fail;
    for (unsigned int i = 0; i < RTE_TARGET_COUNT; i++) {
        status = rte_register_read_image(&controller->mmio.io, i, &controller->state.targets[i].image, error);
        if (status) goto fail;
    }
    /* Staged readback cannot establish active contents. Only explicit per-target
     * loadParam can establish a semantic configuration in this process. */
    controller->state.last_hardware_check_ms = monotonic_ms();
    return 0;
fail:
    rte_controller_close(controller);
    return status;
}

void rte_controller_close(struct rte_controller *controller)
{
    if (!controller) return;
    rte_ad936x_close(&controller->ad936x);
    rte_mmio_close(&controller->mmio);
    if (controller->mutex_initialized) (void)pthread_mutex_destroy(&controller->mutex);
    memset(controller, 0, sizeof(*controller));
    controller->mmio.fd = -1;
}

int rte_controller_snapshot(struct rte_controller *controller, struct rte_snapshot *snapshot, struct rte_error *error)
{
    int status;
    rte_error_clear(error);
    if (!snapshot) return -EINVAL;
    status = lock_controller(controller, error);
    if (status) return status;
    *snapshot = controller->state;
    (void)pthread_mutex_unlock(&controller->mutex);
    return 0;
}

int rte_controller_rf_capabilities(struct rte_controller *controller, struct rte_rf_capabilities *caps, struct rte_error *error)
{
    struct rte_rf_context actual;
    int status;
    rte_error_clear(error);
    if (!caps) return -EINVAL;
    status = lock_controller(controller, error);
    if (status) return status;
    /* Unlike the cached status endpoint, capabilities inspect the live gain
     * table. Publish its RF context as an observation so limits and ETag agree. */
    status = rte_ad936x_read_rf(&controller->ad936x, &actual, error);
    if (!status) {
        if (!rf_equal(&actual, &controller->state.rf)) {
            update_rf(controller, &actual);
            controller->state.revision++;
        } else {
            controller->state.last_hardware_check_ms = monotonic_ms();
        }
        status = rte_ad936x_capabilities(&controller->ad936x, caps, error);
    }
    (void)pthread_mutex_unlock(&controller->mutex);
    return status;
}

static int gain_to_mdb(double db, int32_t *mdb, const char *field, struct rte_error *error)
{
    double scaled = db * 1000.0;
    if (!isfinite(scaled) || scaled < INT32_MIN || scaled > INT32_MAX ||
        fabs(scaled - round(scaled)) > 0.00001) {
        set_error(error, RTE_ERROR_RANGE, field, "gain must be a finite supported dB value");
        return -ERANGE;
    }
    *mdb = (int32_t)llround(scaled);
    return 0;
}

int rte_controller_apply_rf(struct rte_controller *controller, const struct rte_rf_patch *patch,
                            bool has_revision, uint64_t revision, struct rte_snapshot *snapshot,
                            struct rte_error *error)
{
    struct rte_error local_error;
    struct rte_rf_context desired, actual;
    int status;
    if (!error) error = &local_error;
    rte_error_clear(error);
    if (!patch || !snapshot || !(patch->has_carrier_hz || patch->has_bandwidth_hz || patch->has_tx_gain_db || patch->has_rx_gain_db)) {
        set_error(error, RTE_ERROR_ARGUMENT, "rf", "at least one RF field is required");
        return -EINVAL;
    }
    status = lock_controller(controller, error);
    if (status) return status;
    status = preflight(controller, has_revision, revision, error);
    if (status) goto out;
    desired = controller->state.rf;
    if (patch->has_carrier_hz) desired.rx_lo_hz = desired.tx_lo_hz = patch->carrier_hz;
    if (patch->has_bandwidth_hz) desired.rx_bandwidth_hz = desired.tx_bandwidth_hz = patch->bandwidth_hz;
    if (patch->has_tx_gain_db) {
        status = gain_to_mdb(patch->tx_gain_db, &desired.tx_gain_mdb, "tx_gain_db", error);
        if (status) goto out;
    }
    if (patch->has_rx_gain_db) {
        status = gain_to_mdb(patch->rx_gain_db, &desired.rx_gain_mdb, "rx_gain_db", error);
        if (status) goto out;
    }
    actual = controller->state.rf;
    status = rte_ad936x_apply_rf(&controller->ad936x, &desired, &actual, error);
    if (status) {
        /* A successful rollback may quantize the old LO by a few Hz. Publish
         * that real observation and invalidate old Doppler encodings/ETags. */
        if (!error_makes_state_unknown(error) && !rf_equal(&actual, &controller->state.rf)) {
            update_rf(controller, &actual);
            controller->state.revision++;
        }
        goto out;
    }
    update_rf(controller, &actual);
    controller->state.revision++;
out:
    if (status && error_makes_state_unknown(error)) controller->state.degraded = true;
    *snapshot = controller->state;
    (void)pthread_mutex_unlock(&controller->mutex);
    return status;
}

int rte_controller_apply_target(struct rte_controller *controller, unsigned int target_index,
                                const struct rte_config *config, bool has_revision, uint64_t revision,
                                struct rte_snapshot *snapshot, struct rte_error *error)
{
    struct rte_error local_error;
    struct rte_register_image current, next;
    struct rte_applied applied;
    struct rte_target_state *target;
    int status;
    if (!error) error = &local_error;
    rte_error_clear(error);
    if (!config || !snapshot || target_index >= RTE_TARGET_COUNT) {
        set_error(error, RTE_ERROR_ARGUMENT, "target", "one target index in 0..3 and its complete configuration are required");
        return -EINVAL;
    }
    status = lock_controller(controller, error);
    if (status) return status;
    target = &controller->state.targets[target_index];
    status = preflight(controller, has_revision, revision, error);
    if (status) goto out;
    if (target->has_config) {
        uint32_t lp;
        status = rte_register_read_image(&controller->mmio.io, target_index, &current, error);
        if (status) goto out;
        for (unsigned int i = 0; i < RTE_PARAMETER_REGISTER_COUNT; i++) {
            uint32_t mask = rte_target_registers[target_index][i].mask;
            if ((current.words[i] & mask) != (target->image.words[i] & mask)) {
                set_error(error, RTE_ERROR_HARDWARE, "register_state", "selected target staged bank differs from its last successful loadParam");
                status = -EIO;
                goto out;
            }
        }
        status = controller->mmio.io.read32(controller->mmio.io.context,
                    rte_target_registers[target_index][RTE_PARAM_LP].offset, &lp);
        if (status || (lp & 1)) {
            set_error(error, RTE_ERROR_HARDWARE, "register_state", "selected target loadParam state cannot be confirmed low");
            status = -EIO;
            goto out;
        }
    }
    status = rte_encode(config, &controller->state.rf, &next, &applied, error);
    if (status) goto out;
    status = rte_register_apply(&controller->mmio.io, target_index, &next,
                                target->has_config ? &target->image : NULL, error);
    if (status) goto out;
    target->has_config = true;
    target->hardware_state_known = true;
    target->needs_reapply = false;
    target->encoded_carrier_hz = controller->state.rf.rx_lo_hz;
    target->requested = *config;
    target->image = next;
    target->applied = applied;
    controller->state.last_hardware_check_ms = monotonic_ms();
    controller->state.revision++;
out:
    if (status && error_makes_state_unknown(error)) {
        controller->state.degraded = true;
        target->hardware_state_known = false;
    }
    *snapshot = controller->state;
    (void)pthread_mutex_unlock(&controller->mutex);
    return status;
}

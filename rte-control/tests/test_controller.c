/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/controller.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mock_registers {
    uint32_t staged[0x130 / 4];
    uint32_t active[0x130 / 4];
    unsigned int writes;
    unsigned int commits;
    unsigned int fail_high_writes;
};

struct mock_rf_device {
    struct rte_rf_context current;
    uint64_t next_applied_sample_rate_hz;
    unsigned int read_calls;
    unsigned int apply_calls;
};

static struct mock_rf_device mock_rf;
static unsigned int failures;

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                    \
                __FILE__, __LINE__, #condition);                          \
        failures++;                                                       \
    }                                                                     \
} while (0)

static bool parameter_offset(uint32_t offset)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        if (rte_parameter_registers[index].offset == offset)
            return true;
    }
    return false;
}

static int mock_register_read(void *context, uint32_t offset,
                              uint32_t *value)
{
    struct mock_registers *mock = context;

    if (value == NULL || (offset & 3U) != 0 ||
        offset >= sizeof(mock->staged))
        return -EINVAL;
    *value = mock->staged[offset / 4];
    return 0;
}

static int mock_register_write(void *context, uint32_t offset,
                               uint32_t value)
{
    struct mock_registers *mock = context;
    size_t index;

    if ((offset & 3U) != 0 || offset >= sizeof(mock->staged))
        return -EINVAL;
    mock->writes++;
    if (offset == RTE_REG_LOAD_PARAM && (value & 1U) != 0 &&
        mock->fail_high_writes != 0) {
        mock->fail_high_writes--;
        return -EIO;
    }

    if (parameter_offset(offset) &&
        (mock->staged[RTE_REG_LOAD_PARAM / 4] & 1U) != 0)
        mock->active[offset / 4] = value;
    mock->staged[offset / 4] = value;
    if (offset == RTE_REG_LOAD_PARAM && (value & 1U) != 0) {
        for (index = 0; index < rte_parameter_register_count; ++index) {
            uint32_t register_offset =
                rte_parameter_registers[index].offset;
            mock->active[register_offset / 4] =
                mock->staged[register_offset / 4];
        }
        mock->commits++;
    }
    return 0;
}

static void set_bank_image(uint32_t *bank,
                           const struct rte_register_image *image)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        bank[offset / 4] = rte_register_image_word(image, offset);
    }
}

static bool bank_matches(const uint32_t *bank,
                         const struct rte_register_image *image)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        uint32_t mask = rte_parameter_registers[index].mask;

        if ((bank[offset / 4] & mask) !=
            (rte_register_image_word(image, offset) & mask))
            return false;
    }
    return true;
}

static bool images_equal(const struct rte_register_image *left,
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

static struct rte_config reference_config(void)
{
    const struct rte_config config = {
        .range_m = 2.0,
        .radial_velocity_mps = -0.15,
        .loss_db = 12.0,
        .respiration = {0.005, 0.25, 0.5},
        .heartbeat = {0.0005, 1.2, 1.5},
    };

    return config;
}

static struct rte_rf_context reference_rf(void)
{
    const struct rte_rf_context rf = {
        .sample_rate_hz = UINT64_C(61440000),
        .rx_lo_hz = UINT64_C(2450000000),
        .tx_lo_hz = UINT64_C(2450000000),
    };

    return rf;
}

static void init_controller(struct rte_controller *controller,
                            struct mock_registers *registers,
                            bool with_config)
{
    struct rte_error error;

    memset(controller, 0, sizeof(*controller));
    memset(registers, 0, sizeof(*registers));
    memset(&mock_rf, 0, sizeof(mock_rf));
    CHECK(pthread_mutex_init(&controller->mutex, NULL) == 0);
    controller->mutex_initialized = true;
    controller->mmio.fd = -1;
    controller->mmio.io.context = registers;
    controller->mmio.io.read32 = mock_register_read;
    controller->mmio.io.write32 = mock_register_write;
    registers->staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    controller->state.hardware_build_id = RTE_EXPECTED_TIMESTAMP;
    controller->state.rf = reference_rf();
    controller->state.revision = 7;
    mock_rf.current = controller->state.rf;

    if (with_config) {
        controller->state.has_config = true;
        controller->state.requested = reference_config();
        CHECK(rte_encode(&controller->state.requested,
                         &controller->state.rf,
                         &controller->state.image,
                         &controller->state.applied, &error) == 0);
        set_bank_image(registers->staged, &controller->state.image);
        set_bank_image(registers->active, &controller->state.image);
    }
}

static void destroy_controller(struct rte_controller *controller)
{
    if (controller->mutex_initialized)
        CHECK(pthread_mutex_destroy(&controller->mutex) == 0);
}

/* Controller link seams: these tests never exercise open/close discovery. */
int rte_mmio_open(struct rte_mmio *mmio, const char *device_path,
                  struct rte_error *error)
{
    (void)mmio;
    (void)device_path;
    (void)error;
    return -ENOSYS;
}

void rte_mmio_close(struct rte_mmio *mmio)
{
    (void)mmio;
}

int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error)
{
    (void)device;
    (void)error;
    return -ENOSYS;
}

void rte_ad936x_close(struct rte_ad936x *device)
{
    (void)device;
}

int rte_ad936x_read_rf(struct rte_ad936x *device,
                       struct rte_rf_context *rf,
                       struct rte_error *error)
{
    (void)device;
    (void)error;
    mock_rf.read_calls++;
    *rf = mock_rf.current;
    return 0;
}

int rte_ad936x_apply_rf(struct rte_ad936x *device,
                        const struct rte_rf_context *requested,
                        struct rte_rf_context *applied,
                        struct rte_error *error)
{
    (void)device;
    (void)error;
    mock_rf.apply_calls++;
    mock_rf.current = *requested;
    if (mock_rf.next_applied_sample_rate_hz != 0) {
        mock_rf.current.sample_rate_hz =
            mock_rf.next_applied_sample_rate_hz;
        mock_rf.next_applied_sample_rate_hz = 0;
    }
    *applied = mock_rf.current;
    return 0;
}

static void test_first_rf_change_requires_full_rte(void)
{
    struct rte_controller controller;
    struct mock_registers registers;
    struct rte_rf_patch patch = {
        .has_sample_rate_hz = true,
        .sample_rate_hz = UINT64_C(60000000),
    };
    struct rte_snapshot snapshot;
    struct rte_error error;

    init_controller(&controller, &registers, false);
    CHECK(rte_controller_apply(&controller, &patch, NULL, false, 0,
                               &snapshot, &error) != 0);
    CHECK(error.code == RTE_ERROR_STATE);
    CHECK(strcmp(error.field, "rf") == 0);
    CHECK(mock_rf.read_calls == 0);
    CHECK(mock_rf.apply_calls == 0);
    CHECK(registers.writes == 0);
    CHECK(controller.state.revision == 7);
    CHECK(!controller.state.degraded);
    destroy_controller(&controller);
}

static void test_actual_rf_readback_drives_encoding(void)
{
    struct rte_controller controller;
    struct mock_registers registers;
    struct rte_rf_patch patch = {
        .has_sample_rate_hz = true,
        .sample_rate_hz = UINT64_C(60000000),
    };
    struct rte_rf_context actual_rf;
    struct rte_register_image expected_image;
    struct rte_applied expected_applied;
    struct rte_snapshot snapshot;
    struct rte_error error;

    init_controller(&controller, &registers, true);
    mock_rf.next_applied_sample_rate_hz = UINT64_C(59000000);
    actual_rf = controller.state.rf;
    actual_rf.sample_rate_hz = UINT64_C(59000000);
    CHECK(rte_encode(&controller.state.requested, &actual_rf,
                     &expected_image, &expected_applied, &error) == 0);

    CHECK(rte_controller_apply(&controller, &patch, NULL, false, 0,
                               &snapshot, &error) == 0);
    CHECK(mock_rf.apply_calls == 1);
    CHECK(snapshot.rf.sample_rate_hz == UINT64_C(59000000));
    CHECK(images_equal(&snapshot.image, &expected_image));
    CHECK(bank_matches(registers.active, &expected_image));
    CHECK(snapshot.revision == 8);
    CHECK(!snapshot.degraded);
    destroy_controller(&controller);
}

static void test_register_failure_rolls_back_known_image(void)
{
    struct rte_controller controller;
    struct mock_registers registers;
    struct rte_config_patch patch = {
        .has_loss_db = true,
        .loss_db = 6.0,
    };
    struct rte_register_image previous;
    struct rte_snapshot snapshot;
    struct rte_error error;

    init_controller(&controller, &registers, true);
    previous = controller.state.image;
    registers.fail_high_writes = 1;
    CHECK(rte_controller_apply(&controller, NULL, &patch, false, 0,
                               &snapshot, &error) != 0);
    CHECK(error.code == RTE_ERROR_IO);
    CHECK(registers.fail_high_writes == 0);
    CHECK(registers.commits == 1);
    CHECK(bank_matches(registers.staged, &previous));
    CHECK(bank_matches(registers.active, &previous));
    CHECK(images_equal(&controller.state.image, &previous));
    CHECK(controller.state.revision == 7);
    CHECK(!controller.state.degraded);
    destroy_controller(&controller);
}

static void test_first_commit_failure_quarantines_controller(void)
{
    struct rte_controller controller;
    struct mock_registers registers;
    struct rte_config config = reference_config();
    struct rte_config_patch patch;
    struct rte_snapshot snapshot;
    struct rte_error error;

    init_controller(&controller, &registers, false);
    rte_config_to_patch(&config, &patch);
    registers.fail_high_writes = 1;
    CHECK(rte_controller_apply(&controller, NULL, &patch, false, 0,
                               &snapshot, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(strcmp(error.field, "commit_state") == 0);
    CHECK(controller.state.degraded);
    CHECK(!controller.state.has_config);
    CHECK(controller.state.revision == 7);

    CHECK(rte_controller_apply(&controller, NULL, &patch, false, 0,
                               &snapshot, &error) != 0);
    CHECK(error.code == RTE_ERROR_STATE);
    CHECK(strcmp(error.field, "controller") == 0);
    destroy_controller(&controller);
}

static void test_staged_state_mismatch_is_quarantined(void)
{
    struct rte_controller controller;
    struct mock_registers registers;
    struct rte_config_patch patch = {
        .has_loss_db = true,
        .loss_db = 6.0,
    };
    struct rte_snapshot snapshot;
    struct rte_error error;

    init_controller(&controller, &registers, true);
    registers.staged[RTE_REG_DOPPLER_PINC / 4] ^= UINT32_C(1);
    CHECK(rte_controller_apply(&controller, NULL, &patch, false, 0,
                               &snapshot, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(strcmp(error.field, "register_state") == 0);
    CHECK(registers.writes == 0);
    CHECK(mock_rf.read_calls == 0);
    CHECK(mock_rf.apply_calls == 0);
    CHECK(controller.state.degraded);
    destroy_controller(&controller);
}

int main(void)
{
    test_first_rf_change_requires_full_rte();
    test_actual_rf_readback_drives_encoding();
    test_register_failure_rolls_back_known_image();
    test_first_commit_failure_quarantines_controller();
    test_staged_state_mismatch_is_quarantined();

    if (failures != 0) {
        fprintf(stderr, "%u test failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("test_controller: PASS");
    return EXIT_SUCCESS;
}

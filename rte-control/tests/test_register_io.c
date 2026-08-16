/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/register_io.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mock_registers {
    uint32_t staged[0x130 / 4];
    uint32_t active[0x130 / 4];
    uint32_t stale_value[0x130 / 4];
    unsigned int stale_reads[0x130 / 4];
    unsigned int read_count[0x130 / 4];
    bool permanent_mismatch[0x130 / 4];
    unsigned int load_high_reads;
    unsigned int load_high_writes;
    unsigned int load_low_writes;
    unsigned int commits;
    unsigned int reset_writes;
    unsigned int write_attempts;
    unsigned int parameter_writes_while_load_high;
    int fail_write_offset;
    unsigned int fail_write_count;
    bool fail_write_matches_value;
    uint32_t fail_write_value;
    unsigned int stale_high_reads_after_low_write;
};

static unsigned int failures;

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n",                    \
                __FILE__, __LINE__, #condition);                          \
        failures++;                                                       \
    }                                                                     \
} while (0)

static int mock_read(void *context, uint32_t offset, uint32_t *value)
{
    struct mock_registers *mock = context;
    size_t index;

    if ((offset & 3U) != 0 || offset >= sizeof(mock->staged))
        return -EINVAL;
    index = offset / 4;
    mock->read_count[index]++;
    if (mock->stale_reads[index] != 0) {
        mock->stale_reads[index]--;
        *value = mock->stale_value[index];
    } else {
        *value = mock->staged[index];
    }
    if (mock->permanent_mismatch[index])
        *value ^= UINT32_C(1);
    if (offset == RTE_REG_LOAD_PARAM && (*value & 1U) != 0)
        mock->load_high_reads++;
    return 0;
}

static bool is_parameter_register(uint32_t offset)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        if (rte_parameter_registers[index].offset == offset)
            return true;
    }
    return false;
}

static int mock_write(void *context, uint32_t offset, uint32_t value)
{
    struct mock_registers *mock = context;
    size_t index;

    if ((offset & 3U) != 0 || offset >= sizeof(mock->staged))
        return -EINVAL;
    mock->write_attempts++;
    if (offset == RTE_REG_IPCORE_RESET)
        mock->reset_writes++;
    if ((int)offset == mock->fail_write_offset &&
        mock->fail_write_count != 0 &&
        (!mock->fail_write_matches_value ||
         value == mock->fail_write_value)) {
        mock->fail_write_count--;
        return -EIO;
    }

    if (is_parameter_register(offset) &&
        (mock->staged[RTE_REG_LOAD_PARAM / 4] & 1U) != 0) {
        mock->parameter_writes_while_load_high++;
        mock->active[offset / 4] = value;
    }
    mock->staged[offset / 4] = value;
    if (offset == RTE_REG_LOAD_PARAM && (value & 1U) != 0) {
        mock->load_high_writes++;
        for (index = 0; index < rte_parameter_register_count; ++index) {
            uint32_t reg = rte_parameter_registers[index].offset;
            mock->active[reg / 4] = mock->staged[reg / 4];
        }
        mock->commits++;
    } else if (offset == RTE_REG_LOAD_PARAM) {
        mock->load_low_writes++;
        if (mock->stale_high_reads_after_low_write != 0) {
            size_t load_index = RTE_REG_LOAD_PARAM / 4;

            mock->stale_value[load_index] = 1;
            mock->stale_reads[load_index] =
                mock->stale_high_reads_after_low_write;
            mock->stale_high_reads_after_low_write = 0;
        }
    }
    return 0;
}

static struct rte_register_image make_image(int seed)
{
    struct rte_register_image image = {
        .delay_offset = (uint16_t)(seed + 1),
        .doppler_pinc = seed + 2,
        .doppler_phase = -(seed + 3),
        .scaling = (int16_t)(1000 + seed),
        .micro_pinc_resp = seed + 4,
        .micro_phase_resp = seed + 5,
        .micro_gain_resp = -(seed + 6),
        .micro_pinc_heart = seed + 7,
        .micro_phase_heart = seed + 8,
        .micro_gain_heart = -(seed + 9),
    };
    return image;
}

static void check_bank_matches(const uint32_t *bank,
                               const struct rte_register_image *image)
{
    size_t index;

    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        uint32_t mask = rte_parameter_registers[index].mask;

        CHECK((bank[offset / 4] & mask) ==
              (rte_register_image_word(image, offset) & mask));
    }
}

static void test_successful_transaction(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(10);
    struct rte_register_image readback;
    struct rte_error error;
    size_t index;

    mock.staged[RTE_REG_IPCORE_TIMESTAMP / 4] = RTE_EXPECTED_TIMESTAMP;
    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_validate_hardware(
              &io, RTE_EXPECTED_TIMESTAMP, &error) == 0);
    CHECK(rte_register_apply(&io, &image, NULL, &error) == 0);
    CHECK(mock.commits == 1);
    CHECK(mock.load_high_reads >= 2);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.reset_writes == 0);
    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        uint32_t mask = rte_parameter_registers[index].mask;
        CHECK((mock.active[offset / 4] & mask) ==
              (rte_register_image_word(&image, offset) & mask));
    }
    CHECK(rte_register_read_image(&io, &readback, &error) == 0);
    CHECK(readback.micro_gain_resp == image.micro_gain_resp);
    CHECK(readback.micro_gain_heart == image.micro_gain_heart);
}

static void test_stage_failure_does_not_commit(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(20);
    struct rte_error error;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    mock.fail_write_offset = RTE_REG_SCALING;
    mock.fail_write_count = 1;
    CHECK(rte_register_apply(&io, &image, NULL, &error) != 0);
    CHECK(mock.commits == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.reset_writes == 0);
}

static void test_stale_stage_readback_eventually_succeeds(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(23);
    struct rte_error error;
    size_t register_index = RTE_REG_DOPPLER_PINC / 4;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    mock.stale_value[register_index] = 0;
    mock.stale_reads[register_index] = 3;
    CHECK(rte_register_apply(&io, &image, NULL, &error) == 0);
    CHECK(mock.read_count[register_index] >= 4);
    CHECK(mock.load_high_writes == 1);
    CHECK(mock.commits == 1);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    check_bank_matches(mock.active, &image);
}

static void test_permanent_stage_mismatch_never_commits(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(24);
    struct rte_error error;
    size_t register_index = RTE_REG_DOPPLER_PINC / 4;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    mock.permanent_mismatch[register_index] = true;
    CHECK(rte_register_apply(&io, &image, NULL, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(mock.read_count[register_index] >= 1024);
    CHECK(mock.load_high_writes == 0);
    CHECK(mock.commits == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.reset_writes == 0);
}

static void test_initial_load_high_is_cleared_before_staging(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(25);
    struct rte_error error;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    mock.staged[RTE_REG_LOAD_PARAM / 4] = 1;
    CHECK(rte_register_apply(&io, &image, NULL, &error) == 0);
    CHECK(mock.parameter_writes_while_load_high == 0);
    CHECK(mock.load_low_writes >= 2);
    CHECK(mock.load_high_writes == 1);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    check_bank_matches(mock.active, &image);
}

static void test_stage_failure_restores_complete_staged_bank(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image previous = make_image(21);
    struct rte_register_image next = make_image(22);
    struct rte_error error;
    size_t index;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_apply(&io, &previous, NULL, &error) == 0);
    mock.fail_write_offset = RTE_REG_SCALING;
    mock.fail_write_count = 1;
    CHECK(rte_register_apply(&io, &next, &previous, &error) != 0);
    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        uint32_t mask = rte_parameter_registers[index].mask;
        uint32_t expected = rte_register_image_word(&previous, offset);

        CHECK((mock.staged[offset / 4] & mask) == (expected & mask));
        CHECK((mock.active[offset / 4] & mask) == (expected & mask));
    }
    CHECK(mock.reset_writes == 0);
}

static void test_commit_failure_rolls_back(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image previous = make_image(30);
    struct rte_register_image next = make_image(40);
    struct rte_error error;
    size_t index;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_apply(&io, &previous, NULL, &error) == 0);
    mock.fail_write_offset = RTE_REG_LOAD_PARAM;
    /*
     * The first load write for the next transaction fails. The transaction
     * treats an attempted commit as uncertain and safely reapplies the
     * previous complete image.
     */
    mock.fail_write_count = 1;
    CHECK(rte_register_apply(&io, &next, &previous, &error) != 0);
    for (index = 0; index < rte_parameter_register_count; ++index) {
        uint32_t offset = rte_parameter_registers[index].offset;
        uint32_t mask = rte_parameter_registers[index].mask;
        CHECK((mock.active[offset / 4] & mask) ==
              (rte_register_image_word(&previous, offset) & mask));
    }
    CHECK(mock.reset_writes == 0);
}

static void test_first_commit_failure_reports_unknown_state(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(40);
    struct rte_error error;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    mock.fail_write_offset = RTE_REG_LOAD_PARAM;
    mock.fail_write_count = 1;
    mock.fail_write_matches_value = true;
    mock.fail_write_value = 1;
    CHECK(rte_register_apply(&io, &image, NULL, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(strcmp(error.field, "commit_state") == 0);
    CHECK(mock.commits == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.reset_writes == 0);
}

static void test_high_readback_timeout_rolls_back(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image previous = make_image(41);
    struct rte_register_image next = make_image(42);
    struct rte_error error;
    size_t load_index = RTE_REG_LOAD_PARAM / 4;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_apply(&io, &previous, NULL, &error) == 0);

    /*
     * One stale read is consumed by ensure_load_low(). The following 1024
     * stale zeroes hide an accepted high write for the complete poll window.
     */
    mock.stale_value[load_index] = 0;
    mock.stale_reads[load_index] = 1025;
    CHECK(rte_register_apply(&io, &next, &previous, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(mock.stale_reads[load_index] == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.parameter_writes_while_load_high == 0);
    check_bank_matches(mock.staged, &previous);
    check_bank_matches(mock.active, &previous);
    CHECK(mock.reset_writes == 0);
}

static void test_low_write_failure_rolls_back(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image previous = make_image(43);
    struct rte_register_image next = make_image(44);
    struct rte_error error;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_apply(&io, &previous, NULL, &error) == 0);

    mock.fail_write_offset = RTE_REG_LOAD_PARAM;
    mock.fail_write_count = 1;
    mock.fail_write_matches_value = true;
    mock.fail_write_value = 0;
    CHECK(rte_register_apply(&io, &next, &previous, &error) != 0);
    CHECK(error.code == RTE_ERROR_IO);
    CHECK(mock.fail_write_count == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.parameter_writes_while_load_high == 0);
    check_bank_matches(mock.staged, &previous);
    check_bank_matches(mock.active, &previous);
    CHECK(mock.reset_writes == 0);
}

static void test_low_readback_timeout_rolls_back(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image previous = make_image(45);
    struct rte_register_image next = make_image(46);
    struct rte_error error;
    size_t load_index = RTE_REG_LOAD_PARAM / 4;

    mock.staged[RTE_REG_IPCORE_ENABLE / 4] = 1;
    CHECK(rte_register_apply(&io, &previous, NULL, &error) == 0);

    /* The low write succeeds, but its readback remains stale for all polls. */
    mock.stale_high_reads_after_low_write = 1024;
    CHECK(rte_register_apply(&io, &next, &previous, &error) != 0);
    CHECK(error.code == RTE_ERROR_HARDWARE);
    CHECK(mock.stale_reads[load_index] == 0);
    CHECK(mock.staged[RTE_REG_LOAD_PARAM / 4] == 0);
    CHECK(mock.parameter_writes_while_load_high == 0);
    check_bank_matches(mock.staged, &previous);
    check_bank_matches(mock.active, &previous);
    CHECK(mock.reset_writes == 0);
}

static void test_signed_readback_endpoints(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image;
    struct rte_error error;

    mock.staged[RTE_REG_DELAY_OFFSET / 4] = UINT32_C(0x0000ffff);
    mock.staged[RTE_REG_DOPPLER_PINC / 4] = UINT32_C(0x80000000);
    mock.staged[RTE_REG_DOPPLER_PHASE / 4] = UINT32_C(0x7fffffff);
    mock.staged[RTE_REG_SCALING / 4] = UINT32_C(0xffff8000);
    mock.staged[RTE_REG_MICRO_PINC_RESP / 4] = UINT32_C(0xffffffff);
    mock.staged[RTE_REG_MICRO_PHASE_RESP / 4] = UINT32_C(0x80000000);
    mock.staged[RTE_REG_MICRO_GAIN_RESP / 4] = UINT32_C(0xff000000);
    mock.staged[RTE_REG_MICRO_PINC_HEART / 4] = UINT32_C(0x7fffffff);
    mock.staged[RTE_REG_MICRO_PHASE_HEART / 4] = UINT32_C(0x00000000);
    mock.staged[RTE_REG_MICRO_GAIN_HEART / 4] = UINT32_C(0x00ffffff);

    CHECK(rte_register_read_image(&io, &image, &error) == 0);
    CHECK(image.delay_offset == UINT16_MAX);
    CHECK(image.doppler_pinc == INT32_MIN);
    CHECK(image.doppler_phase == INT32_MAX);
    CHECK(image.scaling == INT16_MIN);
    CHECK(image.micro_pinc_resp == -INT32_C(1));
    CHECK(image.micro_phase_resp == INT32_MIN);
    CHECK(image.micro_gain_resp == -INT32_C(16777216));
    CHECK(image.micro_pinc_heart == INT32_MAX);
    CHECK(image.micro_phase_heart == 0);
    CHECK(image.micro_gain_heart == INT32_C(16777215));
}

static void test_disabled_core_is_rejected(void)
{
    struct mock_registers mock = {0};
    struct rte_register_io io = {&mock, mock_read, mock_write};
    struct rte_register_image image = make_image(50);
    struct rte_register_image rollback = make_image(51);
    struct rte_error error;

    CHECK(rte_register_apply(&io, &image, &rollback, &error) != 0);
    CHECK(error.code == RTE_ERROR_STATE);
    CHECK(strcmp(error.field, "IPCore_Enable") == 0);
    CHECK(mock.write_attempts == 0);
    CHECK(mock.commits == 0);
    CHECK(mock.reset_writes == 0);
}

int main(void)
{
    test_successful_transaction();
    test_stage_failure_does_not_commit();
    test_stale_stage_readback_eventually_succeeds();
    test_permanent_stage_mismatch_never_commits();
    test_initial_load_high_is_cleared_before_staging();
    test_stage_failure_restores_complete_staged_bank();
    test_commit_failure_rolls_back();
    test_first_commit_failure_reports_unknown_state();
    test_high_readback_timeout_rolls_back();
    test_low_write_failure_rolls_back();
    test_low_readback_timeout_rolls_back();
    test_signed_readback_endpoints();
    test_disabled_core_is_rejected();

    if (failures != 0) {
        fprintf(stderr, "%u test failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("test_register_io: PASS");
    return EXIT_SUCCESS;
}

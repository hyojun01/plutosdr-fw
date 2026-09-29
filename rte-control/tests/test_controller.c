/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/controller.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
struct bank {
    uint32_t staged[0x170/4], active[0x170/4];
    unsigned writes[4], commits[4], fail_high;
};
static struct bank registers;
static struct rte_rf_context actual_rf;
static unsigned rf_applies;
static bool rf_fail_rollback;
static bool rf_quantized_rollback;

static int reg_read(void *context, uint32_t offset, uint32_t *value)
{
    struct bank *b = context;
    if (offset >= sizeof(b->staged) || offset % 4) return -EINVAL;
    *value = b->staged[offset/4]; return 0;
}
static int reg_write(void *context, uint32_t offset, uint32_t value)
{
    struct bank *b = context;
    for (unsigned t = 0; t < 4; t++) {
        for (unsigned j = 0; j < 7; j++) {
            if (rte_target_registers[t][j].offset != offset) continue;
            b->writes[t]++;
            if (j == RTE_PARAM_LP && value && b->fail_high) { b->fail_high--; return -EIO; }
            b->staged[offset/4] = value;
            if (j == RTE_PARAM_LP && value) {
                b->commits[t]++;
                for (unsigned p = 0; p < 6; p++) {
                    uint32_t addr = rte_target_registers[t][p].offset;
                    b->active[addr/4] = b->staged[addr/4];
                }
            }
            return 0;
        }
    }
    CHECK(false); return -EINVAL;
}
int rte_mmio_open(struct rte_mmio *mmio, const char *path, struct rte_error *error)
{
    (void)path; (void)error;
    mmio->io = (struct rte_register_io){&registers, reg_read, reg_write}; return 0;
}
void rte_mmio_close(struct rte_mmio *mmio) { (void)mmio; }
int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error) { (void)device; (void)error; return 0; }
void rte_ad936x_close(struct rte_ad936x *device) { (void)device; }
int rte_ad936x_read_rf(struct rte_ad936x *device, struct rte_rf_context *rf, struct rte_error *error)
{
    (void)device;
    if (actual_rf.sample_rate_hz != RTE_FIXED_SAMPLE_RATE_HZ || !actual_rf.manual_gain) {
        error->code = RTE_ERROR_RF_CONTEXT; return -EINVAL;
    }
    *rf = actual_rf; return 0;
}
int rte_ad936x_capabilities(struct rte_ad936x *device, struct rte_rf_capabilities *caps, struct rte_error *error)
{
    (void)device; (void)error; memset(caps, 0, sizeof(*caps));
    caps->max_rx_gain_mdb = actual_rf.rx_lo_hz > 4000000000 ? 62000 : 71000;
    return 0;
}
int rte_ad936x_apply_rf(struct rte_ad936x *device, const struct rte_rf_context *request,
                       struct rte_rf_context *applied, struct rte_error *error)
{
    (void)device; rf_applies++;
    if (rf_fail_rollback) { error->code = RTE_ERROR_HARDWARE; strcpy(error->field, "rf_rollback"); return -EIO; }
    if (rf_quantized_rollback) {
        actual_rf.rx_lo_hz-=2; actual_rf.tx_lo_hz-=2; *applied=actual_rf;
        error->code=RTE_ERROR_IO; strcpy(error->field,"bandwidth_hz"); return -EIO;
    }
    actual_rf = *request; *applied = actual_rf; return 0;
}
static void init(struct rte_controller *controller)
{
    struct rte_error error;
    memset(&registers, 0, sizeof(registers)); rf_applies = 0; rf_fail_rollback = false;
    rf_quantized_rollback=false;
    registers.staged[RTE_REG_IPCORE_TIMESTAMP/4] = RTE_EXPECTED_TIMESTAMP;
    registers.staged[RTE_REG_IPCORE_ENABLE/4] = 1;
    actual_rf = (struct rte_rf_context){.sample_rate_hz=61440000, .rx_lo_hz=2450000000,
        .tx_lo_hz=2450000000, .rx_bandwidth_hz=30000000, .tx_bandwidth_hz=30000000,
        .tx_gain_mdb=-10000, .rx_gain_mdb=50000, .manual_gain=true, .compatible="ad9364"};
    CHECK(rte_controller_open(controller, "/mock", &error) == 0);
    CHECK(controller->state.last_hardware_check_ms > 0);
    for (unsigned t = 0; t < 4; t++) {
        CHECK(!controller->state.targets[t].has_config);
        CHECK(!controller->state.targets[t].hardware_state_known);
        CHECK(registers.writes[t] == 0);
    }
}
static struct rte_config config(void)
{ return (struct rte_config){.enabled=true, .range_m=150.6, .radial_velocity_mps=10, .gain_linear=0.25, .phase_offset_deg=0}; }
static void check_only(unsigned selected)
{ for (unsigned t = 0; t < 4; t++) if (t != selected) CHECK(registers.writes[t] == 0); }

static void independent_first_target_and_rf(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg = config();
    struct rte_rf_patch patch = {.has_carrier_hz=true, .carrier_hz=2500000000};
    init(&c);
    CHECK(rte_controller_apply_target(&c, 2, &cfg, true, 0, &s, &e) == 0);
    CHECK(s.targets[2].has_config && s.targets[2].hardware_state_known);
    CHECK(s.revision == 1 && registers.commits[2] == 1);
    CHECK(rf_applies == 0); check_only(2);
    struct rte_register_image image = s.targets[2].image;
    unsigned writes = registers.writes[2];
    CHECK(rte_controller_apply_rf(&c, &patch, true, 1, &s, &e) == 0);
    CHECK(registers.writes[2] == writes && rf_applies == 1); check_only(2);
    CHECK(s.targets[2].needs_reapply && s.targets[2].encoded_carrier_hz == 2450000000);
    CHECK(!memcmp(&s.targets[2].image, &image, sizeof(image)));
    CHECK(!s.targets[0].has_config && !s.targets[1].has_config && !s.targets[3].has_config);
    CHECK(rte_controller_apply_target(&c, 2, &cfg, true, 2, &s, &e) == 0);
    CHECK(!s.targets[2].needs_reapply && s.targets[2].encoded_carrier_hz == 2500000000);
    CHECK(rf_applies == 1); check_only(2);
    rte_controller_close(&c);

    init(&c);
    CHECK(rte_controller_apply_rf(&c, &patch, true, 0, &s, &e) == 0);
    for (unsigned t = 0; t < 4; t++) CHECK(registers.writes[t] == 0);
    CHECK(!s.targets[0].has_config && s.revision == 1);
    rte_controller_close(&c);
}
static void selected_rollback_and_first_failure(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg = config();
    init(&c);
    CHECK(rte_controller_apply_target(&c, 1, &cfg, true, 0, &s, &e) == 0);
    struct rte_register_image before = s.targets[1].image;
    cfg.gain_linear = 0.5; registers.fail_high = 1;
    CHECK(rte_controller_apply_target(&c, 1, &cfg, true, 1, &s, &e) != 0);
    CHECK(!s.degraded && s.revision == 1 && s.targets[1].hardware_state_known);
    CHECK(!memcmp(&s.targets[1].image, &before, sizeof(before)));
    for (unsigned j = 0; j < 6; j++) CHECK(registers.active[rte_target_registers[1][j].offset/4] == before.words[j]);
    check_only(1); CHECK(rf_applies == 0);
    rte_controller_close(&c);
    init(&c); registers.fail_high = 1;
    CHECK(rte_controller_apply_target(&c, 1, &cfg, true, 0, &s, &e) != 0);
    CHECK(s.degraded && !s.targets[1].hardware_state_known && s.revision == 0);
    CHECK(!strcmp(e.field, "commit_state")); check_only(1);
    unsigned writes = registers.writes[1];
    CHECK(rte_controller_apply_target(&c, 1, &cfg, true, 0, &s, &e) != 0);
    CHECK(registers.writes[1] == writes);
    rte_controller_close(&c);
}
static void conflict_and_external_change(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg = config();
    init(&c);
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 1, &s, &e) != 0);
    CHECK(e.code == RTE_ERROR_CONFLICT && registers.writes[0] == 0);
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 0, &s, &e) == 0);
    actual_rf.rx_lo_hz = actual_rf.tx_lo_hz = 3000000000;
    unsigned writes = registers.writes[0];
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 1, &s, &e) != 0);
    CHECK(e.code == RTE_ERROR_CONFLICT && s.targets[0].needs_reapply);
    CHECK(s.revision == 2 && registers.writes[0] == writes);
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 2, &s, &e) == 0);
    registers.staged[rte_target_registers[0][RTE_PARAM_SC].offset/4]++;
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 3, &s, &e) != 0);
    CHECK(s.degraded && !s.targets[0].hardware_state_known);
    rte_controller_close(&c);
    init(&c); actual_rf.sample_rate_hz = 60000000;
    CHECK(rte_controller_apply_target(&c, 0, &cfg, true, 0, &s, &e) != 0);
    CHECK(registers.writes[0] == 0);
    rte_controller_close(&c);
}
static void rf_gain_and_failed_rollback(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg = config();
    struct rte_rf_patch patch = {.has_tx_gain_db=true, .tx_gain_db=-0.25,
                                .has_rx_gain_db=true, .rx_gain_db=0};
    init(&c);
    CHECK(rte_controller_apply_target(&c, 3, &cfg, true, 0, &s, &e) == 0);
    unsigned writes = registers.writes[3];
    CHECK(rte_controller_apply_rf(&c, &patch, true, 1, &s, &e) == 0);
    CHECK(!s.targets[3].needs_reapply && s.rf.tx_gain_mdb == -250 && s.rf.rx_gain_mdb == 0);
    CHECK(registers.writes[3] == writes); check_only(3);
    rf_fail_rollback = true;
    CHECK(rte_controller_apply_rf(&c, &patch, true, 2, &s, &e) != 0);
    CHECK(s.degraded && s.revision == 2 && !strcmp(e.field, "rf_rollback"));
    CHECK(registers.writes[3] == writes); check_only(3);
    unsigned calls = rf_applies;
    CHECK(rte_controller_apply_rf(&c, &patch, true, 2, &s, &e) != 0);
    CHECK(rf_applies == calls);
    rte_controller_close(&c);
}
static void capabilities_observe_external_rf(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg = config(); struct rte_rf_capabilities caps;
    init(&c);
    CHECK(rte_controller_apply_target(&c, 1, &cfg, true, 0, &s, &e) == 0);
    unsigned writes = registers.writes[1];
    actual_rf.rx_lo_hz = actual_rf.tx_lo_hz = 4500000000;
    CHECK(rte_controller_rf_capabilities(&c, &caps, &e) == 0);
    CHECK(caps.max_rx_gain_mdb == 62000);
    CHECK(rte_controller_snapshot(&c, &s, &e) == 0);
    CHECK(s.revision == 2 && s.rf.rx_lo_hz == 4500000000 && s.targets[1].needs_reapply);
    CHECK(s.targets[1].encoded_carrier_hz == 2450000000);
    CHECK(registers.writes[1] == writes && rf_applies == 0); check_only(1);
    CHECK(rte_controller_rf_capabilities(&c, &caps, &e) == 0);
    CHECK(c.state.revision == 2);
    c.state.degraded = true;
    CHECK(rte_controller_rf_capabilities(&c, &caps, &e) == 0 && c.state.degraded);
    CHECK(registers.writes[1] == writes && rf_applies == 0);
    rte_controller_close(&c);
}
static void quantized_rf_rollback_observation(void)
{
    struct rte_controller c; struct rte_snapshot s; struct rte_error e;
    struct rte_config cfg=config();
    struct rte_rf_patch patch={.has_carrier_hz=true,.carrier_hz=2500000000};
    init(&c);
    CHECK(rte_controller_apply_target(&c,1,&cfg,true,0,&s,&e) == 0);
    unsigned writes=registers.writes[1];
    rf_quantized_rollback=true;
    CHECK(rte_controller_apply_rf(&c,&patch,true,1,&s,&e) != 0);
    CHECK(!s.degraded && s.revision == 2 && s.rf.rx_lo_hz == 2449999998);
    CHECK(s.targets[1].needs_reapply && s.targets[1].encoded_carrier_hz == 2450000000);
    CHECK(registers.writes[1] == writes); check_only(1);
    CHECK(rte_controller_apply_target(&c,1,&cfg,true,1,&s,&e) != 0);
    CHECK(e.code == RTE_ERROR_CONFLICT && registers.writes[1] == writes);
    rte_controller_close(&c);
}
int main(void)
{
    independent_first_target_and_rf(); selected_rollback_and_first_failure(); conflict_and_external_change();
    rf_gain_and_failed_rollback();
    capabilities_observe_external_rf();
    quantized_rf_rollback_observation();
    if (failures) return EXIT_FAILURE;
    puts("test_controller: PASS"); return EXIT_SUCCESS;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compile this translation unit alone with the core library; actual adapter
 * source is included so only sysfs fopen and public libiio calls are mocked. */
#include <rte/ad936x.h>
#include <iio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static FILE *mock_fopen(const char *, const char *);
#define fopen mock_fopen
#include "../src/ad936x.c"
#undef fopen

struct iio_context { int unused; };
struct iio_device { int unused; };
struct iio_channel { unsigned id; };
static struct iio_context ctx;
static struct iio_device phy;
static struct iio_channel chans[4] = {{0},{1},{2},{3}};
static const char *boot_compatible;
static const char *gain_mode;
static struct rte_rf_context hardware;
static unsigned writes, sample_writes, mode_writes, writes_by_ch[4];
static unsigned fail_write, fail_write2, clamp_write, read_failure_after_write;
static bool retune_gain;
static uint64_t tx_sample_override;
static uint64_t synth_reference_hz;
static int initial_lo_bias_hz;
static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static bool rf_equal(const struct rte_rf_context *a, const struct rte_rf_context *b)
{
    return rf_matches(a, b) && a->rx_lo_hz == b->rx_lo_hz && a->tx_lo_hz == b->tx_lo_hz;
}

static FILE *mock_fopen(const char *path, const char *mode)
{
    FILE *f;
    CHECK(!strcmp(path, "/sys/bus/iio/devices/iio:device1/of_node/compatible"));
    CHECK(!strcmp(mode, "rb"));
    f = tmpfile();
    if (f) { fwrite(boot_compatible, 1, strlen(boot_compatible)+1, f); rewind(f); }
    return f;
}
struct iio_context *iio_create_local_context(void) { return &ctx; }
void iio_context_destroy(struct iio_context *context) { CHECK(context == &ctx); }
struct iio_device *iio_context_find_device(const struct iio_context *context, const char *name)
{ CHECK(context == &ctx); return !strcmp(name, "ad9361-phy") ? &phy : NULL; }
const char *iio_device_get_id(const struct iio_device *device) { CHECK(device == &phy); return "iio:device1"; }
struct iio_channel *iio_device_find_channel(const struct iio_device *device, const char *name, bool output)
{
    CHECK(device == &phy);
    if (!strcmp(name, "voltage0")) return &chans[output ? 1 : 0];
    if (!strcmp(name, "altvoltage0") && output) return &chans[2];
    if (!strcmp(name, "altvoltage1") && output) return &chans[3];
    return NULL;
}
int iio_channel_attr_read_longlong(const struct iio_channel *channel, const char *attr, long long *value)
{
    if (read_failure_after_write && writes >= read_failure_after_write) return -EIO;
    if (!strcmp(attr, "sampling_frequency")) *value = channel->id == 1 && tx_sample_override ? tx_sample_override : hardware.sample_rate_hz;
    else if (!strcmp(attr, "frequency")) *value = channel->id == 2 ? hardware.rx_lo_hz : hardware.tx_lo_hz;
    else if (!strcmp(attr, "rf_bandwidth")) *value = channel->id == 0 ? hardware.rx_bandwidth_hz : hardware.tx_bandwidth_hz;
    else return -ENOENT;
    return 0;
}
int iio_channel_attr_read_double(const struct iio_channel *channel, const char *attr, double *value)
{
    CHECK(!strcmp(attr, "hardwaregain"));
    *value = (channel->id == 0 ? hardware.rx_gain_mdb : hardware.tx_gain_mdb) / 1000.0;
    return 0;
}
ssize_t iio_channel_attr_read(const struct iio_channel *channel, const char *attr, char *dst, size_t len)
{
    const char *text;
    if (!strcmp(attr, "gain_control_mode")) text = gain_mode;
    else if (!strcmp(attr, "frequency_available")) text = channel->id == 2 ? "[70000000 1 6000000000]" : "[46875001 1 6000000000]";
    else if (!strcmp(attr, "rf_bandwidth_available")) text = channel->id == 0 ? "[200000 1 56000000]" : "[200000 1 40000000]";
    else if (!strcmp(attr, "hardwaregain_available")) {
        if (channel->id == 1) text = "[-89.750000 0.250000 0.000000]";
        else text = hardware.rx_lo_hz <= 1300000000 ? "[-1 1 73]" : hardware.rx_lo_hz <= 4000000000 ? "[-3 1 71]" : "[-10 1 62]";
    } else return -ENOENT;
    if (strlen(text) >= len) return -ENOSPC;
    strcpy(dst, text); return (ssize_t)strlen(text);
}
static int before_write(const struct iio_channel *channel, const char *attr)
{
    writes++; writes_by_ch[channel->id]++;
    if (!strcmp(attr, "sampling_frequency")) sample_writes++;
    if (!strcmp(attr, "gain_control_mode")) mode_writes++;
    if (writes == fail_write || writes == fail_write2) return -EIO;
    return 0;
}
/* Match linux/drivers/iio/adc/ad9361.c: to/from_clk, RFPLL divider and
 * recalc_rate. A calibrated reference need not be an exact 40/80 MHz. */
static uint64_t quantized_lo(uint64_t hz)
{
    uint64_t vco = (hz / 2) * 2, divider = 1, integer, remainder, fraction;
    if (!synth_reference_hz) return hz;
    while (vco <= UINT64_C(6000000000)) { vco *= 2; divider *= 2; }
    integer = vco / synth_reference_hz;
    remainder = vco % synth_reference_hz;
    fraction = (remainder * UINT64_C(8388593) + synth_reference_hz / 2) / synth_reference_hz;
    return ((synth_reference_hz * integer + synth_reference_hz * fraction / UINT64_C(8388593)) / divider) / 2 * 2;
}
int iio_channel_attr_write_longlong(const struct iio_channel *channel, const char *attr, long long value)
{
    int status = before_write(channel, attr);
    if (status) return status;
    if (writes == clamp_write) value--;
    if (!strcmp(attr, "frequency")) {
        value = (long long)quantized_lo((uint64_t)value);
        if (writes <= 2) value += initial_lo_bias_hz;
        if (channel->id == 2) {
            hardware.rx_lo_hz = (uint64_t)value;
            if (retune_gain) hardware.rx_gain_mdb = 40000;
        } else hardware.tx_lo_hz = (uint64_t)value;
    } else if (!strcmp(attr, "rf_bandwidth")) {
        if (channel->id == 0) hardware.rx_bandwidth_hz = (uint64_t)value;
        else hardware.tx_bandwidth_hz = (uint64_t)value;
    } else if (!strcmp(attr, "sampling_frequency")) hardware.sample_rate_hz = (uint64_t)value;
    else CHECK(false);
    return 0;
}
int iio_channel_attr_write_double(const struct iio_channel *channel, const char *attr, double value)
{
    int status = before_write(channel, attr);
    if (status) return status;
    CHECK(!strcmp(attr, "hardwaregain"));
    if (writes == clamp_write) value -= 0.25;
    if (channel->id == 0) hardware.rx_gain_mdb = (int32_t)llround(value*1000);
    else hardware.tx_gain_mdb = (int32_t)llround(value*1000);
    return 0;
}
static void reset(void)
{
    boot_compatible = "ad9364"; gain_mode = "manual";
    hardware = (struct rte_rf_context){.sample_rate_hz=61440000, .rx_lo_hz=2450000000,
        .tx_lo_hz=2450000000, .rx_bandwidth_hz=30000000, .tx_bandwidth_hz=30000000,
        .tx_gain_mdb=-10000, .rx_gain_mdb=50000, .manual_gain=true, .compatible="ad9364"};
    writes=sample_writes=mode_writes=fail_write=fail_write2=clamp_write=read_failure_after_write=0;
    memset(writes_by_ch, 0, sizeof(writes_by_ch)); retune_gain=false;
    tx_sample_override=synth_reference_hz=0;
    initial_lo_bias_hz=0;
}
static void close_checked(struct rte_ad936x *d)
{ CHECK(sample_writes == 0 && mode_writes == 0); rte_ad936x_close(d); }
static void test_open_and_ranges(void)
{
    struct rte_ad936x d; struct rte_error e; struct rte_rf_capabilities caps;
    reset(); CHECK(rte_ad936x_open(&d, &e) == 0); CHECK(writes == 0);
    CHECK(rte_ad936x_capabilities(&d, &caps, &e) == 0);
    CHECK(caps.min_carrier_hz == 70000000 && caps.max_carrier_hz == 6000000000);
    CHECK(caps.min_bandwidth_hz == 200000 && caps.max_bandwidth_hz == 40000000);
    CHECK(caps.min_tx_gain_mdb == -89750 && caps.tx_gain_step_mdb == 250);
    CHECK(caps.min_rx_gain_mdb == -3000 && caps.max_rx_gain_mdb == 71000);
    close_checked(&d);
    reset(); boot_compatible="ad9363a"; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(!strcmp(e.field,"compatible")); CHECK(writes == 0);
    reset(); gain_mode="slow_attack"; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(writes == 0);
    reset(); hardware.sample_rate_hz=60000000; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(writes == 0);
    reset(); tx_sample_override=60000000; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(writes == 0);
    reset(); hardware.tx_bandwidth_hz=20000000; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(writes == 0);
    reset(); hardware.tx_lo_hz=2500000000; CHECK(rte_ad936x_open(&d, &e) != 0); CHECK(writes == 0);
}
static void test_gain_and_noop(void)
{
    struct rte_ad936x d; struct rte_error e; struct rte_rf_context desired, applied;
    const int32_t tx[] = {-250, 0, -89750}, rx[] = {-3000, 0, 71000};
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0);
    desired = hardware;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0 && writes == 0);
    for (unsigned i=0; i<3; i++) {
        desired.tx_gain_mdb=tx[i]; desired.rx_gain_mdb=rx[i];
        CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0);
        CHECK(applied.tx_gain_mdb == tx[i] && applied.rx_gain_mdb == rx[i]);
    }
    unsigned before=writes;
    desired.tx_gain_mdb=-100; CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && writes == before);
    desired=hardware; desired.rx_gain_mdb=500; CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && writes == before);
    desired=hardware; desired.rx_bandwidth_hz=desired.tx_bandwidth_hz=41000000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && writes == before);
    desired=hardware; desired.sample_rate_hz=60000000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && writes == before);
    close_checked(&d);
}
static void test_retune_and_rollback(void)
{
    struct rte_ad936x d; struct rte_error e; struct rte_rf_context desired, applied, previous;
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0); previous=hardware;
    retune_gain=true; desired=hardware; desired.rx_lo_hz=desired.tx_lo_hz=4500000000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0);
    CHECK(writes == 3 && applied.rx_gain_mdb == 50000); /* two LO writes, then gain repair */
    close_checked(&d);
    reset(); hardware.rx_gain_mdb=70000; CHECK(rte_ad936x_open(&d,&e) == 0); previous=hardware;
    desired=hardware; desired.rx_lo_hz=desired.tx_lo_hz=4500000000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0);
    CHECK(!strcmp(e.field,"rx_gain_db") && rf_equal(&hardware,&previous));
    CHECK(writes == 4); close_checked(&d);
    /* A gain valid only in the new LO band must be checked after retuning. */
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0); desired=hardware;
    desired.rx_lo_hz=desired.tx_lo_hz=4500000000; desired.rx_gain_mdb=-10000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0); close_checked(&d);
    for (unsigned failure=1; failure<=6; failure++) {
        reset(); CHECK(rte_ad936x_open(&d,&e) == 0); previous=hardware; desired=hardware;
        desired.rx_lo_hz=desired.tx_lo_hz=2500000000;
        desired.rx_bandwidth_hz=desired.tx_bandwidth_hz=18000000;
        desired.tx_gain_mdb=-250; desired.rx_gain_mdb=0; fail_write=failure;
        CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0);
        CHECK(rf_equal(&hardware,&previous) && strcmp(e.field,"rf_rollback")); close_checked(&d);
    }
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0); previous=hardware; desired=hardware;
    desired.rx_bandwidth_hz=desired.tx_bandwidth_hz=18000000; clamp_write=2;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && rf_equal(&hardware,&previous)); close_checked(&d);
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0); desired=hardware;
    desired.rx_lo_hz=desired.tx_lo_hz=2500000000; fail_write=2; fail_write2=3;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && !strcmp(e.field,"rf_rollback")); close_checked(&d);
    reset(); CHECK(rte_ad936x_open(&d,&e) == 0); desired=hardware;
    desired.rx_lo_hz=desired.tx_lo_hz=2500000000; read_failure_after_write=1;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && !strcmp(e.field,"rf_rollback")); close_checked(&d);
}
static void test_pll_quantization(void)
{
    struct rte_ad936x d; struct rte_error e; struct rte_rf_context desired, applied;
    reset(); synth_reference_hz=79999984;
    hardware.rx_lo_hz=hardware.tx_lo_hz=quantized_lo(2400000000);
    CHECK(hardware.rx_lo_hz == 2399999998);
    CHECK(rte_ad936x_open(&d,&e) == 0);
    desired=hardware; desired.rx_lo_hz=desired.tx_lo_hz=2450000000;
    desired.rx_bandwidth_hz=desired.tx_bandwidth_hz=40000000;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0);
    CHECK(applied.rx_lo_hz == 2449999998 && applied.tx_lo_hz == 2449999998);
    CHECK(applied.rx_bandwidth_hz == 40000000 && applied.tx_bandwidth_hz == 40000000);
    CHECK(applied.tx_gain_mdb == -10000 && applied.rx_gain_mdb == 50000);
    unsigned before=writes;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0 && writes == before);
    desired.tx_gain_mdb=-10250;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0 && writes == before+1);
    CHECK(writes_by_ch[2] == 1 && writes_by_ch[3] == 1);
    close_checked(&d);
    /* TX write failure after RX retune: rollback must retune both chains even
     * when TX still equals the original, already quantized readback. */
    reset(); synth_reference_hz=79999984;
    hardware.rx_lo_hz=hardware.tx_lo_hz=quantized_lo(2400000000);
    uint64_t restored_lo=quantized_lo(hardware.rx_lo_hz);
    CHECK(rte_ad936x_open(&d,&e) == 0);
    desired=hardware; desired.rx_lo_hz=desired.tx_lo_hz=2450000000; fail_write=2;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0);
    CHECK(strcmp(e.field,"rf_rollback") && writes == 4);
    CHECK(applied.rx_lo_hz == restored_lo && applied.tx_lo_hz == restored_lo);
    CHECK(rf_equal(&applied,&hardware));
    close_checked(&d);
    /* Only LO gets a tolerance; a real common-LO error still rolls back. */
    for (int bias=-6; bias<=6; ++bias) {
        reset(); initial_lo_bias_hz=bias;
        CHECK(rte_ad936x_open(&d,&e) == 0);
        desired=hardware; desired.rx_lo_hz=desired.tx_lo_hz=2500000000;
        int result=rte_ad936x_apply_rf(&d,&desired,&applied,&e);
        if (abs(bias) <= 5) {
            CHECK(result == 0 && applied.rx_lo_hz == (uint64_t)(INT64_C(2500000000)+bias));
        } else {
            CHECK(result != 0 && !strcmp(e.field,"rf_readback"));
            CHECK(strstr(e.message,"LO readback differs") && applied.rx_lo_hz == 2450000000);
        }
        close_checked(&d);
    }
    /* A current readback at an endpoint may be just outside the nominal range.
     * Gain-only writes may retain it; a new out-of-range setpoint is rejected. */
    reset(); hardware.rx_lo_hz=hardware.tx_lo_hz=69999998;
    CHECK(rte_ad936x_open(&d,&e) == 0);
    desired=hardware; desired.tx_gain_mdb=-10250;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) == 0);
    CHECK(writes == 1 && applied.rx_lo_hz == 69999998);
    desired.rx_lo_hz=desired.tx_lo_hz=69999999;
    CHECK(rte_ad936x_apply_rf(&d,&desired,&applied,&e) != 0 && writes == 1);
    close_checked(&d);
}
int main(void)
{
    test_open_and_ranges(); test_gain_and_noop(); test_retune_and_rollback(); test_pll_quantization();
    if (failures) return EXIT_FAILURE;
    puts("test_ad936x: PASS (all paths: sample-rate and gain-mode writes = 0)"); return EXIT_SUCCESS;
}

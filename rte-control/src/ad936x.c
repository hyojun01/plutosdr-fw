/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/ad936x.h>
#include <errno.h>
#include <inttypes.h>
#include <iio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef RTE_IIO_SYSFS_ROOT
#define RTE_IIO_SYSFS_ROOT "/sys/bus/iio/devices"
#endif

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

static int read_hz(struct iio_channel *channel, const char *attr,
                   uint64_t *value, const char *field, struct rte_error *error)
{
    long long result;
    int status = iio_channel_attr_read_longlong(channel, attr, &result);
    if (status < 0 || result <= 0) {
        set_error(error, RTE_ERROR_IO, field, "cannot read valid %s: %d", attr, status);
        return status < 0 ? status : -ERANGE;
    }
    *value = (uint64_t)result;
    return 0;
}

static int read_gain(struct iio_channel *channel, int32_t *value,
                     const char *field, struct rte_error *error)
{
    double result;
    int status = iio_channel_attr_read_double(channel, "hardwaregain", &result);
    if (status < 0 || !isfinite(result) || fabs(result) > 1000.0 ||
        fabs(result * 1000.0 - round(result * 1000.0)) > 0.0001) {
        set_error(error, RTE_ERROR_IO, field, "cannot read valid hardwaregain: %d", status);
        return status < 0 ? status : -ERANGE;
    }
    *value = (int32_t)llround(result * 1000.0);
    return 0;
}

static int write_hz(struct iio_channel *channel, const char *attr, uint64_t value,
                    const char *field, struct rte_error *error)
{
    int status;
    /* This helper deliberately has no sampling_frequency or gain mode path. */
    if ((strcmp(attr, "frequency") && strcmp(attr, "rf_bandwidth")) ||
        !value || value > INT64_MAX) return -EINVAL;
    status = iio_channel_attr_write_longlong(channel, attr, (long long)value);
    if (status < 0) set_error(error, RTE_ERROR_IO, field, "cannot write %s: %d", attr, status);
    return status < 0 ? status : 0;
}

static int write_gain(struct iio_channel *channel, int32_t value,
                      const char *field, struct rte_error *error)
{
    int status = iio_channel_attr_write_double(channel, "hardwaregain", value / 1000.0);
    if (status < 0) set_error(error, RTE_ERROR_IO, field, "cannot write hardwaregain: %d", status);
    return status < 0 ? status : 0;
}

static int compatible_profile(struct rte_ad936x *device, struct rte_error *error)
{
    if (strcmp(device->compatible, "ad9364") && strcmp(device->compatible, "adi,ad9364")) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "compatible",
                  "AD9364 boot profile required (found %.31s); set U-Boot attr_name=compatible, attr_val=ad9364 and reboot",
                  device->compatible);
        return -EINVAL;
    }
    return 0;
}

int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error)
{
    char path[256], compatible[128];
    const char *id;
    FILE *file;
    size_t bytes;
    struct rte_rf_context rf;
    int status;
    rte_error_clear(error);
    if (!device) return -EINVAL;
    memset(device, 0, sizeof(*device));
    device->context = iio_create_local_context();
    if (!device->context) goto unavailable;
    device->phy = iio_context_find_device(device->context, "ad9361-phy");
    if (!device->phy) goto unavailable;
    device->rx_sample = iio_device_find_channel(device->phy, "voltage0", false);
    device->tx_sample = iio_device_find_channel(device->phy, "voltage0", true);
    device->rx_lo = iio_device_find_channel(device->phy, "altvoltage0", true);
    device->tx_lo = iio_device_find_channel(device->phy, "altvoltage1", true);
    if (!device->rx_sample || !device->tx_sample || !device->rx_lo || !device->tx_lo)
        goto unavailable;
    id = iio_device_get_id(device->phy);
    if (!id || strncmp(id, "iio:device", 10) || strchr(id, '/')) goto unavailable;
    snprintf(path, sizeof(path), "%s/%s/of_node/compatible", RTE_IIO_SYSFS_ROOT, id);
    file = fopen(path, "rb");
    if (!file) {
        set_error(error, RTE_ERROR_IO, "compatible", "cannot read AD936x device-tree compatible: %s", strerror(errno));
        rte_ad936x_close(device);
        return -EIO;
    }
    bytes = fread(compatible, 1, sizeof(compatible) - 1, file);
    fclose(file);
    compatible[bytes] = '\0';
    /* Linux DT strings may be NUL-separated; U-Boot normally installs one. */
    snprintf(device->compatible, sizeof(device->compatible), "%.31s", compatible);
    for (size_t pos = 0; pos < bytes; pos += strlen(compatible + pos) + 1) {
        if (!strcmp(compatible + pos, "ad9364") || !strcmp(compatible + pos, "adi,ad9364")) {
            snprintf(device->compatible, sizeof(device->compatible), "ad9364");
            break;
        }
    }
    status = rte_ad936x_read_rf(device, &rf, error);
    if (status) rte_ad936x_close(device);
    return status;
unavailable:
    set_error(error, RTE_ERROR_HARDWARE, "ad936x", "local ad9361-phy and its RX/TX channels are required");
    rte_ad936x_close(device);
    return -ENODEV;
}

void rte_ad936x_close(struct rte_ad936x *device)
{
    if (!device) return;
    if (device->context) iio_context_destroy(device->context);
    memset(device, 0, sizeof(*device));
}

/* Permit unequal LO/bandwidth only while inspecting a partial transaction for rollback. */
static int read_raw(struct rte_ad936x *device, struct rte_rf_context *rf, struct rte_error *error)
{
    uint64_t tx_sample_rate;
    char mode[64];
    ssize_t count;
    int status;
    memset(rf, 0, sizeof(*rf));
    if (!device || !device->context) return -EINVAL;
    status = compatible_profile(device, error);
    if (status) return status;
#define READ_HZ(ch, attr, dest, field) do { status = read_hz(ch, attr, dest, field, error); if (status) return status; } while (0)
    READ_HZ(device->rx_sample, "sampling_frequency", &rf->sample_rate_hz, "sample_rate_hz");
    READ_HZ(device->tx_sample, "sampling_frequency", &tx_sample_rate, "sample_rate_hz");
    if (rf->sample_rate_hz != RTE_FIXED_SAMPLE_RATE_HZ || tx_sample_rate != RTE_FIXED_SAMPLE_RATE_HZ) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "sample_rate_hz", "RX and TX sample rates must remain fixed at 61440000 Hz");
        return -EINVAL;
    }
    count = iio_channel_attr_read(device->rx_sample, "gain_control_mode", mode, sizeof(mode) - 1);
    if (count < 0) {
        set_error(error, RTE_ERROR_IO, "gain_control_mode", "cannot read RX gain control mode");
        return (int)count;
    }
    mode[count] = '\0';
    mode[strcspn(mode, "\r\n")] = '\0';
    rf->manual_gain = !strcmp(mode, "manual");
    if (!rf->manual_gain) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "gain_control_mode", "firmware RX gain_control_mode must already be manual");
        return -EINVAL;
    }
    READ_HZ(device->rx_lo, "frequency", &rf->rx_lo_hz, "carrier_hz");
    READ_HZ(device->tx_lo, "frequency", &rf->tx_lo_hz, "carrier_hz");
    READ_HZ(device->rx_sample, "rf_bandwidth", &rf->rx_bandwidth_hz, "bandwidth_hz");
    READ_HZ(device->tx_sample, "rf_bandwidth", &rf->tx_bandwidth_hz, "bandwidth_hz");
#undef READ_HZ
    status = read_gain(device->tx_sample, &rf->tx_gain_mdb, "tx_gain_db", error);
    if (status) return status;
    status = read_gain(device->rx_sample, &rf->rx_gain_mdb, "rx_gain_db", error);
    snprintf(rf->compatible, sizeof(rf->compatible), "%s", device->compatible);
    return status;
}

int rte_ad936x_read_rf(struct rte_ad936x *device, struct rte_rf_context *rf, struct rte_error *error)
{
    int status;
    rte_error_clear(error);
    if (!rf) return -EINVAL;
    status = read_raw(device, rf, error);
    if (status) return status;
    if (rf->rx_lo_hz != rf->tx_lo_hz || rf->rx_bandwidth_hz != rf->tx_bandwidth_hz) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "rf", "RX/TX must have identical LO and bandwidth");
        return -EINVAL;
    }
    return 0;
}

static int read_range(struct iio_channel *channel, const char *attr, double range[3], struct rte_error *error)
{
    char text[160], extra;
    ssize_t count = iio_channel_attr_read(channel, attr, text, sizeof(text) - 1);
    if (count < 0) goto invalid;
    text[count] = '\0';
    if (sscanf(text, " [ %lf %lf %lf ] %c", &range[0], &range[1], &range[2], &extra) != 3 ||
        !isfinite(range[0]) || !isfinite(range[1]) || !isfinite(range[2]) ||
        range[0] > range[2] || range[1] <= 0) goto invalid;
    return 0;
invalid:
    set_error(error, RTE_ERROR_RF_CONTEXT, attr, "valid driver %s range is required", attr);
    return count < 0 ? (int)count : -EINVAL;
}

static int capabilities_raw(struct rte_ad936x *device, struct rte_rf_capabilities *caps, struct rte_error *error)
{
    double rx[3], tx[3];
    int status = compatible_profile(device, error);
    if (status) return status;
#define RANGE(ch, attr, out) do { status = read_range(ch, attr, out, error); if (status) return status; } while (0)
    RANGE(device->rx_lo, "frequency_available", rx);
    RANGE(device->tx_lo, "frequency_available", tx);
    if (rx[1] != 1 || tx[1] != 1 || rx[0] < 1 || tx[0] < 1 || rx[2] > 6e9 || tx[2] > 6e9) goto invalid;
    caps->min_carrier_hz = (uint64_t)fmax(70000000, fmax(rx[0], tx[0]));
    caps->max_carrier_hz = (uint64_t)fmin(6000000000, fmin(rx[2], tx[2]));
    RANGE(device->rx_sample, "rf_bandwidth_available", rx);
    RANGE(device->tx_sample, "rf_bandwidth_available", tx);
    if (rx[1] != 1 || tx[1] != 1 || rx[0] < 1 || tx[0] < 1 || rx[2] > 56e6 || tx[2] > 40e6) goto invalid;
    caps->min_bandwidth_hz = (uint64_t)fmax(200000, fmax(rx[0], tx[0]));
    caps->max_bandwidth_hz = (uint64_t)fmin(40000000, fmin(rx[2], tx[2]));
    RANGE(device->tx_sample, "hardwaregain_available", tx);
    RANGE(device->rx_sample, "hardwaregain_available", rx);
    if (tx[0] < -89.75 || tx[2] > 0 || tx[1] != 0.25 || rx[0] < -100 || rx[2] > 100 || rx[1] != 1) goto invalid;
    caps->min_tx_gain_mdb = (int32_t)llround(tx[0] * 1000);
    caps->max_tx_gain_mdb = (int32_t)llround(tx[2] * 1000);
    caps->tx_gain_step_mdb = 250;
    caps->min_rx_gain_mdb = (int32_t)llround(rx[0] * 1000);
    caps->max_rx_gain_mdb = (int32_t)llround(rx[2] * 1000);
    caps->rx_gain_step_mdb = 1000;
#undef RANGE
    if (caps->min_carrier_hz > caps->max_carrier_hz || caps->min_bandwidth_hz > caps->max_bandwidth_hz) goto invalid;
    return 0;
invalid:
    set_error(error, RTE_ERROR_RF_CONTEXT, "rf_capabilities", "driver ranges conflict with the AD9364 operating profile");
    return -EINVAL;
}

int rte_ad936x_capabilities(struct rte_ad936x *device, struct rte_rf_capabilities *caps, struct rte_error *error)
{
    struct rte_rf_context rf;
    int status;
    if (!caps) return -EINVAL;
    status = rte_ad936x_read_rf(device, &rf, error);
    return status ? status : capabilities_raw(device, caps, error);
}

static int validate_request(const struct rte_rf_context *rf, const struct rte_rf_capabilities *caps,
                            bool check_carrier, bool check_rx_gain, struct rte_error *error)
{
    const char *field = NULL;
    if (rf->sample_rate_hz != RTE_FIXED_SAMPLE_RATE_HZ) field = "sample_rate_hz";
    else if (!rf->manual_gain) field = "gain_control_mode";
    else if (rf->rx_lo_hz != rf->tx_lo_hz || (check_carrier &&
             (rf->rx_lo_hz < caps->min_carrier_hz || rf->rx_lo_hz > caps->max_carrier_hz))) field = "carrier_hz";
    else if (rf->rx_bandwidth_hz != rf->tx_bandwidth_hz || rf->rx_bandwidth_hz < caps->min_bandwidth_hz || rf->rx_bandwidth_hz > caps->max_bandwidth_hz) field = "bandwidth_hz";
    else if (rf->tx_gain_mdb < caps->min_tx_gain_mdb || rf->tx_gain_mdb > caps->max_tx_gain_mdb ||
             (rf->tx_gain_mdb - caps->min_tx_gain_mdb) % caps->tx_gain_step_mdb) field = "tx_gain_db";
    else if (check_rx_gain && (rf->rx_gain_mdb < caps->min_rx_gain_mdb || rf->rx_gain_mdb > caps->max_rx_gain_mdb ||
             (rf->rx_gain_mdb - caps->min_rx_gain_mdb) % caps->rx_gain_step_mdb)) field = "rx_gain_db";
    if (!field) return 0;
    set_error(error, RTE_ERROR_RANGE, field, "RF value is outside the supported range or step");
    return -ERANGE;
}

static bool rf_matches(const struct rte_rf_context *a, const struct rte_rf_context *b)
{
    return a->sample_rate_hz == b->sample_rate_hz && a->manual_gain == b->manual_gain &&
        rte_ad936x_lo_matches(a->rx_lo_hz, b->rx_lo_hz) &&
        rte_ad936x_lo_matches(a->tx_lo_hz, b->tx_lo_hz) &&
        a->rx_bandwidth_hz == b->rx_bandwidth_hz && a->tx_bandwidth_hz == b->tx_bandwidth_hz &&
        a->rx_gain_mdb == b->rx_gain_mdb && a->tx_gain_mdb == b->tx_gain_mdb;
}

static int write_context(struct rte_ad936x *device, const struct rte_rf_context *desired,
                         struct rte_rf_context *applied, struct rte_error *error)
{
    struct rte_rf_context current;
    struct rte_rf_capabilities caps;
    int status = read_raw(device, &current, error);
    if (status) return status;
    /* ad9361.c rounds the fractional PLL (modulus 8388593, reference <=
     * 80008000 Hz, VCO divider >= 2), and truncates through a half-rate clock
     * on both write and read. Together these cause at most 5 integer Hz error.
     * Avoid retuning an already matching LO, including nominal GUI echoes.
     * If either chain needs retuning, write BOTH with the same setpoint: a
     * rollback to a quantized prior readback may itself round by another Hz. */
    if (current.rx_lo_hz != current.tx_lo_hz ||
        !rte_ad936x_lo_matches(current.rx_lo_hz, desired->rx_lo_hz) ||
        !rte_ad936x_lo_matches(current.tx_lo_hz, desired->tx_lo_hz)) {
        status = write_hz(device->rx_lo, "frequency", desired->rx_lo_hz, "carrier_hz", error);
        if (status) return status;
        status = write_hz(device->tx_lo, "frequency", desired->tx_lo_hz, "carrier_hz", error);
        if (status) return status;
    }
#define WRITE_HZ_IF(ch, attr, member, field) do { if (current.member != desired->member) { \
    status = write_hz(ch, attr, desired->member, field, error); if (status) return status; } } while (0)
    /* LO retuning may change the gain table, limits and current gain. */
    status = capabilities_raw(device, &caps, error);
    if (status) return status;
    /* New setpoints were range-checked before writes. Restored/current
     * readbacks can be a few Hz outside a nominal endpoint after rounding. */
    status = validate_request(desired, &caps, false, true, error);
    if (status) return status;
    WRITE_HZ_IF(device->rx_sample, "rf_bandwidth", rx_bandwidth_hz, "bandwidth_hz");
    WRITE_HZ_IF(device->tx_sample, "rf_bandwidth", tx_bandwidth_hz, "bandwidth_hz");
#undef WRITE_HZ_IF
    status = read_gain(device->tx_sample, &current.tx_gain_mdb, "tx_gain_db", error);
    if (status) return status;
    status = read_gain(device->rx_sample, &current.rx_gain_mdb, "rx_gain_db", error);
    if (status) return status;
    if (current.tx_gain_mdb != desired->tx_gain_mdb) {
        status = write_gain(device->tx_sample, desired->tx_gain_mdb, "tx_gain_db", error);
        if (status) return status;
    }
    if (current.rx_gain_mdb != desired->rx_gain_mdb) {
        status = write_gain(device->rx_sample, desired->rx_gain_mdb, "rx_gain_db", error);
        if (status) return status;
    }
    status = rte_ad936x_read_rf(device, applied, error);
    if (!status && !rf_matches(applied, desired)) {
        if (!rte_ad936x_lo_matches(applied->rx_lo_hz, desired->rx_lo_hz) ||
            !rte_ad936x_lo_matches(applied->tx_lo_hz, desired->tx_lo_hz))
            set_error(error, RTE_ERROR_HARDWARE, "rf_readback",
                      "LO readback differs: requested %" PRIu64 " Hz, RX %" PRIu64
                      " Hz, TX %" PRIu64 " Hz (tolerance %" PRIu64 " Hz)",
                      desired->rx_lo_hz, applied->rx_lo_hz, applied->tx_lo_hz,
                      RTE_AD936X_LO_READBACK_TOLERANCE_HZ);
        else if (applied->rx_bandwidth_hz != desired->rx_bandwidth_hz ||
                 applied->tx_bandwidth_hz != desired->tx_bandwidth_hz)
            set_error(error, RTE_ERROR_HARDWARE, "rf_readback",
                      "bandwidth readback differs: requested %" PRIu64 " Hz, RX %" PRIu64 " Hz, TX %" PRIu64 " Hz",
                      desired->rx_bandwidth_hz, applied->rx_bandwidth_hz, applied->tx_bandwidth_hz);
        else
            set_error(error, RTE_ERROR_HARDWARE, "rf_readback",
                      "gain readback differs: requested TX %.3f / RX %.3f dB, read TX %.3f / RX %.3f dB",
                      desired->tx_gain_mdb / 1000.0, desired->rx_gain_mdb / 1000.0,
                      applied->tx_gain_mdb / 1000.0, applied->rx_gain_mdb / 1000.0);
        return -EIO;
    }
    return status;
}

int rte_ad936x_apply_rf(struct rte_ad936x *device, const struct rte_rf_context *requested,
                        struct rte_rf_context *applied, struct rte_error *error)
{
    struct rte_rf_context previous, restored;
    struct rte_rf_capabilities caps;
    struct rte_error local_error, original, rollback_error;
    int status, rollback_status;
    if (!error) error = &local_error;
    rte_error_clear(error);
    if (!device || !requested || !applied) return -EINVAL;
    status = rte_ad936x_read_rf(device, &previous, error);
    if (status) return status;
    *applied = previous;
    status = capabilities_raw(device, &caps, error);
    if (status) return status;
    status = validate_request(requested, &caps, requested->rx_lo_hz != previous.rx_lo_hz,
                              rte_ad936x_lo_matches(requested->rx_lo_hz, previous.rx_lo_hz), error);
    if (status) return status;
    status = write_context(device, requested, applied, error);
    if (!status) return 0;
    original = *error;
    rte_error_clear(&rollback_error);
    rollback_status = write_context(device, &previous, &restored, &rollback_error);
    if (rollback_status || !rf_matches(&previous, &restored)) {
        set_error(error, RTE_ERROR_HARDWARE, "rf_rollback", "RF failed (%.64s); restoration failed (%.64s)",
                  original.message, rollback_error.message);
        return -EIO;
    }
    *applied = restored;
    *error = original;
    return status;
}

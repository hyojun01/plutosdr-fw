/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/ad936x.h>

#include <errno.h>
#include <iio.h>
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

static const char *iio_error_string(int status, char *buffer,
                                    size_t buffer_size)
{
    if (status >= 0)
        return "success";
    iio_strerror(-status, buffer, buffer_size);
    return buffer;
}

static int read_attr(struct iio_channel *channel, const char *attribute,
                     uint64_t *value, const char *field,
                     struct rte_error *error)
{
    long long result;
    char buffer[96];
    int status;

    status = iio_channel_attr_read_longlong(channel, attribute, &result);
    if (status < 0) {
        set_error(error, RTE_ERROR_IO, field,
                  "cannot read AD936x attribute %s: %s", attribute,
                  iio_error_string(status, buffer, sizeof(buffer)));
        return status;
    }
    if (result <= 0) {
        set_error(error, RTE_ERROR_RF_CONTEXT, field,
                  "AD936x attribute %s returned invalid value %lld",
                  attribute, result);
        return -ERANGE;
    }
    *value = (uint64_t)result;
    return 0;
}

static int write_attr(struct iio_channel *channel, const char *attribute,
                      uint64_t value, const char *field,
                      struct rte_error *error)
{
    char buffer[96];
    int status;

    if (value == 0 || value > INT64_MAX) {
        set_error(error, RTE_ERROR_RANGE, field,
                  "AD936x attribute %s is outside the signed 64-bit range",
                  attribute);
        return -ERANGE;
    }
    status = (int)iio_channel_attr_write_longlong(
        channel, attribute, (long long)value);
    if (status < 0) {
        set_error(error, RTE_ERROR_IO, field,
                  "cannot write AD936x attribute %s: %s", attribute,
                  iio_error_string(status, buffer, sizeof(buffer)));
        return status;
    }
    return 0;
}

int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error)
{
    rte_error_clear(error);
    if (device == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "ad936x",
                  "AD936x device object is required");
        return -EINVAL;
    }

    memset(device, 0, sizeof(*device));
    device->context = iio_create_local_context();
    if (device->context == NULL) {
        set_error(error, RTE_ERROR_IO, "ad936x",
                  "cannot create local IIO context");
        return -ENODEV;
    }
    device->phy = iio_context_find_device(device->context, "ad9361-phy");
    if (device->phy == NULL) {
        set_error(error, RTE_ERROR_HARDWARE, "ad936x",
                  "IIO device ad9361-phy was not found");
        rte_ad936x_close(device);
        return -ENODEV;
    }

    device->rx_sample =
        iio_device_find_channel(device->phy, "voltage0", false);
    device->tx_sample =
        iio_device_find_channel(device->phy, "voltage0", true);
    device->rx_lo =
        iio_device_find_channel(device->phy, "altvoltage0", true);
    device->tx_lo =
        iio_device_find_channel(device->phy, "altvoltage1", true);
    if (device->rx_sample == NULL || device->tx_sample == NULL ||
        device->rx_lo == NULL || device->tx_lo == NULL) {
        set_error(error, RTE_ERROR_HARDWARE, "ad936x",
                  "required RX/TX sampling or LO IIO channels are missing");
        rte_ad936x_close(device);
        return -ENODEV;
    }
    return 0;
}

void rte_ad936x_close(struct rte_ad936x *device)
{
    if (device == NULL)
        return;
    if (device->context != NULL)
        iio_context_destroy(device->context);
    memset(device, 0, sizeof(*device));
}

int rte_ad936x_read_rf(struct rte_ad936x *device,
                       struct rte_rf_context *rf,
                       struct rte_error *error)
{
    uint64_t rx_sample_rate;
    uint64_t tx_sample_rate;
    int status;

    rte_error_clear(error);
    if (device == NULL || device->context == NULL || rf == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "ad936x",
                  "open AD936x device and RF output are required");
        return -EINVAL;
    }

    status = read_attr(device->rx_sample, "sampling_frequency",
                       &rx_sample_rate, "sample_rate_hz", error);
    if (status != 0)
        return status;
    status = read_attr(device->tx_sample, "sampling_frequency",
                       &tx_sample_rate, "sample_rate_hz", error);
    if (status != 0)
        return status;
    if (rx_sample_rate != tx_sample_rate) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "sample_rate_hz",
                  "RTE requires equal RX and TX sample rates "
                  "(RX=%llu, TX=%llu)",
                  (unsigned long long)rx_sample_rate,
                  (unsigned long long)tx_sample_rate);
        return -EINVAL;
    }
    rf->sample_rate_hz = rx_sample_rate;

    status = read_attr(device->rx_lo, "frequency", &rf->rx_lo_hz,
                       "carrier_hz", error);
    if (status != 0)
        return status;
    status = read_attr(device->tx_lo, "frequency", &rf->tx_lo_hz,
                       "carrier_hz", error);
    if (status != 0)
        return status;
    if (rf->rx_lo_hz != rf->tx_lo_hz) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "carrier_hz",
                  "monostatic RTE requires equal RX and TX LO "
                  "(RX=%llu, TX=%llu)",
                  (unsigned long long)rf->rx_lo_hz,
                  (unsigned long long)rf->tx_lo_hz);
        return -EINVAL;
    }
    return 0;
}

static int write_context(struct rte_ad936x *device,
                         const struct rte_rf_context *rf,
                         struct rte_error *error)
{
    int status;

    if (rf->rx_lo_hz != rf->tx_lo_hz) {
        set_error(error, RTE_ERROR_RF_CONTEXT, "carrier_hz",
                  "requested RX and TX LO must be identical");
        return -EINVAL;
    }

    status = write_attr(device->rx_sample, "sampling_frequency",
                        rf->sample_rate_hz, "sample_rate_hz", error);
    if (status != 0)
        return status;
    status = write_attr(device->tx_sample, "sampling_frequency",
                        rf->sample_rate_hz, "sample_rate_hz", error);
    if (status != 0)
        return status;
    status = write_attr(device->rx_lo, "frequency", rf->rx_lo_hz,
                        "carrier_hz", error);
    if (status != 0)
        return status;
    return write_attr(device->tx_lo, "frequency", rf->tx_lo_hz,
                      "carrier_hz", error);
}

int rte_ad936x_apply_rf(struct rte_ad936x *device,
                        const struct rte_rf_context *requested,
                        struct rte_rf_context *applied,
                        struct rte_error *error)
{
    struct rte_rf_context previous;
    struct rte_rf_context restored;
    struct rte_error original_error;
    struct rte_error rollback_error;
    int rollback_status;
    int status;

    rte_error_clear(error);
    if (device == NULL || requested == NULL || applied == NULL) {
        set_error(error, RTE_ERROR_ARGUMENT, "ad936x",
                  "AD936x device, requested RF, and applied RF are required");
        return -EINVAL;
    }
    status = rte_ad936x_read_rf(device, &previous, error);
    if (status != 0)
        return status;

    status = write_context(device, requested, error);
    if (status == 0)
        status = rte_ad936x_read_rf(device, applied, error);
    if (status == 0)
        return 0;

    /*
     * IIO exposes independent attributes, so a failed multi-attribute write
     * can be partial. Best-effort restoration keeps RF and the last RTE
     * register image coherent for the controller's higher-level rollback.
     */
    original_error = *error;
    rte_error_clear(&rollback_error);
    rollback_status = write_context(device, &previous, &rollback_error);
    if (rollback_status == 0)
        rollback_status = rte_ad936x_read_rf(
            device, &restored, &rollback_error);
    if (rollback_status == 0 &&
        (restored.sample_rate_hz != previous.sample_rate_hz ||
         restored.rx_lo_hz != previous.rx_lo_hz ||
         restored.tx_lo_hz != previous.tx_lo_hz)) {
        set_error(&rollback_error, RTE_ERROR_HARDWARE, "rf_rollback",
                  "AD936x did not restore the previous RF values");
        rollback_status = -EIO;
    }
    if (rollback_status != 0) {
        set_error(error, RTE_ERROR_HARDWARE, "rf_rollback",
                  "%s; RF rollback also failed: %s",
                  original_error.message, rollback_error.message);
        return -EIO;
    }
    *error = original_error;
    return status;
}

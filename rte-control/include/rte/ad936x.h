/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_AD936X_H
#define RTE_AD936X_H

#include <rte/model.h>

struct iio_context;
struct iio_device;
struct iio_channel;

struct rte_ad936x {
    struct iio_context *context;
    struct iio_device *phy;
    struct iio_channel *rx_sample;
    struct iio_channel *tx_sample;
    struct iio_channel *rx_lo;
    struct iio_channel *tx_lo;
};

int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error);
void rte_ad936x_close(struct rte_ad936x *device);

int rte_ad936x_read_rf(struct rte_ad936x *device,
                       struct rte_rf_context *rf,
                       struct rte_error *error);

int rte_ad936x_apply_rf(struct rte_ad936x *device,
                        const struct rte_rf_context *requested,
                        struct rte_rf_context *applied,
                        struct rte_error *error);

#endif

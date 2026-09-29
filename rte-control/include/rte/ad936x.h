/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RTE_AD936X_H
#define RTE_AD936X_H

#include <rte/model.h>

/* This firmware's AD9361 RFPLL + half-rate clock readback can differ by 5 Hz.
 * This is a digital readback bound, not an RF oscillator accuracy claim. */
#define RTE_AD936X_LO_READBACK_TOLERANCE_HZ UINT64_C(5)

static inline bool rte_ad936x_lo_matches(uint64_t requested, uint64_t actual)
{
    return (requested > actual ? requested - actual : actual - requested) <=
        RTE_AD936X_LO_READBACK_TOLERANCE_HZ;
}

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
    char compatible[32];
};

struct rte_rf_capabilities {
    uint64_t min_carrier_hz, max_carrier_hz, min_bandwidth_hz, max_bandwidth_hz;
    int32_t min_tx_gain_mdb, max_tx_gain_mdb, tx_gain_step_mdb;
    int32_t min_rx_gain_mdb, max_rx_gain_mdb, rx_gain_step_mdb;
};

int rte_ad936x_capabilities(struct rte_ad936x *device,
                            struct rte_rf_capabilities *caps,
                            struct rte_error *error);

int rte_ad936x_open(struct rte_ad936x *device, struct rte_error *error);
void rte_ad936x_close(struct rte_ad936x *device);

int rte_ad936x_read_rf(struct rte_ad936x *device,
                       struct rte_rf_context *rf,
                       struct rte_error *error);

/* On success or verified rollback, applied contains actual readback. Before
 * writes it may contain the initial readback; on rf_rollback it is not valid. */
int rte_ad936x_apply_rf(struct rte_ad936x *device,
                        const struct rte_rf_context *requested,
                        struct rte_rf_context *applied,
                        struct rte_error *error);

#endif

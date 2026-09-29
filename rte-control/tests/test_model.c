/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/model.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static struct rte_rf_context rf = {.sample_rate_hz=61440000, .rx_lo_hz=2400000000, .tx_lo_hz=2400000000};
static struct rte_capabilities caps;
static struct rte_config cfg;
static struct rte_register_image image;
static struct rte_applied applied;
static struct rte_error error;
static void encode(void) {
    assert(rte_encode(&cfg,&rf,&image,&applied,&error)==0);
    assert(image.words[RTE_PARAM_FD] <= 63); /* API fractions are always canonical. */
}
static void bad(const char *field) {
    assert(rte_encode(&cfg,&rf,&image,&applied,&error)!=0);
    assert(strcmp(error.field,field)==0);
}
int main(void)
{
    uint32_t p;
    assert(rte_capabilities_for_rf(&rf,&caps,&error)==0);
    assert(!caps.rf_calibrated);
    assert(caps.fixed_latency_samples==18.0);
    assert(fabs(caps.range_resolution_m-0.038120582326253254)<1e-14);
    assert(caps.max_gain_linear==32.0-1.0/1048576.0);
    cfg=(struct rte_config){.enabled=true,.range_m=caps.min_range_m,.gain_linear=0.5};
    encode();
    assert(image.words[0]==0 && image.words[1]==0 && image.words[2]==0);
    assert(image.words[4]==524288 && image.words[5]==1);
    assert(applied.delay_samples==caps.fixed_latency_samples && applied.linear_gain==0.5);
    cfg.range_m=caps.max_range_m; encode();
    assert(image.words[0]==1023 && image.words[1]==63);
    cfg.range_m=nextafter(caps.max_range_m,INFINITY); bad("range_m");
    cfg.range_m=nextafter(caps.min_range_m,-INFINITY); bad("range_m");
    cfg.range_m=caps.min_range_m+63.49*caps.range_resolution_m; encode();
    assert(image.words[0]==0 && image.words[1]==63);
    cfg.range_m=caps.min_range_m+63.51*caps.range_resolution_m; encode();
    assert(image.words[0]==1 && image.words[1]==0);
    cfg.range_m=caps.min_range_m+64.49*caps.range_resolution_m; encode();
    assert(image.words[0]==1 && image.words[1]==0);
    cfg.range_m=caps.min_range_m+64.51*caps.range_resolution_m; encode();
    assert(image.words[0]==1 && image.words[1]==1);
    cfg.range_m=150.6; cfg.radial_velocity_mps=10; encode();
    assert(image.words[2]==UINT32_C(0xffffd447)); /* -11193; -160.11077 Hz */
    assert(applied.doppler_hz<0 && fabs(applied.radial_velocity_mps-10)<0.001);
    cfg.radial_velocity_mps=-10; encode(); assert(image.words[2]==11193);
    cfg.radial_velocity_mps=0; encode(); assert(image.words[2]==0);
    cfg.range_m=250; encode();
    assert(image.words[3]==UINT32_C(0x3b197c31)); /* Exact rational distance phase at 2.4 GHz. */
    p=image.words[3]; cfg.phase_offset_deg=90; encode();
    assert(image.words[3]-p==UINT32_C(0x40000000));
    cfg.phase_offset_deg=450; encode(); assert(image.words[3]-p==UINT32_C(0x40000000));
    cfg.phase_offset_deg=-270; encode(); assert(image.words[3]-p==UINT32_C(0x40000000));
    cfg.gain_linear=caps.max_gain_linear; encode(); assert(image.words[4]==0x1ffffff);
    cfg.gain_linear=nextafter(caps.max_gain_linear,INFINITY); bad("gain_linear");
    cfg.gain_linear=-0.01; bad("gain_linear");
    cfg.gain_linear=0; cfg.enabled=false; encode();
    assert(image.words[4]==0 && image.words[5]==0 && !applied.enabled);
    cfg.radial_velocity_mps=RTE_SPEED_OF_LIGHT_MPS*(61440000.0/2)/(2*2400000000.0); bad("radial_velocity_mps");
    cfg.radial_velocity_mps=-cfg.radial_velocity_mps; bad("radial_velocity_mps");
    cfg.radial_velocity_mps=nextafter(cfg.radial_velocity_mps,0.0);
    bad("radial_velocity_mps"); /* Positive PINC rounds above INT32_MAX near Nyquist. */
    cfg.radial_velocity_mps=0;
    cfg.gain_linear=0.5/1048576.0; encode(); assert(image.words[4]==1);
    cfg.phase_offset_deg=NAN; bad("phase_offset_deg"); cfg.phase_offset_deg=0;
    cfg.range_m=INFINITY; bad("range_m"); cfg.range_m=150.6;
    cfg.gain_linear=NAN; bad("gain_linear"); cfg.gain_linear=1;
    cfg.radial_velocity_mps=INFINITY; bad("radial_velocity_mps"); cfg.radial_velocity_mps=0;
    rf.sample_rate_hz=30720000; bad("sample_rate_hz"); rf.sample_rate_hz=61440000;
    rf.tx_lo_hz++; bad("carrier_hz"); rf.tx_lo_hz=rf.rx_lo_hz;
    assert(rte_encode(NULL,&rf,&image,&applied,NULL)!=0);
    assert(rte_capabilities_for_rf(&rf,NULL,NULL)!=0);
    puts("model tests passed");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/mmio.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream, const char *name)
{
    fprintf(stream,
        "Usage:\n  %s [--device PATH] info\n"
        "  %s [--device PATH] readback [TARGET]\n"
        "  %s encode TARGET CARRIER_HZ RANGE_M VELOCITY_MPS GAIN_LINEAR PHASE_DEG ENABLED\n"
        "TARGET: 1..4; ENABLED: 0 or 1; sample rate: fixed 61440000 Hz.\n"
        "Range reference: digital DUT ports, 18 sample fixed latency; RF uncalibrated.\n"
        "Readback reports staged words only, not independent active-bank state.\n"
        "Stop rte-httpd before opening its exclusively owned device.\n", name, name, name);
}
static int parse_u64(const char *text, uint64_t *out)
{
    char *end;
    unsigned long long value;
    if (text == NULL || *text == '\0') return -EINVAL;
    for (const char *c=text; *c; ++c) if (*c<'0' || *c>'9') return -EINVAL;
    errno=0; value=strtoull(text,&end,10);
    if (errno || *end) return -EINVAL;
    *out=(uint64_t)value; return 0;
}
static int parse_double(const char *text, double *out)
{
    char *end;
    errno=0; *out=strtod(text,&end);
    return errno || end==text || *end || !isfinite(*out) ? -EINVAL : 0;
}
static int parse_target(const char *text, unsigned int *target)
{
    uint64_t value;
    if (parse_u64(text,&value) || value<1 || value>RTE_TARGET_COUNT) return -EINVAL;
    *target=(unsigned int)value-1; return 0;
}
static void print_error(const struct rte_error *error)
{
    fprintf(stderr,"%s (%s): %s\n",rte_error_code_name(error->code),error->field,error->message);
}
static void print_image(unsigned int target,const struct rte_register_image *image)
{
    for (unsigned int j=0; j<RTE_PARAMETER_REGISTER_COUNT; ++j) {
        const struct rte_register_descriptor *r=&rte_target_registers[target][j];
        printf("target.%u.%-4s offset=0x%03" PRIx32 " value=0x%08" PRIx32 "\n",
               target+1,r->name,r->offset,image->words[j]);
    }
}
static int info(struct rte_mmio *mmio)
{
    uint32_t timestamp, enable, lp;
    if (mmio->io.read32(mmio->io.context,RTE_REG_IPCORE_TIMESTAMP,&timestamp) ||
        mmio->io.read32(mmio->io.context,RTE_REG_IPCORE_ENABLE,&enable)) return EXIT_FAILURE;
    printf("device.physical_base=0x%08" PRIxPTR "\ndevice.register_size=0x%zx\n",mmio->physical_base,mmio->size);
    printf("hardware.profile=multitarget-v2\nhardware.timestamp=%" PRIu32 "\n",timestamp);
    printf("hardware.timestamp_expected=%" PRIu32 "\nhardware.enabled=%s\n",RTE_EXPECTED_TIMESTAMP,(enable&1)?"true":"false");
    for (unsigned int t=0;t<RTE_TARGET_COUNT;++t) {
        if (mmio->io.read32(mmio->io.context,rte_target_registers[t][RTE_PARAM_LP].offset,&lp)) return EXIT_FAILURE;
        printf("target.%u.LP=%u\n",t+1,lp&1);
    }
    return timestamp==RTE_EXPECTED_TIMESTAMP ? EXIT_SUCCESS : EXIT_FAILURE;
}
int main(int argc,char **argv)
{
    const char *device="/dev/mwipcore0",*command;
    struct rte_register_image image;
    struct rte_rf_context rf={.sample_rate_hz=RTE_FIXED_SAMPLE_RATE_HZ};
    struct rte_config config={0};
    struct rte_applied applied;
    struct rte_error error;
    struct rte_mmio mmio;
    unsigned int target=0, first=0, count=RTE_TARGET_COUNT;
    int arg=1,status;
    uint64_t enabled;
    if (argc>2 && strcmp(argv[1],"--device")==0) {device=argv[2];arg=3;}
    if (arg>=argc) {usage(stderr,argv[0]);return EXIT_FAILURE;}
    command=argv[arg++];
    if (strcmp(command,"encode")==0) {
        if (argc-arg!=7 || parse_target(argv[arg],&target) ||
            parse_u64(argv[arg+1],&rf.rx_lo_hz) || parse_double(argv[arg+2],&config.range_m) ||
            parse_double(argv[arg+3],&config.radial_velocity_mps) || parse_double(argv[arg+4],&config.gain_linear) ||
            parse_double(argv[arg+5],&config.phase_offset_deg) || parse_u64(argv[arg+6],&enabled) || enabled>1) {
            usage(stderr,argv[0]);return EXIT_FAILURE;
        }
        rf.tx_lo_hz=rf.rx_lo_hz; config.enabled=enabled!=0;
        if (rte_encode(&config,&rf,&image,&applied,&error)) {print_error(&error);return EXIT_FAILURE;}
        print_image(target,&image);
        printf("applied.delay_samples=%.12g\napplied.delay_range_m=%.12g\napplied.range_phase_rad=%.12g\n",
               applied.delay_samples,applied.delay_range_m,applied.range_phase_rad);
        printf("applied.doppler_hz=%.12g\napplied.radial_velocity_mps=%.12g\napplied.linear_gain=%.12g\n",
               applied.doppler_hz,applied.radial_velocity_mps,applied.linear_gain);
        printf("applied.enabled=%s\nrf_calibrated=false\n",applied.enabled?"true":"false");
        return EXIT_SUCCESS;
    }
    if (strcmp(command,"readback")==0) {
        if (argc-arg>1 || (argc-arg==1 && parse_target(argv[arg],&first))) {usage(stderr,argv[0]);return EXIT_FAILURE;}
        if (argc-arg==1) count=1;
    } else if (strcmp(command,"info")!=0 || argc!=arg) {usage(stderr,argv[0]);return EXIT_FAILURE;}
    if (rte_mmio_open(&mmio,device,&error)) {print_error(&error);return EXIT_FAILURE;}
    if (strcmp(command,"info")==0) {status=info(&mmio);rte_mmio_close(&mmio);return status;}
    status=rte_register_validate_hardware(&mmio.io,RTE_EXPECTED_TIMESTAMP,&error);
    for (unsigned int t=first;!status && t<first+count;++t) {
        status=rte_register_read_image(&mmio.io,t,&image,&error);
        if (!status) print_image(t,&image);
    }
    if (status) print_error(&error);
    rte_mmio_close(&mmio);
    return status ? EXIT_FAILURE : EXIT_SUCCESS;
}

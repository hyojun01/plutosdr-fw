# Generated RTL profile verification

The [firmware profile](../../src/model.c) for timestamp **2609182026** uses **18 fixed samples**
between `HDL_DUT_ip_src_HDL_DUT.Rx_Data_{I,Q}_In` and
`Tx_Data_{I,Q}_Out`, with `clk_enable=1`, continuous valid input and settled
parameters. Input and output are observed in the same clock interval before
the next rising edge: one register contributes one sample of delay.
This excludes the custom reference design FIFOs, external wrappers, AD936x
filters and RF propagation. `rf_calibrated=false` remains mandatory.

Run from anywhere, with Vivado 2023.2 installed:

```sh
sh plutosdr-fw/rte-control/tests/rtl/run.sh
# Optional: VIVADO_BIN=/path/to/Vivado/bin RTE_RTL_BUILD_DIR=/tmp/rte-rtl ...
```

`run.sh` compiles the production generated RTL without editing it, captures
12 impulse cases, checks the [source SHA-256 manifest](rtl.sha256) and validates the result.
[check_abi.py](check_abi.py) independently compares all 28 firmware offsets
and masks with the generated address header and RTL write decoder, and checks
the timestamp against the top-level RTL constant.
The tool needs the local IPC access used by Vivado's simulator kernel. The
2026-09-28 run succeeded using Vivado 2023.2 with that access. The generated
RTL and address-header hashes are recorded in `rtl.sha256`; regenerate and
review the profile if any source changes. Simulation artifacts are retained
under the printed temporary build directory.

| Target(s) | ID | FD raw | SC raw | Peak delay | Peak I | DC group delay (samples) |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 0 | 65536 | 18 | 16400 | 17.995098 |
| 1 | 16 | 0 | 65536 | 34 | 16400 | 33.995098 |
| 1 | 1023 | 0 | 65536 | 1041 | 16400 | 1040.995098 |
| 1 | 0 | 32 | 65536 | 18 | 10080 | 18.500000 |
| 1 | 0 | 63 | 65536 | 19 | 16336 | 18.989194 |
| 2, 3, 4 individually | 0 | 0 | 65536 | 18 | 16400 | 17.995098 |
| All 4, lower gain | 0 | 0 | 16384 | 18 | 16384 | 17.992126 |
| 1, half gain | 0 | 0 | 32768 | 18 | 8192 | 17.994106 |
| All disabled | 0 | 0 | 65536 | none | 0 | n/a |

The input impulse is I=16384, Q=0. DC group delay is the signed impulse
first moment divided by its sum (the transfer-function phase derivative at
zero frequency). Fixed-point truncation, the finite Farrow approximation and
NCO dither explain the sub-sample residuals and small Q residues. These
checks bound fractional-delay error to less than 1/64 sample for the tested
cases; they do not promise frequency-independent group delay throughout the
RF passband.

`SC=65536` means a digital multiplier of 1/16, not unity. The DUT's final
left shift by four makes the measured single-target output approximately
unity in this test. Four such coherent targets produce wraparound: at sample
18, `4*16400` becomes 64 after the 16-bit output truncation. This deliberate
overflow case is also checked. The firmware reports the SC coefficient as
`linear_gain`; it does not infer clipping or RF gain from it.

The nominal representable digital distance is **43.914910840 m** through
**2542.147273591 m** at 61.44 MS/s, with 0.038120582326 m spacing. The exact
capabilities values are calculated in `model.c`. Actual RF-port distances
require separate loopback calibration, including bandwidth-dependent delay.

The FD register's hardware mask remains `0xff` (8-bit ufix8_En6).
The physical-parameter encoder always emits normalized `FD=0..63`; full-width
staged readback is retained so external changes are detected rather than
hidden by a narrower software mask. Core tests cover both contracts.

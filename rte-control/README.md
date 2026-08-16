# RTE control daemon

`rte-httpd` is the single owner of the Pluto AD936x RF configuration and the
RTE DUT AXI4-Lite register bank. It exposes physical radar parameters instead
of writable raw registers. The design follows the useful control-plane ideas
from Maia SDR: one hardware-owning service, a versioned HTTP API, typed state,
and explicit requested-versus-applied readback.

The first implementation is C11 because the existing Pluto firmware already
provides a C `libiio` stack and Buildroot 2023.02 only contains Rust 1.67. The
service uses Mongoose 7.8, which is already packaged by this firmware tree.

## Hardware contract

- monostatic radar: RX LO equals TX LO
- Doppler convention: `f_d = -2 v f_c / c`
- positive attenuation means loss: `gain = 10^(-loss_db/20)`
- micro-motion:
  `A_r sin(2 pi f_r t + phi_r) + A_h sin(2 pi f_h t + phi_h)`
- the RTE clock is the actual common RX/TX sampling frequency
- supported FPGA timestamp: `2608142020` (`0x9b7516c4`)
- register resource: physical `0x43c00000`, size `0x10000`
- character device: `/dev/mwipcore0`

Normal parameter updates never write `IPCore_Reset` or `IPCore_Enable`.
All ten staged parameters are written and verified with `loadParam=0`, then
committed with a bounded `loadParam=1 -> 0` sequence. NCO accumulators therefore
retain their phase; changing a user phase offset still produces the requested
offset step.

## API

The defensive daemon default is `http://127.0.0.1:8080`. The RTE firmware init
script overrides it with the Pluto USB Ethernet `ipaddr` (normally
`192.168.2.1`) and never binds Ethernet or WLAN implicitly. There is no
authentication or TLS in this version, so do not expose the port to an
untrusted network.

The same listener serves the browser GUI at `/` from
`/usr/share/rte-control/www`. Keeping the GUI and `/api/v1` on one origin
avoids CORS and makes the Pluto USB Ethernet address the only URL users need.
API and health routes are matched before static files; only `GET` and `HEAD`
are accepted for GUI assets. Static responses include a restrictive content
security policy and other browser hardening headers, and missing assets return
HTTP 404. Use `--web-root PATH` (or `RTE_HTTPD_WEB_ROOT` in
`/etc/default/rte-httpd`) to serve a different directory.

Read endpoints:

- `GET /healthz`
- `GET /api/v1`
- `GET /api/v1/system`
- `GET /api/v1/rf/config`
- `GET /api/v1/rte/config`
- `GET /api/v1/rte/status`
- `GET /api/v1/rte/capabilities`

Write endpoints:

- `PATCH /api/v1/rf/config`
- `PATCH /api/v1/rte/config`
- `PATCH /api/v1/config` for one combined RF/RTE transaction

The first RTE request after boot must provide every semantic field. Later
requests may be partial patches. An RF change before that first complete RTE
transaction is rejected because the daemon cannot safely reinterpret an
unknown active register image. Unknown or duplicate JSON fields are rejected,
and raw register writes are deliberately absent.

Example initial transaction:

```sh
curl -sS -X PATCH http://192.168.2.1:8080/api/v1/config \
  -H 'Content-Type: application/json' \
  -d '{
    "rf": {
      "sample_rate_hz": 61440000,
      "carrier_hz": 2450000000
    },
    "rte": {
      "range_m": 2.0,
      "radial_velocity_mps": -0.15,
      "loss_db": 12.0,
      "respiration": {
        "amplitude_m": 0.005,
        "frequency_hz": 0.25,
        "phase_rad": 0.5235987755982988
      },
      "heartbeat": {
        "amplitude_m": 0.0005,
        "frequency_hz": 1.2,
        "phase_rad": 1.5707963267948966
      }
    }
  }'
```

Every successful state response contains:

- `requested`: the user's physical values
- `applied`: values after fixed-point/NCO quantization
- `applied.registers`: read-only diagnostic words
- `rf`: actual AD936x attribute readback
- `hardware_state_known`: false before the first complete transaction or
  after an unrecoverable rollback until restart
- `revision` and an equivalent HTTP `ETag`

For optimistic concurrency, copy the returned ETag into `If-Match`:

```sh
curl -sS -X PATCH http://192.168.2.1:8080/api/v1/rte/config \
  -H 'Content-Type: application/json' \
  -H 'If-Match: "1"' \
  -d '{"loss_db":18.0}'
```

The complete contract is in [OpenAPI](docs/openapi.yaml).

## Transaction and persistence behavior

One process-wide mutex serializes AD936x and RTE operations. A combined update
is handled as follows:

1. merge the semantic patch and preflight it against the requested RF values;
2. update both AD936x sample-rate channels and both LO channels;
3. read the actual RF values back;
4. re-encode all RTE words using that actual RF context;
5. stage, verify, and commit a complete RTE register image;
6. publish one new revision and schedule the semantic RTE configuration for
   coalesced persistence.

Partial AD936x writes and uncertain RTE commits have best-effort rollback. A
rollback failure is surfaced as a hardware error, marks
`hardware_state_known=false`, and blocks later patches rather than reporting a
stale state as successful.

The requested RTE configuration is atomically stored at
`/mnt/jffs2/rte/last-config.json` with `fsync`, `rename`, and directory `fsync`.
Successful GUI-style bursts are coalesced for two seconds to avoid one JFFS2
erase/write cycle per slider event; shutdown forces the latest pending write.
At startup it is re-encoded using the AD936x values actually present after
boot. Persistence failure does not undo an already-applied hardware update; it
is reported in the response's `warning` field. That field also reports a
pending coalesced write or disabled persistence. If `/mnt/jffs2` is not
actually mounted as JFFS2, persistence is disabled instead of writing a
temporary initramfs directory. The daemon never formats JFFS2.

## Build and tests

Host conversion, register-transaction, and JSON tests:

```sh
make -C rte-control clean
make -C rte-control test
make -C rte-control test-http \
  MONGOOSE_SOURCE=../buildroot/output/build/mongoose-7.8/mongoose.c \
  MONGOOSE_CFLAGS=-I../buildroot/output/build/mongoose-7.8 \
  MONGOOSE_LIBS=
make -C rte-control test-ui
```

`test-ui` uses host Node.js for JavaScript syntax checking and Python 3's
standard library to verify same-origin assets and API request literals.

Firmware-profile cross-build (from `plutosdr-fw`):

```sh
make build/rootfs.cpio.gz
```

Complete FIT and mass-storage update image using an already validated HDL XSA:

```sh
make XSA_FILE="$PWD/hdl/projects/pluto/pluto.sdk/system_top.xsa" \
  build/pluto.frm
```

This produces `build/pluto.itb` and `build/pluto.frm`. An explicit `XSA_FILE`
takes priority over an installed Vivado toolchain, and the bitstream is
extracted directly from the XSA for headless firmware packaging.

Useful offline encoder golden vector:

```sh
rte-control/build/rte-regtool encode \
  61440000 2450000000 2 -0.15 12 \
  0.005 0.25 0.5235987755982988 \
  0.0005 1.2 1.5707963267948966
```

Expected principal words are `delayOffset=0x00000001`,
`dopplerPINC=0x000000ab`, `dopplerPhOffs=0x4f8b4240`,
`scaling=0x00002027`, `microGainRes=0xfffff58a`, and
`microGainHeart=0xfffffef4`.

## Board checks

After booting a newly built FIT image:

```sh
dmesg | grep -E 'mwipcore|MathWorks'
ls -l /dev/mwipcore0
pgrep iiod || true
test ! -e /sys/kernel/config/usb_gadget/composite_gadget/functions/ffs.iio_ffs
wget -qO- http://192.168.2.1:8080/healthz
/etc/init.d/S60rte-httpd stop
rte-regtool info
/etc/init.d/S60rte-httpd start
```

The service must be stopped before `rte-regtool` because the AXI4-Lite driver
enforces exclusive open. Expected results are a root-only `/dev/mwipcore0`, no
`iiod` or IIO
FunctionFS USB function, preserved USB Ethernet/ACM/mass-storage functions,
and a healthy `rte-httpd`. DMA-backed libiio IQ streaming is intentionally
disabled in this RTE firmware profile.

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../../.." && pwd)
vivado_bin=${VIVADO_BIN:-/opt/Xilinx/Vivado/2023.2/bin}
build=${RTE_RTL_BUILD_DIR:-$(mktemp -d /tmp/rte-rtl.XXXXXX)}
mkdir -p "$build"
cd "$build"
"$vivado_bin/xvlog" "$root"/hdl_prj/ipcore/HDL_DUT_ip_v1_0/hdl/HDL_DUT_ip_src_*.v "$here/tb_latency.v"
"$vivado_bin/xelab" tb_latency -s rte_latency
"$vivado_bin/xsim" rte_latency -runall
python3 "$here/check_latency.py" "$build/impulse.csv"
printf 'RTL simulation artifacts: %s\n' "$build"

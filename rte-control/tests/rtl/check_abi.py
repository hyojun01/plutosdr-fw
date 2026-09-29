#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare firmware descriptors against generated header, decoder and timestamp."""
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[4]
APP = ROOT / 'plutosdr-fw/rte-control'
IP = ROOT / 'hdl_prj/ipcore/HDL_DUT_ip_v1_0'

def check():
    abi = (APP / 'include/rte/abi.h').read_text()
    model = (APP / 'src/model.c').read_text()
    header = (IP / 'include/HDL_DUT_ip_addr.h').read_text()
    decoder = (IP / 'hdl/HDL_DUT_ip_addr_decoder_write.v').read_text()
    top = (IP / 'hdl/HDL_DUT_ip.v').read_text()
    entries = re.findall(r'\{"(ID|FD|FRQ|PHOF|SC|EN|LP)",\s*(0x[0-9a-f]+),\s*(RTE_MASK_\w+)\}', model)
    assert len(entries) == 28
    header_map = dict(re.findall(r'#define\s+(\w+)_Data_HDL_DUT_ip\s+(0x[0-9a-fA-F]+)', header))
    offsets = dict(re.findall(r'assign decode_sel_(\w+)_1_1 = addr_write == 14\x27b([01]+);', decoder))
    masks = {name: int(value, 0) for name, value in re.findall(r'#define\s+(RTE_MASK_\w+)\s+UINT32_C\(([^)]+)\)', abi)}
    for index, (name, offset, mask) in enumerate(entries):
        port = name + str(index // 7 + 1)
        assert int(offset, 0) == int(header_map[port], 0) == int(offsets[port], 2) * 4, port
        bits = re.search(r'assign data_in_' + port + r' = data_write(?:\[(\d+)(?::0)?\])?;', decoder)
        assert bits, port
        width = int(bits[1]) + 1 if bits[1] is not None else 32
        assert masks[mask] == (1 << width) - 1, port
    timestamp = int(re.search(r'RTE_EXPECTED_TIMESTAMP UINT32_C\((\d+)\)', abi)[1])
    assert timestamp == int(re.search(r'assign ip_timestamp = 32\x27b([01]+)', top)[1], 2)
    assert f': {timestamp}' in header
    assert masks['RTE_MASK_FRACTION'] == 0xff  # Full ufix8_En6 readback, unlike API canonical fraction.
    print('PASS: all 28 offsets/masks and timestamp match generated RTL/header (including SC3/SC4).')

if __name__ == '__main__':
    check()

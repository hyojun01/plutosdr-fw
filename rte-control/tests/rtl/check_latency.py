#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check an xsim impulse.csv capture against the timestamp-specific profile."""
import argparse
import collections
import csv
import hashlib
from pathlib import Path
from check_abi import check as check_abi

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]

def check(capture):
    check_abi()
    for line in (HERE / 'rtl.sha256').read_text().splitlines():
        expected, relative = line.split('  ', 1)
        assert hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == expected, relative
    groups = collections.defaultdict(list)
    with Path(capture).open() as stream:
        for row in csv.DictReader(stream):
            groups[int(row['case'])].append({key: int(value) for key, value in row.items()})
    assert set(groups) == set(range(12)), 'missing simulation cases'
    for case, rows in groups.items():
        assert [r['n'] for r in rows] == list(range(1150))
        peak = max(rows, key=lambda r: abs(r['i']))
        integral = sum(r['i'] for r in rows)
        centroid = sum(r['n'] * r['i'] for r in rows) / integral if integral else None
        if case in (0, 1, 2, 5, 6, 7):
            assert peak['n'] == 18 + rows[0]['id']
            assert abs(centroid - (18 + rows[0]['id'])) < 1 / 64
            assert abs(peak['i'] - 16384) <= 32
        if case in (3, 4):
            ideal = 18 + rows[0]['fd'] / 64
            assert abs(centroid - ideal) < 1 / 64
        if case == 8:
            # Four coherent unity output paths exceed 16-bit range and wrap.
            assert groups[0][18]['i'] * 4 > 32767
            assert rows[18]['i'] == (groups[0][18]['i'] * 4 + 32768) % 65536 - 32768
        if case == 9:
            assert peak['n'] == 18 and abs(peak['i'] - 16384) <= 64
        if case == 10:
            assert peak['n'] == 18 and abs(peak['i'] - 8192) <= 32
        if case == 11:
            assert all(r['i'] == 0 and r['q'] == 0 for r in rows)
        print(f"case={case:2d} target={rows[0]['target']} ID={rows[0]['id']:4d} FD={rows[0]['fd']:2d} "
              f"SC={rows[0]['scale']:5d} peak_n={peak['n']:4d} peak_i={peak['i']:6d} centroid={centroid}")
    print('PASS: fixed digital DUT latency = 18 samples; RF path is not calibrated.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    check(parser.parse_args().capture)

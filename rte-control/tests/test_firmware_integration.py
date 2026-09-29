#!/usr/bin/env python3
"""Exercise firmware XSA recipes in an isolated tree without Vivado or hardware."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile

FIRMWARE = Path(__file__).resolve().parents[2]

class FirmwareXsaTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rte-xsa-")
        self.root = Path(self.tmp.name)
        (self.root / "scripts").mkdir()
        shutil.copy2(FIRMWARE / "Makefile", self.root / "Makefile")
        shutil.copy2(FIRMWARE / "scripts/pluto.mk", self.root / "scripts/pluto.mk")
        self.xsa = self.root / "input.xsa"

    def tearDown(self):
        self.tmp.cleanup()

    def write_xsa(self, content):
        with zipfile.ZipFile(self.xsa, "w") as archive:
            archive.writestr("system_top.bit", content)

    def make(self, *args):
        return subprocess.run(["make", "--no-print-directory", "HAVE_VIVADO=0",
            "LATEST_TAG=test", "VERSION=test", "UBOOT_VERSION=test", *args],
            cwd=self.root, text=True, capture_output=True)

    def test_degraded_http_server_remains_available_for_diagnostics(self):
        service = (FIRMWARE / "buildroot/package/rte-control/S60rte-httpd").read_text()
        readiness = re.search(r"^daemon_is_ready\(\) \{\n.*?^\}", service, re.M | re.S)
        self.assertIsNotNone(readiness)
        script = r'''RTE_HTTPD_LISTEN_URL=http://192.168.2.1:8080
        daemon_process_is_running() { return 0; }
        wget() {
            case "$*" in
                */api/v1/system) printf '%s\n' '{"hardware_profile":"multitarget-v2","degraded":true}' ;;
                *) return 1 ;;
            esac
        }
        ''' + readiness.group(0) + "\ndaemon_is_ready\n"
        subprocess.run(["sh", "-c", script], check=True)

    def service_functions(self, *names):
        service = (FIRMWARE / "buildroot/package/rte-control/S60rte-httpd").read_text()
        return "\n".join(re.search(r"^" + name + r"\(\) \{\n.*?^\}", service,
                                   re.M | re.S).group(0) for name in names)

    def test_busybox_stop_waits_for_exit_and_only_then_removes_pid(self):
        functions = self.service_functions("wait_for_exit", "stop_running_daemon")
        script = r'''
        PIDFILE=unused DAEMON=/usr/sbin/rte-httpd
        polls=0 removed=0 signals=""
        daemon_process_is_running() { [ "$polls" -lt 3 ]; }
        start-stop-daemon() { signals="$signals $*"; return 0; }
        sleep() { polls=$((polls + 1)); }
        rm() { removed=1; }
        ''' + functions + r'''
        stop_running_daemon || exit 1
        [ "$polls" -eq 3 ] && [ "$removed" -eq 1 ] || exit 2
        case "$signals" in *"-s TERM"*) ;; *) exit 3 ;; esac
        case "$signals" in *"-R"*|*"KILL"*) exit 4 ;; esac
        '''
        # dash requires an identifier for a shell function used as a mock.
        script = script.replace("start-stop-daemon", "mock_start_stop_daemon")
        subprocess.run(["sh", "-c", script], check=True)

    def test_busybox_stop_escalates_only_after_grace_period(self):
        functions = self.service_functions("wait_for_exit", "stop_running_daemon")
        script = r'''
        PIDFILE=unused DAEMON=/usr/sbin/rte-httpd
        polls=0 removed=0 killed=0
        daemon_process_is_running() { [ "$killed" -eq 0 ]; }
        start-stop-daemon() {
            case "$*" in
                *"-s TERM"*) return 0 ;;
                *"-s KILL"*) [ "$polls" -eq 50 ] || exit 3; killed=1 ;;
                *) exit 4 ;;
            esac
        }
        sleep() { polls=$((polls + 1)); }
        rm() { removed=1; }
        ''' + functions + r'''
        stop_running_daemon || exit 1
        [ "$killed" -eq 1 ] && [ "$removed" -eq 1 ]
        '''
        script = script.replace("start-stop-daemon", "mock_start_stop_daemon")
        subprocess.run(["sh", "-c", script], check=True)

    def test_start_of_running_daemon_resolves_default_usb_url(self):
        functions = self.service_functions("valid_ipv4_address", "resolve_listen_url", "do_start")
        script = r'''
        RTE_HTTPD_LISTEN_URL="" RTE_HTTPD_PORT=8080
        fw_printenv() { printf '%s\n' 192.168.2.1; }
        daemon_process_is_running() { return 0; }
        daemon_is_ready() { [ "$RTE_HTTPD_LISTEN_URL" = http://192.168.2.1:8080 ]; }
        ''' + functions + "\ndo_start\n"
        subprocess.run(["sh", "-c", script], check=True, capture_output=True)

    def test_custom_xsa_required(self):
        result = self.make("build/system_top.xsa")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XSA_FILE=/path/to/current-project/system_top.xsa", result.stderr)
        self.assertFalse((self.root / "build/system_top.xsa").exists())

    def test_explicit_xsa_precedes_vivado_and_hash_detects_old_mtime(self):
        self.write_xsa(b"first-custom-bitstream")
        result = self.make("HAVE_VIVADO=1", "VIVADO_INSTALL=2023.2",
            f"XSA_FILE={self.xsa}", "build/system_top.bit")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.root / "build/system_top.bit"
        self.assertEqual(output.read_bytes(), b"first-custom-bitstream")
        self.assertFalse((self.root / "build/sdk").exists(), "bitstream extraction must not build FSBL")
        self.write_xsa(b"second-custom-bitstream")
        os.utime(self.xsa, (1, 1))
        result = self.make(f"XSA_FILE={self.xsa}", "build/system_top.bit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), b"second-custom-bitstream")

    def test_external_archive_cannot_borrow_unrelated_ps7_init(self):
        self.write_xsa(b"custom-bitstream")
        result = self.make("HAVE_VIVADO=1", "VIVADO_INSTALL=2023.2",
            f"XSA_FILE={self.xsa}", "build/ps7_init.tcl")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ps7_init.tcl is missing", result.stderr)

if __name__ == "__main__":
    unittest.main()

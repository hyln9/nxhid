"""Exercise the USB backend against a simulated libnx service boundary."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxhid-usb-tests-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.executable = Path(cls.build.name) / "usb"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "tests/fakes"), "-I" + str(ROOT / "include"),
            str(ROOT / "source/usb_keyboard.c"), str(ROOT / "tests/usb_harness.c"),
            "-o", str(cls.executable),
        ], check=True)

    def test_descriptors_and_partial_reads(self):
        subprocess.run([str(self.executable), "descriptors"], check=True, timeout=5)

    def test_hid_controls_idle_and_disconnect(self):
        subprocess.run([str(self.executable), "controls"], check=True, timeout=5)

    def test_service_errors_short_transfers_and_timeouts(self):
        subprocess.run([str(self.executable), "errors"], check=True, timeout=5)

    def test_slow_enumeration_keeps_servicing_control_requests(self):
        subprocess.run([str(self.executable), "enumeration"], check=True, timeout=5)

    def test_idle_reports_do_not_reset_a_host_that_stops_polling(self):
        subprocess.run([str(self.executable), "idle"], check=True, timeout=5)

    def test_suspend_preserves_session_and_releases_keys_on_resume(self):
        subprocess.run([str(self.executable), "suspend"], check=True, timeout=5)

    def test_suspend_with_a_pending_report_does_not_reuse_its_buffer(self):
        subprocess.run([str(self.executable), "suspend-pending"], check=True, timeout=5)

    def test_pending_transfers_control_requests_and_state_changes(self):
        subprocess.run([str(self.executable), "transfers"], check=True, timeout=5)

    def test_control_errors_latch_until_teardown(self):
        subprocess.run([str(self.executable), "control-errors"], check=True, timeout=5)

    def test_initialization_failure_cleanup_and_bus_reset(self):
        subprocess.run([str(self.executable), "lifecycle"], check=True, timeout=5)

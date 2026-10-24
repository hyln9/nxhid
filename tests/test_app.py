"""Exercise the real Switch main loop with scripted service and input events."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AppScreenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxhid-app-tests-")
        cls.addClassCleanup(cls.build.cleanup)
        directory = Path(cls.build.name)
        cls.executable = directory / "app"
        flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "tests/fakes"), "-I" + str(ROOT / "include")]
        subprocess.run(flags + ["-Dmain=nxhid_main", "-Dprintf=ui_test_printf", "-c",
                                str(ROOT / "source/main.c"), "-o", str(directory / "main.o")], check=True)
        subprocess.run(flags + [str(directory / "main.o"), str(ROOT / "tests/app_harness.c"),
                                "-o", str(cls.executable)], check=True)

    def frames(self, scenario):
        result = subprocess.run([str(self.executable), scenario], capture_output=True, text=True,
                                cwd=self.build.name, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        frames = [re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", frame)
                  for frame in result.stdout.split("\x1e") if frame]
        self.assertTrue(frames)
        # The default libnx console is 80 columns by 45 rows.
        for frame in frames:
            self.assertLessEqual(len(frame.splitlines()), 45)
            self.assertTrue(all(len(line) <= 80 for line in frame.splitlines()))
        return frames

    def test_disconnect_notice_clears_after_reconnect(self):
        frames = self.frames("disconnect")
        self.assertIn("Sender disconnected", frames[1])
        self.assertIn("192.0.2.20", frames[-1])
        self.assertNotIn("Sender disconnected", frames[-1])
        self.assertIn("Forwarding keyboard input", frames[-1])

    def test_reset_notice_clears_after_recovery(self):
        frames = self.frames("reset")
        self.assertIn("USB reset (00005678)", frames[1])
        self.assertNotIn("Keyboard ready", frames[1])
        self.assertIn("Keyboard ready", frames[-1])
        self.assertNotIn("USB reset", frames[-1])

    def test_instructions_follow_current_connection_state(self):
        self.assertIn("Connect the Switch USB-C port", self.frames("waiting-usb")[-1])
        self.assertIn("Connect the Switch to Wi-Fi", self.frames("waiting-wifi")[-1])
        self.assertIn("Connect a sender", self.frames("ready")[-1])

    def test_startup_errors_identify_the_failed_service(self):
        for scenario, message in (
            ("focus-error", "Could not configure application focus"),
            ("socket-error", "Could not initialize networking"),
            ("wifi-error", "Could not initialize Wi-Fi"),
            ("usb-error", "Could not initialize the USB keyboard"),
            ("restart-error", "Could not restart the USB keyboard"),
        ):
            with self.subTest(scenario=scenario):
                frame = self.frames(scenario)[-1]
                self.assertIn(message, frame)
                self.assertIn("Result: 00001234", frame)

    def test_non_result_errors_do_not_display_success_codes(self):
        for scenario, message in (("applet-mode", "Full application mode required"),
                                  ("listen-error", "Cannot listen on TCP")):
            with self.subTest(scenario=scenario):
                frame = self.frames(scenario)[-1]
                self.assertIn(message, frame)
                self.assertNotIn("Result: 00000000", frame)

    def test_error_screen_is_only_rendered_in_focus(self):
        frames = self.frames("background-error")
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0], frames[1])

    def test_background_buttons_do_not_close_the_app(self):
        self.assertIn("Forwarding keyboard input", self.frames("background-input")[-1])

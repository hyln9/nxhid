import argparse
import importlib.util
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nxhid", ROOT / "client/nxhid.py")
nxhid = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nxhid)


class KeyMappingTests(unittest.TestCase):
    def test_ascii_coverage_and_shift(self):
        for code in range(32, 127):
            mod, key = nxhid.character_key(chr(code))
            self.assertIn(mod, (0, 2))
            self.assertTrue(4 <= key <= 56)
        self.assertEqual(nxhid.character_key("A"), (2, 4))
        self.assertEqual(nxhid.character_key("@"), (2, 31))
        self.assertEqual(nxhid.character_key("?"), (2, 56))
        self.assertEqual(nxhid.character_key("0"), (0, 39))

    def test_chords(self):
        self.assertEqual(nxhid.chord_report("ctrl+alt+delete"), bytes([5, 0, 76, 0, 0, 0, 0, 0]))
        self.assertEqual(nxhid.chord_report("cmd+shift+a"), bytes([10, 0, 4, 0, 0, 0, 0, 0]))
        self.assertEqual(nxhid.chord_report("shift"), bytes([2, 0, 0, 0, 0, 0, 0, 0]))
        self.assertEqual(nxhid.chord_report("f24")[2], 115)
        self.assertEqual(nxhid.chord_report("capslock"), bytes([0, 0, 57, 0, 0, 0, 0, 0]))
        self.assertEqual(nxhid.chord_report(" CTRL + A "), nxhid.chord_report("ctrl+a"))

    def test_live_key_mapping_preserves_physical_keys(self):
        self.assertEqual(nxhid.live_key("A", "\x01"), (0, 4))
        self.assertEqual(nxhid.live_key("at", "\x00"), (0, 31))
        self.assertEqual(nxhid.live_key("question", ""), (0, 56))
        self.assertEqual(nxhid.live_key("Shift_R", ""), (32, 0))
        self.assertEqual(nxhid.live_key("Caps_Lock", ""), (0, 57))
        self.assertEqual(nxhid.live_key("Win_L", ""), (8, 0))
        self.assertEqual(nxhid.live_key("Win_R", ""), (128, 0))
        self.assertEqual(nxhid.live_key("App", ""), (0, 101))
        self.assertEqual(nxhid.live_key("KP_Equal", "="), (0, 103))
        self.assertEqual(nxhid.live_key("ISO_Left_Tab", ""), (0, 43))

    def test_live_keypad_mapping_does_not_depend_on_local_num_lock(self):
        for unlocked, locked in (
            ("KP_Insert", "KP_0"), ("KP_End", "KP_1"), ("KP_Down", "KP_2"),
            ("KP_Next", "KP_3"), ("KP_Left", "KP_4"), ("KP_Begin", "KP_5"),
            ("KP_Right", "KP_6"), ("KP_Home", "KP_7"), ("KP_Up", "KP_8"),
            ("KP_Prior", "KP_9"), ("KP_Delete", "KP_Decimal"),
            ("KP_Page_Up", "KP_9"), ("KP_Page_Down", "KP_3"),
        ):
            with self.subTest(keysym=unlocked):
                self.assertEqual(nxhid.live_key(unlocked, ""), nxhid.live_key(locked, ""))
        self.assertEqual(nxhid.live_key("Home", ""), (0, 74))

    def test_transport_timeout_closes_the_session(self):
        client = nxhid.Client("unused")
        sock, reader = mock.Mock(), mock.Mock()
        client.sock, client.reader = sock, reader
        reader.readline.side_effect = TimeoutError("timed out")
        with self.assertRaises(TimeoutError):
            client.request("PING")
        sock.close.assert_called_once()
        reader.close.assert_called_once()
        self.assertIsNone(client.sock)
        self.assertIsNone(client.reader)

    def test_failed_final_release_does_not_report_success(self):
        client = nxhid.Client("unused")
        client.sock = mock.Mock()
        client.request = mock.Mock(side_effect=ConnectionError("release failed"))
        with self.assertRaisesRegex(ConnectionError, "release failed"):
            client.__exit__(None, None, None)
        self.assertIsNone(client.sock)

    def test_failed_cleanup_preserves_original_error(self):
        client = nxhid.Client("unused")
        client.sock = mock.Mock()
        client.request = mock.Mock(side_effect=ConnectionError("release failed"))
        client.__exit__(ValueError, ValueError("original failure"), None)
        self.assertIsNone(client.sock)

    def test_led_response_validation(self):
        client = nxhid.Client("unused")
        for response in (b"", b"OK\n", b"LEDS gg\n", b"LEDS 20\n", b"LEDS 02x"):
            with self.subTest(response=response):
                client.exchange = lambda _line: response
                with self.assertRaises(ConnectionError):
                    client.leds()
        client.exchange = lambda _line: b"LEDS 1F\n"
        self.assertEqual(client.leds(), 31)

    def test_invalid_keys(self):
        for value in ("é", "😀", "\x00", "", "ab"):
            with self.assertRaises(ValueError):
                nxhid.character_key(value)
        for keys in ([1], [116], list(range(4, 11))):
            with self.assertRaises(ValueError):
                nxhid.make_report(keys=keys)
        self.assertEqual(nxhid.make_report(keys=[4, 4]), nxhid.make_report(keys=[4]))

    def test_sequence_parsing(self):
        self.assertEqual(nxhid.parse_sequence(" HOME, CTRL + DELETE, Ctrl+A "),
                         (nxhid.chord_report("home"), nxhid.chord_report("ctrl+delete"),
                          nxhid.chord_report("ctrl+a")))
        for value in ("", "HOME,", ",HOME", "HOME,,ENTER", "CTRL+", "UNKNOWN"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    nxhid.parse_sequence(value)

    def test_text_prevalidation(self):
        class Recorder:
            def __init__(self):
                self.reports = []

            def report(self, report):
                self.reports.append(report)

            def leds(self):
                return 0

        recorder = Recorder()
        with self.assertRaises(ValueError):
            nxhid.send_text(recorder, "abcé")
        self.assertEqual(recorder.reports, [])
        nxhid.send_text(recorder, "aA\n", delay=0)
        self.assertEqual(recorder.reports, [nxhid.make_report(0, [4]), bytes(8),
                                         nxhid.make_report(2, [4]), bytes(8),
                                         nxhid.make_report(0, [40]), bytes(8)])

    def test_newline_sequence_follows_typing_delay(self):
        for gap in (0.4, 0, 0.03):
            with self.subTest(delay=gap):
                now = [0.0]
                events = []
                client = mock.Mock()
                client.leds.return_value = 0
                client.report.side_effect = lambda report: events.append((round(now[0], 6), report))

                def sleep(seconds):
                    now[0] += seconds

                with mock.patch.object(nxhid.time, "monotonic", side_effect=lambda: now[0]), \
                        mock.patch.object(nxhid.time, "sleep", side_effect=sleep):
                    nxhid.send_text(client, "a\nb", delay=gap,
                                    newline_sequence=nxhid.parse_sequence("HOME,CTRL+DELETE"))
                self.assertEqual(events, [
                    (0.0, nxhid.chord_report("a")), (0.0, nxhid.RELEASE),
                    (gap, nxhid.chord_report("home")), (round(gap + 0.05, 6), nxhid.RELEASE),
                    (round(0.05 + gap * 2, 6), nxhid.chord_report("ctrl+delete")),
                    (round(0.10 + gap * 2, 6), nxhid.RELEASE),
                    (round(0.10 + gap * 3, 6), nxhid.chord_report("b")),
                    (round(0.10 + gap * 3, 6), nxhid.RELEASE)])
                self.assertAlmostEqual(now[0], 0.10 + gap * 4)


class LiveRuntimeTests(unittest.TestCase):
    def test_legacy_macos_tk_fails_before_opening_a_window(self):
        tk = mock.Mock(TkVersion=8.5, TclError=RuntimeError)
        tk.Tk.side_effect = AssertionError("Unsupported Tk must not create a window")
        with mock.patch.dict(sys.modules, {"tkinter": tk}), \
                mock.patch.object(nxhid.sys, "platform", "darwin"):
            with self.assertRaisesRegex(RuntimeError, "Tk 8.6 or newer.*python3.14"):
                nxhid.live("unused", 1)
        tk.Tk.assert_not_called()

    def test_missing_macos_tk_reports_matching_python_command(self):
        with mock.patch.dict(sys.modules, {"tkinter": None}), \
                mock.patch.object(nxhid.sys, "platform", "darwin"):
            with self.assertRaisesRegex(RuntimeError, "python-tk@3.14.*python3.14"):
                nxhid.live("unused", 1)

    def test_supported_runtimes_attempt_to_create_the_window(self):
        for platform, version in (("darwin", 8.6), ("darwin", 9.0), ("linux", 8.5)):
            with self.subTest(platform=platform, version=version):
                tk = mock.Mock(TkVersion=version, TclError=RuntimeError)
                tk.Tk.side_effect = RuntimeError("test display unavailable")
                with mock.patch.dict(sys.modules, {"tkinter": tk}), \
                        mock.patch.object(nxhid.sys, "platform", platform):
                    with self.assertRaisesRegex(RuntimeError, "Cannot open the live keyboard window"):
                        nxhid.live("unused", 1)
                tk.Tk.assert_called_once()


class ServerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxhid-tests-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.executable = Path(cls.build.name) / "server"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-D_POSIX_C_SOURCE=200809L", "-I" + str(ROOT / "include"),
                        str(ROOT / "source/server.c"), str(ROOT / "source/protocol.c"),
                        str(ROOT / "tests/server_harness.c"), "-o", str(cls.executable)], check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxhid-session-")
        self.addCleanup(self.temp.cleanup)
        self.process = None
        self.addCleanup(self.stop)
        self.sockets = []
        self.log = Path(self.temp.name) / "reports.txt"
        self.leds_file = Path(self.temp.name) / "leds.txt"
        self.leds_file.write_text("0")
        self.start()

    def start(self, fail=False):
        self.process = subprocess.Popen([str(self.executable), str(self.log),
                                         "fail" if fail else "ok", str(self.leds_file)],
                                        stdout=subprocess.PIPE, text=True)
        line = self.process.stdout.readline()
        self.assertTrue(line, "Native server did not start")
        self.port = int(line)

    def stop(self):
        process, self.process = self.process, None
        if process is not None:
            try:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            finally:
                process.stdout.close()

    def tearDown(self):
        for sock in self.sockets:
            sock.close()

    def connect(self, ping=True):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=1)
        self.sockets.append(sock)
        if ping:
            sock.sendall(b"PING\n")
            self.assertEqual(self.line(sock), b"OK\n")
        return sock

    def line(self, sock):
        result = b""
        while not result.endswith(b"\n"):
            chunk = sock.recv(1)
            if not chunk:
                break
            result += chunk
        return result

    def reports(self, count):
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            lines = self.log.read_text().splitlines()
            if len(lines) >= count:
                return lines
            time.sleep(0.01)
        self.fail(f"Expected {count} reports, got {lines}")

    def test_real_python_client_and_release(self):
        with nxhid.Client("127.0.0.1", self.port) as client:
            nxhid.send_text(client, "Ab", delay=0)
        self.assertEqual(self.reports(5)[:5], ["0200040000000000", "0000000000000000",
                                              "0000050000000000", "0000000000000000", "0000000000000000"])

    def test_text_preserves_case_with_caps_lock_on(self):
        self.leds_file.write_text("7")  # Caps + Num + Scroll, only Caps affects text.
        with nxhid.Client("127.0.0.1", self.port) as client:
            self.assertEqual(client.leds(), 7)
            nxhid.send_text(client, "aAzZ1!?", delay=0)
            self.assertEqual(client.leds(), 7)  # Compensation must not toggle Caps.
        expected = []
        for mod, key in ((2, 4), (0, 4), (2, 29), (0, 29), (0, 30), (2, 30), (2, 56)):
            expected.extend([nxhid.make_report(mod, [key]).hex(), bytes(8).hex()])
        self.assertEqual(self.reports(15)[:15], expected + [bytes(8).hex()])

    def test_capslock_chord_toggles_target_and_raw_reports_stay_raw(self):
        with nxhid.Client("127.0.0.1", self.port) as client:
            client.report(nxhid.chord_report("capslock"))
            client.report(nxhid.RELEASE)
            self.assertEqual(client.leds(), 2)
            client.report(nxhid.chord_report("ctrl+a"))
            client.report(nxhid.RELEASE)
            nxhid.send_text(client, "a", delay=0)
            client.report(nxhid.chord_report("capslock"))
            client.report(nxhid.RELEASE)
            self.assertEqual(client.leds(), 0)
            nxhid.send_text(client, "a", delay=0)
        reports = self.reports(11)
        self.assertEqual(reports[2], "0100040000000000")
        self.assertEqual(reports[4], "0200040000000000")
        self.assertEqual(reports[8], "0000040000000000")

    def test_led_query_does_not_emit_keyboard_reports(self):
        self.leds_file.write_text("2")
        sock = self.connect()
        sock.sendall(b"LEDS\n")
        self.assertEqual(self.line(sock), b"LEDS 02\n")
        self.assertEqual(self.log.read_text(), "")

    def run_text_cli(self, input_text, *args):
        return subprocess.run([sys.executable, str(ROOT / "client/nxhid.py"),
                               "127.0.0.1", "--port", str(self.port), "text",
                               "--delay", "0", *args], input=input_text,
                              encoding="utf-8", capture_output=True, timeout=5)

    def start_text_stream(self, *args):
        process = subprocess.Popen([sys.executable, "-X", "utf8", str(ROOT / "client/nxhid.py"),
                                    "127.0.0.1", "--port", str(self.port), "text",
                                    "--file", "-", "--delay", "0", *args],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)

        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

        self.addCleanup(cleanup)
        return process

    def test_cli_piped_stdin_preserves_whitespace(self):
        result = self.run_text_cli(" A\t\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = []
        for mod, key in ((0, 44), (2, 4), (0, 43), (0, 40)):
            expected.extend([nxhid.make_report(mod, [key]).hex(), bytes(8).hex()])
        self.assertEqual(self.reports(9)[:9], expected + [bytes(8).hex()])

    def test_cli_file_dash_reads_stdin(self):
        result = self.run_text_cli("b", "--file", "-")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.reports(3)[:3], ["0000050000000000", "0000000000000000", "0000000000000000"])

    def test_cli_newline_sequence_replace_each_streamed_newline(self):
        result = self.run_text_cli("\na\r\n\n", "--newline-sequence", "HOME,CTRL+DELETE",
                                   "--delay", "0.01")
        self.assertEqual(result.returncode, 0, result.stderr)
        sequence = ["00004a0000000000", "0000000000000000",
                    "01004c0000000000", "0000000000000000"]
        expected = sequence + ["0000040000000000", "0000000000000000"] + sequence * 2
        self.assertEqual(self.reports(15)[:15], expected + [bytes(8).hex()])

    def test_cli_newline_sequence_allow_explicit_enter_in_literal_text(self):
        result = self.run_text_cli("", "\r\n", "--newline-sequence", "ENTER,HOME")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.reports(5)[:5], ["0000280000000000", "0000000000000000",
                                              "00004a0000000000", "0000000000000000", "0000000000000000"])

    def test_cli_newline_sequence_file_keeps_chords_raw_with_caps_on(self):
        self.leds_file.write_text("2")
        path = Path(self.temp.name) / "text.txt"
        path.write_text("a\nA")
        result = self.run_text_cli("", "--file", str(path), "--newline-sequence", "CTRL+A")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.reports(7)[:7], ["0200040000000000", "0000000000000000",
                                              "0100040000000000", "0000000000000000",
                                              "0000040000000000", "0000000000000000", "0000000000000000"])

    def test_cli_invalid_newline_sequence_sends_nothing(self):
        result = self.run_text_cli("a\n", "--newline-sequence", "HOME,UNKNOWN")
        self.assertEqual(result.returncode, 2)
        self.assertIn("Invalid sequence item", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_cli_rejects_removed_sequence_delay_options(self):
        for option in ("--newline-sequence-delay", "--newline-shortcut-delay"):
            with self.subTest(option=option):
                result = self.run_text_cli("a\n", option, "0.3")
                self.assertEqual(result.returncode, 2)
                self.assertIn("unrecognized arguments", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_cli_regular_file_handles_caps_lock(self):
        self.leds_file.write_text("2")
        path = Path(self.temp.name) / "text.txt"
        path.write_text("aA")
        result = self.run_text_cli("", "--file", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.reports(5)[:5], ["0200040000000000", "0000000000000000",
                                              "0000040000000000", "0000000000000000", "0000000000000000"])

    def test_cli_invalid_stdin_stops_after_valid_prefix(self):
        result = self.run_text_cli("abcé")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unsupported character", result.stderr)
        expected = []
        for key in (4, 5, 6):
            expected.extend([nxhid.make_report(0, [key]).hex(), bytes(8).hex()])
        self.assertEqual(self.reports(7)[:7], expected + [bytes(8).hex()])

    def test_cli_stdin_streams_before_eof_and_survives_idle(self):
        self.leds_file.write_text("2")
        process = self.start_text_stream()
        process.stdin.write(b"a")
        process.stdin.flush()
        self.assertEqual(self.reports(2), ["0200040000000000", "0000000000000000"])
        self.assertIsNone(process.poll())
        self.leds_file.write_text("0")  # Target Caps Lock changes during the stream.
        time.sleep(3.2)  # Longer than the server watchdog, with stdin still open.
        process.stdin.write(b"B")
        process.stdin.flush()
        self.assertEqual(self.reports(4)[2:], ["0200050000000000", "0000000000000000"])
        process.stdin.close()
        process.stdin = None
        _, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, error)
        self.assertEqual(self.reports(5)[4], "0000000000000000")

    def test_cli_carriage_return_streams_immediately_and_split_crlf_is_one_newline(self):
        process = self.start_text_stream("--newline-sequence", "ENTER,HOME,CTRL+DELETE")
        process.stdin.write(b"a\r")
        process.stdin.flush()
        expected = []
        for chord in ("a", "enter", "home", "ctrl+delete"):
            expected.extend([nxhid.chord_report(chord).hex(), nxhid.RELEASE.hex()])
        self.assertEqual(self.reports(8), expected)
        self.assertIsNone(process.poll())
        process.stdin.write(b"\nB")
        process.stdin.flush()
        expected.extend([nxhid.make_report(2, [5]).hex(), nxhid.RELEASE.hex()])
        self.assertEqual(self.reports(10), expected)
        process.stdin.close()
        process.stdin = None
        _, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, error)
        # Both graceful client cleanup and server disconnect release keys.
        self.assertEqual(self.reports(12), expected + [nxhid.RELEASE.hex()] * 2)

    def test_cli_invalid_utf8_keeps_valid_prefix_and_exits_with_stdin_open(self):
        process = self.start_text_stream()
        process.stdin.write(b"ab\xff")
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=5), 1)
        _, error = process.communicate(timeout=5)
        self.assertIn(b"decode", error)
        self.assertNotIn(b"Traceback", error)
        self.assertEqual(self.reports(6), ["0000040000000000", "0000000000000000",
                                          "0000050000000000", "0000000000000000",
                                          "0000000000000000",
                                          "0000000000000000"])

    def test_cli_disconnect_exits_with_stdin_still_open(self):
        process = self.start_text_stream()
        process.stdin.write(b"a")
        process.stdin.flush()
        self.reports(2)
        self.stop()
        self.assertEqual(process.wait(timeout=5), 1)
        _, error = process.communicate(timeout=5)
        self.assertIn(b"nxhid:", error)
        self.assertNotIn(b"Traceback", error)

    def test_cli_literal_text_still_validates_before_sending(self):
        result = self.run_text_cli("", "abcé")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unsupported character", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_cli_rejects_multiple_text_sources(self):
        result = self.run_text_cli("b", "a", "--file", "-")
        self.assertEqual(result.returncode, 2)
        self.assertIn("not both", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_fragmented_and_coalesced_tcp_lines(self):
        sock = self.connect(ping=False)
        for part in (b"PI", b"NG\nREP", b"ORT 010004", b"0000000000\nPING\n"):
            sock.sendall(part)
        self.assertEqual(self.line(sock), b"OK\n")
        self.assertEqual(self.line(sock), b"OK\n")
        self.assertEqual(self.line(sock), b"OK\n")
        sock.close()
        self.assertEqual(self.reports(2), ["0100040000000000", "0000000000000000"])

    def test_report_accepted_without_handshake(self):
        sock = self.connect(ping=False)
        sock.sendall(b"REPORT 0000040000000000\n")
        self.assertEqual(self.line(sock), b"OK\n")
        sock.close()
        self.assertEqual(self.reports(2), ["0000040000000000", "0000000000000000"])

    def test_release_accepted_as_first_command(self):
        sock = self.connect(ping=False)
        sock.sendall(b"RELEASE\n")
        self.assertEqual(self.line(sock), b"OK\n")
        self.assertEqual(self.reports(1), ["0000000000000000"])

    def test_malformed_report_releases_keys(self):
        sock = self.connect()
        sock.sendall(b"REPORT 0000040000000000\n")
        self.assertEqual(self.line(sock), b"OK\n")
        sock.sendall(b"REPORT 0000040400000000\n")  # duplicate usage
        self.assertEqual(self.line(sock), b"")
        self.assertEqual(self.reports(2)[-1], "0000000000000000")

    def test_invalid_reports(self):
        for report in ("0101040000000000", "0000010000000000", "0000740000000000", "0000gg0000000000", "00", "00000000000000000"):
            with self.subTest(report=report):
                sock = self.connect()
                sock.sendall(("REPORT " + report + "\n").encode())
                self.assertEqual(self.line(sock), b"")
                sock.close()

    def test_oversized_line_and_nul(self):
        for message in (b"x" * 96, b"PING\x00\n"):
            sock = self.connect()
            sock.sendall(message)
            try:
                self.assertEqual(self.line(sock), b"")
            except ConnectionResetError:
                pass
            sock.close()

    def test_second_sender_rejected(self):
        first = self.connect()
        second = self.connect(ping=False)
        self.assertEqual(second.recv(1), b"")
        first.sendall(b"PING\n")
        self.assertEqual(self.line(first), b"OK\n")

    def test_reconnect_after_closed_sender_is_accepted(self):
        first = self.connect()
        first.sendall(b"REPORT 0100040000000000\n")
        self.assertEqual(self.line(first), b"OK\n")
        # Queue the previous sender's EOF and the new connection before the next
        # server tick, without relying on the scheduler to reproduce the race.
        self.process.send_signal(signal.SIGSTOP)
        try:
            _, status = os.waitpid(self.process.pid, os.WUNTRACED)
            self.assertTrue(os.WIFSTOPPED(status))
            first.close()
            second = self.connect(ping=False)
            second.sendall(b"PING\n")
        finally:
            self.process.send_signal(signal.SIGCONT)
        self.assertEqual(self.line(second), b"OK\n")
        self.assertEqual(self.reports(2), ["0100040000000000", "0000000000000000"])

    def test_watchdog_releases_held_key(self):
        sock = self.connect()
        sock.sendall(b"REPORT 0100040000000000\n")
        self.assertEqual(self.line(sock), b"OK\n")
        sock.settimeout(4)
        self.assertEqual(self.line(sock), b"")
        self.assertEqual(self.reports(2)[-1], "0000000000000000")

    def test_heartbeat_keeps_chord_held(self):
        with nxhid.Client("127.0.0.1", self.port) as client:
            client.report(nxhid.chord_report("shift+a"))
            nxhid.wait_with_heartbeat(client, 3.2)
            self.assertEqual(self.log.read_text().splitlines(), ["0200040000000000"])
        self.assertEqual(self.reports(2)[1], "0000000000000000")

    def test_usb_failure_is_not_acknowledged(self):
        self.stop()
        self.start(fail=True)
        sock = self.connect()
        sock.sendall(b"REPORT 0000040000000000\n")
        self.assertEqual(self.line(sock), b"")
        self.assertEqual(self.reports(2)[-1], "0000000000000000")


if __name__ == "__main__":
    unittest.main()

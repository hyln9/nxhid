#!/usr/bin/env python3
"""Send US-layout keyboard reports to NXHID over Wi-Fi."""

import argparse
import codecs
import os
from pathlib import Path
import queue
import select
import socket
import sys
import threading
import time

PORT = 24872
RELEASE = bytes(8)
HEARTBEAT_INTERVAL = 0.5
SEQUENCE_HOLD = 0.05
MODIFIERS = {
    "ctrl": 1, "control": 1, "ctrl_l": 1, "control_l": 1,
    "shift": 2, "shift_l": 2, "alt": 4, "alt_l": 4,
    "gui": 8, "win": 8, "win_l": 8, "cmd": 8, "meta_l": 8, "super_l": 8,
    "ctrl_r": 16, "control_r": 16, "shift_r": 32, "alt_r": 64,
    "meta_r": 128, "super_r": 128, "win_r": 128,
}
KEYS = {
    "enter": 40, "return": 40, "esc": 41, "escape": 41, "backspace": 42,
    "tab": 43, "space": 44, "minus": 45, "equal": 46, "bracketleft": 47,
    "bracketright": 48, "backslash": 49, "semicolon": 51, "apostrophe": 52,
    "quoteleft": 53, "comma": 54, "period": 55, "slash": 56,
    "capslock": 57, "caps_lock": 57, "printscreen": 70, "print": 70,
    "scrolllock": 71, "scroll_lock": 71, "pause": 72, "insert": 73,
    "home": 74, "pageup": 75, "prior": 75, "delete": 76,
    "end": 77, "pagedown": 78, "next": 78, "right": 79,
    "left": 80, "down": 81, "up": 82, "numlock": 83, "num_lock": 83,
    "kp_divide": 84, "kp_multiply": 85, "kp_subtract": 86, "kp_add": 87,
    "kp_enter": 88, "kp_0": 98, "kp_decimal": 99, "menu": 101, "app": 101,
    "kp_equal": 103, "iso_left_tab": 43,
}
KEYS.update({f"f{i}": 57+i for i in range(1, 13)})
KEYS.update({f"f{i}": 104+i-13 for i in range(13, 25)})
KEYS.update({f"kp_{i}": 88+i for i in range(1, 10)})
# Tk changes keypad keysyms with local Num Lock. Forward the same physical usage
# so the target's lock state decides whether it acts as a digit or navigation key.
KEYS.update({
    "kp_insert": 98, "kp_end": 89, "kp_down": 90, "kp_next": 91,
    "kp_left": 92, "kp_begin": 93, "kp_right": 94, "kp_home": 95,
    "kp_up": 96, "kp_prior": 97, "kp_delete": 99,
    "kp_page_up": 97, "kp_page_down": 91,
})
PUNCTUATION = {
    " ": (0, 44), "-": (0, 45), "=": (0, 46), "[": (0, 47), "]": (0, 48),
    "\\": (0, 49), ";": (0, 51), "'": (0, 52), "`": (0, 53),
    ",": (0, 54), ".": (0, 55), "/": (0, 56),
    "\n": (0, 40), "\r": (0, 40), "\t": (0, 43), "\b": (0, 42),
}
# Shifted Tk keysyms still identify the physical key when Ctrl makes event.char
# a control character (for example, Ctrl+Shift+2 has keysym "at").
TK_SYMBOLS = {
    "exclam": "!", "quotedbl": '"', "numbersign": "#", "dollar": "$",
    "percent": "%", "ampersand": "&", "parenleft": "(", "parenright": ")",
    "asterisk": "*", "plus": "+", "colon": ":", "less": "<",
    "greater": ">", "question": "?", "at": "@", "asciicircum": "^",
    "underscore": "_", "braceleft": "{", "bar": "|", "braceright": "}",
    "asciitilde": "~", "grave": "`",
}
for unshifted, shifted in zip("1234567890-=[]\\;'`,./", '!@#$%^&*()_+{}|:"~<>?'):
    usage = 39 if unshifted == "0" else (29 + int(unshifted) if unshifted.isdigit() else PUNCTUATION[unshifted][1])
    PUNCTUATION[shifted] = (2, usage)


def character_key(char):
    if len(char) != 1:
        raise ValueError("Expected one character")
    if "a" <= char <= "z":
        return 0, ord(char) - ord("a") + 4
    if "A" <= char <= "Z":
        return 2, ord(char) - ord("A") + 4
    if char in "1234567890":
        return 0, 39 if char == "0" else 29 + int(char)
    if char in PUNCTUATION:
        return PUNCTUATION[char]
    raise ValueError(f"Unsupported character {char!r}; text mode supports US-layout ASCII")


def make_report(modifiers=0, keys=()):
    keys = sorted(set(keys))
    if not 0 <= modifiers <= 255 or any(not 4 <= key <= 0x73 for key in keys):
        raise ValueError("Invalid HID key or modifier")
    if len(keys) > 6:
        raise ValueError("A boot keyboard supports at most six non-modifier keys")
    return bytes([modifiers, 0] + keys + [0] * (6 - len(keys)))


def chord_report(chord):
    modifiers, keys = 0, []
    for part in chord.split("+"):
        name = part.strip().lower()
        if name in MODIFIERS:
            modifiers |= MODIFIERS[name]
        elif name in KEYS:
            keys.append(KEYS[name])
        else:
            extra, key = character_key(name)
            modifiers |= extra
            keys.append(key)
    return make_report(modifiers, keys)


def parse_sequence(value):
    reports = []
    for chord in value.split(","):
        try:
            reports.append(chord_report(chord))
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"Invalid sequence item {chord!r}: {exc}") from exc
    return tuple(reports)


def live_key(keysym, char):
    """Map a Tk event to physical modifiers and usage, ignoring local Caps Lock."""
    name = keysym.lower()
    if name in MODIFIERS:
        return MODIFIERS[name], 0
    if name in KEYS:
        return 0, KEYS[name]
    symbol = TK_SYMBOLS.get(name, keysym if len(keysym) == 1 else char)
    _, usage = character_key(symbol)
    return 0, usage


class Client:
    def __init__(self, host, port=PORT, timeout=2.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = self.reader = None

    def __enter__(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), self.timeout)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.reader = self.sock.makefile("rb")
            self.request("PING")
            return self
        except BaseException:
            self.close()
            raise

    def exchange(self, line):
        try:
            self.sock.sendall((line + "\n").encode("ascii"))
            response = self.reader.readline(128)
            if not response.endswith(b"\n"):
                raise ConnectionError("Switch closed the session or sent an incomplete response")
            return response
        except OSError:
            # A timed-out buffered reader cannot be reused. Closing the socket
            # also lets the server release keys instead of waiting for its watchdog.
            self.close()
            raise

    def request(self, line):
        response = self.exchange(line)
        if response != b"OK\n":
            raise ConnectionError("Switch closed/rejected the session. Check USB status, focus, and whether another sender is connected.")

    def leds(self):
        response = self.exchange("LEDS")
        if (len(response) != 8 or not response.startswith(b"LEDS ") or
                response[-1:] != b"\n" or
                any(c not in b"0123456789abcdefABCDEF" for c in response[5:7])):
            raise ConnectionError("Cannot read target keyboard LEDs; update the Switch app and check USB status.")
        value = int(response[5:7], 16)
        if value > 0x1f:
            raise ConnectionError("Invalid keyboard LED state from Switch")
        return value

    def report(self, report):
        if len(report) != 8:
            raise ValueError("A keyboard report must be eight bytes")
        self.request("REPORT " + report.hex())

    def close(self):
        reader, sock = self.reader, self.sock
        self.reader = self.sock = None
        try:
            if reader:
                reader.close()
        finally:
            if sock:
                sock.close()

    def __exit__(self, exc_type, _exc, _traceback):
        try:
            if self.sock:
                self.request("RELEASE")
        except (OSError, ValueError):
            if exc_type is None:
                raise
        finally:
            self.close()


def wait_with_heartbeat(client, seconds):
    deadline = time.monotonic() + seconds
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return
        time.sleep(min(left, HEARTBEAT_INTERVAL))
        if time.monotonic() < deadline:
            client.request("PING")


def send_text(client, text, delay=0.02, newline_sequence=None):
    # Validate everything first, so unsupported Unicode never sends a partial string.
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    keys = [character_key(char) for char in text]
    for mod, key in keys:
        if key == 40 and newline_sequence is not None:
            for report in newline_sequence:
                client.report(report)
                wait_with_heartbeat(client, SEQUENCE_HOLD)
                client.report(RELEASE)
                wait_with_heartbeat(client, delay)
            continue
        # Caps Lock reverses Shift's effect on US-layout letters only. Query
        # before each letter so a long-running stdin stream follows host changes.
        if 4 <= key <= 29 and client.leds() & 0x02:
            mod ^= 0x02
        client.report(make_report(mod, [key]))
        client.report(RELEASE)
        wait_with_heartbeat(client, delay)


def stream_text(client, stream, delay=0.02, newline_sequence=None):
    """Read available stdin bytes without blocking the sender's heartbeat loop."""
    incoming = queue.Queue(maxsize=256)
    stopped = threading.Event()
    fd = stream.fileno()
    decoder = codecs.getincrementaldecoder(stream.encoding or "utf-8")("strict")

    def publish(item):
        while not stopped.is_set():
            try:
                incoming.put(item, timeout=0.1)
                return True
            except queue.Full:
                pass
        return False

    def read_input():
        previous_cr = False

        def publish_text(text):
            nonlocal previous_cr
            for char in text:
                # Emit CR immediately, then swallow a following LF, even if the
                # pair arrives in separate pipe writes. Never wait for EOF here.
                if char == "\n" and previous_cr:
                    previous_cr = False
                    continue
                previous_cr = char == "\r"
                if not publish("\n" if previous_cr else char):
                    return False
            return True

        try:
            while not stopped.is_set():
                if os.name == "posix" and not select.select([fd], [], [], 0.1)[0]:
                    continue
                # A pipe read returns available bytes without waiting for a newline
                # or a full buffer. Avoid taking sys.stdin's buffered-reader lock:
                # this daemon may still be blocked when the process exits.
                data = os.read(fd, 4096)
                # Decode incrementally so a malformed byte cannot discard the
                # valid text that preceded it in the same read.
                for byte in data:
                    if not publish_text(decoder.decode(bytes((byte,)))):
                        return
                if not data:
                    if publish_text(decoder.decode(b"", final=True)):
                        publish(None)
                    return
        except (OSError, ValueError) as exc:
            publish(exc)

    thread = threading.Thread(target=read_input, daemon=True)
    thread.start()
    try:
        while True:
            try:
                item = incoming.get(timeout=HEARTBEAT_INTERVAL)
            except queue.Empty:
                client.request("PING")
                continue
            if item is None:
                return
            if isinstance(item, Exception):
                raise item
            send_text(client, item, delay, newline_sequence)
    finally:
        stopped.set()
        # POSIX readers can leave select promptly. Windows pipe reads may stay
        # blocked, so shutdown never waits indefinitely for the producer.
        thread.join(timeout=0.2)


def live(host, port):
    """Focused Tk window, with socket work kept off the GUI thread."""
    tk_help = ("Install python-tk@3.14 with Homebrew and run the client with python3.14."
               if sys.platform == "darwin" else
               "Install Python's Tk support (usually the python3-tk package).")
    try:
        import tkinter as tk
    except ImportError as exc:
        raise RuntimeError(f"Live mode needs Tkinter. {tk_help}") from exc

    # Apple's legacy Tk can create a window without drawing any of its widgets.
    # Check before creating the root so an unsupported runtime never opens it.
    if sys.platform == "darwin" and tk.TkVersion < 8.6:
        raise RuntimeError(f"Live mode needs Tk 8.6 or newer on macOS; this Python uses "
                           f"Tk {tk.TkVersion:g}, which can display an empty window. {tk_help}")

    try:
        root = tk.Tk()
    except tk.TclError as exc:
        raise RuntimeError(f"Cannot open the live keyboard window: {exc}") from exc
    root.title("NXHID — live keyboard")
    root.geometry("650x300")
    status = tk.StringVar(value="Connecting…")
    tk.Label(root, text="Wi-Fi → Switch → USB keyboard", font=("TkDefaultFont", 20)).pack(pady=24)
    tk.Label(root, text="Type while this window is focused. Escape stops the sender.\n"
                       "Switching windows releases all keys. US keyboard layout.", justify="center").pack(pady=10)
    tk.Label(root, textvariable=status, wraplength=600).pack(pady=20)
    outgoing, updates = queue.Queue(maxsize=256), queue.Queue()
    stop, connected = threading.Event(), threading.Event()
    held, pending_releases = {}, {}
    closing = False

    def worker():
        try:
            with Client(host, port) as client:
                connected.set()
                updates.put("Connected. Click here and type; Escape stops.")
                while not stop.is_set():
                    try:
                        report = outgoing.get(timeout=HEARTBEAT_INTERVAL)
                    except queue.Empty:
                        client.request("PING")
                    else:
                        if not stop.is_set():
                            client.report(report)
        except (OSError, ValueError) as exc:
            updates.put(f"Disconnected: {exc}")
        finally:
            connected.clear()
            stop.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()

    def enqueue(report):
        if not connected.is_set() or stop.is_set():
            return
        try:
            outgoing.put_nowait(report)
        except queue.Full:
            status.set("Sender fell behind; disconnecting and releasing keys.")
            stop.set()

    def current_report():
        modifiers, keys = 0, []
        for mod, usage in held.values():
            modifiers |= mod
            if usage:
                keys.append(usage)
        return make_report(modifiers, keys)

    def clear_keys(_event=None):
        for job in pending_releases.values():
            root.after_cancel(job)
        pending_releases.clear()
        held.clear()
        # Discard unsent keystrokes on focus loss; the next report releases everything.
        while True:
            try:
                outgoing.get_nowait()
            except queue.Empty:
                break
        enqueue(RELEASE)

    def close():
        nonlocal closing
        closing = True
        clear_keys()
        stop.set()
        status.set("Releasing keys and disconnecting…")

    def press(event):
        if event.keysym == "Escape":
            close()
            return "break"
        if not connected.is_set() or stop.is_set():
            return "break"
        if event.keycode in pending_releases:
            root.after_cancel(pending_releases.pop(event.keycode))
        if event.keycode in held:
            return "break"  # USB host generates repeat; don't duplicate local repeat events.
        try:
            item = live_key(event.keysym, event.char)
        except ValueError:
            return "break"
        held[event.keycode] = item
        try:
            enqueue(current_report())
        except ValueError as exc:
            held.pop(event.keycode, None)
            status.set(str(exc))
        return "break"

    def release(event):
        # X11 auto-repeat sends release/press pairs. Defer releases to the idle queue.
        def finish():
            pending_releases.pop(event.keycode, None)
            held.pop(event.keycode, None)
            enqueue(current_report())
        previous = pending_releases.pop(event.keycode, None)
        if previous:
            root.after_cancel(previous)
        pending_releases[event.keycode] = root.after_idle(finish)
        return "break"

    def refresh():
        while True:
            try:
                status.set(updates.get_nowait())
            except queue.Empty:
                break
        if closing and not thread.is_alive():
            root.destroy()
            return
        root.after(50, refresh)

    root.bind("<KeyPress>", press)
    root.bind("<KeyRelease>", release)
    root.bind("<FocusOut>", clear_keys)
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(50, refresh)
    root.focus_set()
    try:
        root.mainloop()
    finally:
        stop.set()
        thread.join(timeout=5)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Switch Wi-Fi IPv4 address shown in NXHID")
    parser.add_argument("--port", type=int, default=PORT)
    commands = parser.add_subparsers(dest="command", required=True)
    text = commands.add_parser("text", help="Type US-layout ASCII text")
    text.add_argument("value", nargs="?", help="Text to send; streams stdin as it arrives when omitted without --file")
    text.add_argument("--file", type=Path, help="Read UTF-8 text file, or - for stdin (characters must be supported ASCII)")
    text.add_argument("--delay", type=float, default=0.02, help="Seconds after each character or newline sequence step (default 0.02)")
    text.add_argument("--newline-sequence", type=parse_sequence, metavar="CHORDS",
                      help='Replace each newline with a comma-separated key sequence, e.g. "ENTER,HOME,CTRL+DELETE"')
    press = commands.add_parser("press", help="Press a chord, such as ctrl+alt+delete")
    press.add_argument("chord")
    press.add_argument("--hold", type=float, default=SEQUENCE_HOLD, help="Seconds to hold the chord (default 0.05)")
    commands.add_parser("live", help="Forward keys typed into a focused desktop window (requires Tk)")
    commands.add_parser("release", help="Release all keys")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    try:
        if args.command == "text":
            value = None
            if args.value is not None and args.file is not None:
                parser.error("text accepts either a value or --file, not both")
            if not 0 <= args.delay <= 60:
                parser.error("delay must be between 0 and 60 seconds")
            if args.file is not None and args.file != Path("-"):
                value = args.file.read_text(encoding="utf-8")
            elif args.value is not None:
                value = args.value
            if value is not None:
                for char in value:
                    character_key(char)
        if args.command == "press":
            report = chord_report(args.chord)
            if not 0 <= args.hold <= 60:
                parser.error("hold must be between 0 and 60 seconds")
        if args.command == "live":
            live(args.host, args.port)
        else:
            with Client(args.host, args.port) as client:
                if args.command == "text":
                    if value is None:
                        stream_text(client, sys.stdin, args.delay, args.newline_sequence)
                    else:
                        send_text(client, value, args.delay, args.newline_sequence)
                elif args.command == "press":
                    client.report(report)
                    wait_with_heartbeat(client, args.hold)
                    client.report(RELEASE)
                else:
                    client.request("RELEASE")
        return 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"nxhid: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

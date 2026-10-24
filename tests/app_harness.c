#include <switch.h>
#include "server.h"
#include "usb_keyboard.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int nxhid_main(void);
static const char *scenario;
static unsigned step, usb_starts, frames, sleeps;
static u64 now_ns;
static bool usb_active, sockets_active, network_active;

static bool is(const char *name) { return !strcmp(scenario, name); }

int appletGetFocusState(void) {
    bool background = (is("background-input") && step == 2) ||
        (is("background-error") && (step < 3 || step == 4));
    return background ? AppletFocusState_Background : AppletFocusState_InFocus;
}
int appletGetAppletType(void) {
    return is("applet-mode") ? AppletType_LibraryApplet : AppletType_Application;
}
Result appletSetFocusHandlingMode(int mode) {
    assert(mode == AppletFocusHandlingMode_NoSuspend);
    return is("focus-error") ? 0x1234 : 0;
}
Result appletSetAutoSleepDisabled(bool disabled) { (void)disabled; return 0; }
bool appletMainLoop(void) {
    step++;
    now_ns += 250000000;
    return step < 6;
}
void padConfigureInput(unsigned players, unsigned styles) {
    assert(players == 1 && styles == HidNpadStyleSet_NpadStandard);
}
void padInitializeDefault(PadState *pad) { (void)pad; }
void padUpdate(PadState *pad) { (void)pad; }
u64 padGetButtonsDown(const PadState *pad) {
    (void)pad;
    if (is("disconnect") && step == 2) return HidNpadButton_B;
    if (is("background-input") && step == 2) return HidNpadButton_Plus;
    return 0;
}
u64 armGetSystemTick(void) { return now_ns; }
u64 armTicksToNs(u64 ticks) { return ticks; }
Result svcSleepThread(int64_t nanoseconds) { assert(nanoseconds > 0); sleeps++; return 0; }

PrintConsole *consoleInit(PrintConsole *console) { return console; }
void consoleExit(PrintConsole *console) { (void)console; }
int ui_test_printf(const char *format, ...) {
    // libnx console printing itself touches the framebuffer, before consoleUpdate.
    assert(appletGetFocusState() == AppletFocusState_InFocus);
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}
void consoleUpdate(PrintConsole *console) {
    (void)console;
    assert(appletGetFocusState() == AppletFocusState_InFocus);
    frames++;
    putchar('\x1e'); // Separate presented frames for the Python assertions.
}

Result socketInitializeDefault(void) {
    if (is("socket-error")) return 0x1234;
    sockets_active = true;
    return 0;
}
void socketExit(void) { assert(sockets_active); sockets_active = false; }
Result nifmInitialize(int type) {
    assert(type == NifmServiceType_User);
    if (is("wifi-error")) return 0x1234;
    network_active = true;
    return 0;
}
void nifmExit(void) { assert(network_active); network_active = false; }
Result nifmGetCurrentIpAddress(u32 *address) {
    if (is("waiting-wifi")) { *address = 0; return 0; }
    assert(inet_pton(AF_INET, "192.0.2.10", address) == 1);
    return 0;
}

Result usb_keyboard_init(void) {
    usb_starts++;
    if (is("usb-error") || is("background-error") ||
            (is("restart-error") && usb_starts == 2)) return 0x1234;
    assert(!usb_active);
    usb_active = true;
    return 0;
}
void usb_keyboard_exit(void) { assert(usb_active); usb_active = false; }
Result usb_keyboard_tick(void) {
    assert(usb_active);
    return (is("reset") || is("restart-error")) && step == 2 ? 0x5678 : 0;
}
bool usb_keyboard_ready(void) { return usb_active && !is("waiting-usb"); }
uint8_t usb_keyboard_leds(void) { return usb_active ? 2 : 0; }
Result usb_keyboard_write(const uint8_t report[8]) { (void)report; assert(usb_active); return 0; }

bool server_init(Server *s, uint16_t port, ReportWriter writer, LedReader reader) {
    assert(port == NXHID_PORT);
    *s = (Server){.client = -1, .listener = -1, .write_report = writer, .read_leds = reader};
    if (is("listen-error")) { errno = EADDRINUSE; return false; }
    s->listener = 10;
    return true;
}
void server_drop(Server *s) {
    bool connected = s->client >= 0;
    s->client = -1;
    s->peer[0] = 0;
    if (connected) s->write_report((u8[8]){0});
}
void server_close(Server *s) { server_drop(s); s->listener = -1; }
void server_tick(Server *s, uint64_t now, bool enabled) {
    (void)now;
    if (!enabled) { server_drop(s); return; }
    if ((is("disconnect") && step == 2) || is("ready") || is("waiting-wifi")) return;
    s->client = 11;
    strcpy(s->peer, "192.0.2.20");
    s->reports++;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    scenario = argv[1];
    int rc = nxhid_main();
    assert(!sockets_active && !network_active && !usb_active);
    assert(frames > 0);
    if (is("background-input")) assert(step == 6);
    if (is("background-error")) assert(frames == 2 && sleeps > 0);
    return rc;
}

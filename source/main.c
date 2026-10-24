#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "server.h"
#include "usb_keyboard.h"

static Result usb_fault;

static bool write_report(const uint8_t report[8]) {
    // Never reuse a DMA buffer following a failed transfer before teardown.
    if (R_FAILED(usb_fault)) return false;
    usb_fault = usb_keyboard_write(report);
    return R_SUCCEEDED(usb_fault);
}

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    bool sockets = false, network = false, usb = false;
    Server server = {.listener = -1, .client = -1};
    char message[160] = "";
    Result rc = 0;

    if (appletGetAppletType() != AppletType_Application) {
        snprintf(message, sizeof(message), "Full application mode required. Hold R while launching a game, then open NXHID.");
        goto error;
    }
    rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    if (R_FAILED(rc)) {
        snprintf(message, sizeof(message), "Could not configure application focus.");
        goto error;
    }
    rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        snprintf(message, sizeof(message), "Could not initialize networking.");
        goto error;
    }
    sockets = true;
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        snprintf(message, sizeof(message), "Could not initialize Wi-Fi.");
        goto error;
    }
    network = true;
    rc = usb_keyboard_init();
    if (R_FAILED(rc)) {
        snprintf(message, sizeof(message), "Could not initialize the USB keyboard.");
        goto error;
    }
    usb = true;
    if (!server_init(&server, NXHID_PORT, write_report, usb_keyboard_leds)) {
        snprintf(message, sizeof(message), "Cannot listen on TCP %d: %s", NXHID_PORT, strerror(errno));
        goto error;
    }
    appletSetAutoSleepDisabled(true);
    u64 next_draw = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        u64 down = focused ? padGetButtonsDown(&pad) : 0;
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_B) {
            server_drop(&server);
            snprintf(message, sizeof(message), "Sender disconnected. Waiting for a new connection.");
        }
        if (R_SUCCEEDED(usb_fault)) usb_fault = usb_keyboard_tick();
        bool ready = R_SUCCEEDED(usb_fault) && usb_keyboard_ready();
        server_tick(&server, milliseconds(), ready && focused);
        if (R_FAILED(usb_fault)) {
            ready = false;
            server_drop(&server);
            snprintf(message, sizeof(message), "USB reset (%08x). Reconnect the sender once USB is ready.", usb_fault);
            usb_keyboard_exit();
            usb = false;
            rc = usb_keyboard_init();
            if (R_FAILED(rc)) {
                snprintf(message, sizeof(message), "Could not restart the USB keyboard.");
                goto error;
            }
            usb = true;
            usb_fault = 0;
        }
        if (server.client >= 0) message[0] = 0;
        u64 now = milliseconds();
        if (focused && now >= next_draw) {
            next_draw = now + 250;
            u32 ip = 0;
            char address[32] = "Wi-Fi not connected";
            bool has_ip = R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip;
            if (has_ip)
                inet_ntop(AF_INET, &ip, address, sizeof(address));
            const char *status = message;
            if (!*status) {
                if (!ready)
                    status = "Connect the Switch USB-C port directly to the target device.";
                else if (server.client >= 0)
                    status = "Forwarding keyboard input.";
                else if (!has_ip)
                    status = "Connect the Switch to Wi-Fi.";
                else
                    status = "Ready. Connect a sender to the Switch IP shown above.";
            }
            printf("\x1b[2J\x1b[HNXHID - Wi-Fi to USB keyboard\n\n");
            printf("Switch IP:  %s\nTCP port:   %d\n\n", address, NXHID_PORT);
            printf("USB:        %s\nSender:     %s\nReports:    %lu\n", ready ? "Keyboard ready" : "Waiting for USB host", server.client >= 0 ? server.peer : "Waiting", server.reports);
            u8 leds = usb_keyboard_leds();
            printf("Host LEDs:  Num %s  Caps %s  Scroll %s\n\n", leds & 1 ? "on" : "off", leds & 2 ? "on" : "off", leds & 4 ? "on" : "off");
            printf("B: disconnect sender    +: exit\n\n%s\n\n", status);
            printf("Computer -> Wi-Fi -> Switch -> USB -> target device\n");
            consoleUpdate(NULL);
        }
        svcSleepThread(1000000);
    }
    goto cleanup;
error:
    server_close(&server);
    if (usb) { usb_keyboard_exit(); usb = false; }
    bool error_drawn = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (appletGetFocusState() == AppletFocusState_InFocus) {
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            if (!error_drawn) {
                printf("\x1b[2J\x1b[HNXHID could not continue.\n");
                if (R_FAILED(rc)) printf("Result: %08x\n", rc);
                printf("%s\n\nPress + to exit.\n", message);
                consoleUpdate(NULL);
                error_drawn = true;
            }
        } else {
            error_drawn = false;
        }
        svcSleepThread(10000000);
    }
cleanup:
    server_close(&server);
    if (usb) usb_keyboard_exit();
    if (network) nifmExit();
    if (sockets) socketExit();
    appletSetAutoSleepDisabled(false);
    consoleExit(NULL);
    return 0;
}

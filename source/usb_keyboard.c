#include "usb_keyboard.h"
#include <string.h>

#ifndef NXHID_USB_VID
// pid.codes test identity: private testing only, not a unique production allocation.
#define NXHID_USB_VID 0x1209
#endif
#ifndef NXHID_USB_PID
#define NXHID_USB_PID 0x0001
#endif

// Boot-compatible keyboard: modifiers, reserved byte, six key usages; LED output.
static const u8 report_descriptor[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07,
    0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08,
    0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x73,
    0x05,0x07, 0x19,0x00, 0x29,0x73, 0x81,0x00, 0xC0
};
static const u8 hid_descriptor[] = {
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_REPORT,
    sizeof(report_descriptor), 0
};
static UsbDsInterface *interface;
static UsbDsEndpoint *endpoint;
static bool initialized, was_ready;
static Result fault;
static bool input_pending, release_pending, input_is_release;
static u32 input_id;
// USB DMA buffers must remain page aligned and alive until session teardown.
static u8 control_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 input_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 last_report[8], idle_rate, protocol, leds;
static u64 last_sent;

enum {
    CONTROL_TIMEOUT_NS = 500000000,
    REPORT_TIMEOUT_MS = 250,
    EVENT_POLL_NS = 1000000,
};

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
static Result io_error(void) { return MAKERESULT(Module_Libnx, LibnxError_IoError); }
static Result remember_error(Result rc) {
    if (R_FAILED(rc)) fault = rc;
    return rc;
}

// Resetting an event that is not signaled fails with InvalidState; that is not a fault.
static Result clear_event(Event *event) {
    Result rc = eventClear(event);
    return rc == KERNELRESULT(InvalidState) ? 0 : rc;
}

void usb_keyboard_exit(void) {
    if (initialized) usbDsExit(); // Cancels transfers before buffers can be reused.
    initialized = was_ready = false;
    input_pending = release_pending = input_is_release = false;
    fault = 0;
    memset(last_report, 0, sizeof(last_report));
    leds = 0;
    interface = NULL;
    endpoint = NULL;
}

Result usb_keyboard_init(void) {
    if (initialized) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    if (hosversionBefore(5, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;
    initialized = true;
#define TRY(expr) do { rc = (expr); if (R_FAILED(rc)) goto fail; } while (0)
    TRY(usbDsClearDeviceData());
    u8 language, manufacturer, product;
    const u16 langs[] = {0x0409};
    TRY(usbDsAddUsbLanguageStringDescriptor(&language, langs, 1));
    TRY(usbDsAddUsbStringDescriptor(&manufacturer, "NXHID"));
    TRY(usbDsAddUsbStringDescriptor(&product, "NXHID Wi-Fi Keyboard"));
    struct usb_device_descriptor device = {
        .bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200, .bMaxPacketSize0 = 64,
        .idVendor = NXHID_USB_VID, .idProduct = NXHID_USB_PID,
        .bcdDevice = 0x0100, .iManufacturer = manufacturer, .iProduct = product,
        .bNumConfigurations = 1
    };
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device));
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device));
    device.bcdUSB = 0x0300;
    device.bMaxPacketSize0 = 9;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device));
    const u8 bos[] = {
        5,USB_DT_BOS,22,0,2,
        7,USB_DT_DEVICE_CAPABILITY,2,2,0,0,0,
        10,USB_DT_DEVICE_CAPABILITY,3,0,0x0e,0,1,0,0,0
    };
    TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    TRY(usbDsRegisterInterfaceEx(&interface, 0));
    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0, .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID, .bInterfaceSubClass = 1, .bInterfaceProtocol = 1
    };
    struct usb_endpoint_descriptor ep = {
        .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81, .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = 8, .bInterval = 10
    };
    struct usb_ss_endpoint_companion_descriptor companion = {
        .bLength = 6, .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION, .wBytesPerInterval = 8
    };
    for (UsbDeviceSpeed speed = UsbDeviceSpeed_Full; speed <= UsbDeviceSpeed_Super; speed++) {
        // Full speed: 10 ms; high/super: 2^(7-1) microframes = 8 ms.
        ep.bInterval = speed == UsbDeviceSpeed_Full ? 10 : 7;
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &intf, sizeof(intf)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, hid_descriptor, sizeof(hid_descriptor)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &ep, sizeof(ep)));
        if (speed == UsbDeviceSpeed_Super)
            TRY(usbDsInterface_AppendConfigurationData(interface, speed, &companion, sizeof(companion)));
    }
    TRY(usbDsInterface_RegisterEndpoint(interface, &endpoint, 0x81));
    TRY(usbDsInterface_EnableInterface(interface));
    memset(last_report, 0, sizeof(last_report));
    idle_rate = leds = 0;
    protocol = 1;
    last_sent = milliseconds();
    was_ready = false;
    release_pending = true;
    TRY(usbDsEnable());
#undef TRY
    return 0;
fail:
    usb_keyboard_exit();
    return rc;
}

bool usb_keyboard_ready(void) {
    UsbState state;
    return initialized && R_SUCCEEDED(fault) && was_ready && !release_pending &&
        R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured;
}

uint8_t usb_keyboard_leds(void) { return leds; }

// Each control stage is bounded; the public entry points latch failures until exit.
static Result control_stage(bool in, size_t size) {
    Event *event = in ? &interface->CtrlInCompletionEvent : &interface->CtrlOutCompletionEvent;
    Result rc = clear_event(event);
    if (R_FAILED(rc)) return rc;
    u32 id, transferred;
    rc = in ? usbDsInterface_CtrlInPostBufferAsync(interface, control_buffer, size, &id)
            : usbDsInterface_CtrlOutPostBufferAsync(interface, control_buffer, size, &id);
    if (R_FAILED(rc)) return rc;
    rc = eventWait(event, CONTROL_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;
    rc = clear_event(event);
    if (R_FAILED(rc)) return rc;
    UsbDsReportData reports;
    rc = in ? usbDsInterface_GetCtrlInReportData(interface, &reports)
            : usbDsInterface_GetCtrlOutReportData(interface, &reports);
    if (R_FAILED(rc)) return rc;
    rc = usbDsParseReportData(&reports, id, NULL, &transferred);
    if (R_FAILED(rc)) return rc;
    if (!in && size) armDCacheFlush(control_buffer, size);
    return transferred == size ? 0 : io_error();
}

static Result control_reply(const void *data, size_t size, u16 requested) {
    if (!requested) return usbDsInterface_StallCtrl(interface);
    if (size > requested) size = requested;
    memcpy(control_buffer, data, size);
    Result rc = control_stage(true, size);
    return R_FAILED(rc) ? rc : control_stage(false, 0);
}

static Result service_setup(void) {
    Result rc = eventWait(&interface->SetupEvent, 0);
    if (rc == KERNELRESULT(TimedOut)) return 0;
    if (R_FAILED(rc)) return rc;
    rc = clear_event(&interface->SetupEvent);
    if (R_FAILED(rc)) return rc;
    struct usb_control_setup s;
    rc = usbDsInterface_GetSetupPacket(interface, &s, sizeof(s));
    if (R_FAILED(rc)) return rc;
    if (s.wIndex != interface->interface_index) return usbDsInterface_StallCtrl(interface);
    const u8 zero[2] = {0};
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_STATUS && s.wValue == 0 && s.wLength == 2)
        return control_reply(zero, 2, s.wLength);
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_INTERFACE && s.wValue == 0 && s.wLength == 1)
        return control_reply(zero, 1, s.wLength);
    if (s.bmRequestType == 0x01 && s.bRequest == USB_REQUEST_SET_INTERFACE && s.wValue == 0 && s.wLength == 0)
        return control_stage(true, 0);
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_DESCRIPTOR && !(s.wValue & 0xff)) {
        if ((s.wValue >> 8) == USB_DT_REPORT)
            return control_reply(report_descriptor, sizeof(report_descriptor), s.wLength);
        if ((s.wValue >> 8) == USB_DT_HID)
            return control_reply(hid_descriptor, sizeof(hid_descriptor), s.wLength);
    }
    if (s.bmRequestType == 0xA1) {
        if (s.bRequest == 1 && s.wValue == 0x0100) // GET_REPORT input
            return control_reply(last_report, sizeof(last_report), s.wLength);
        if (s.bRequest == 1 && s.wValue == 0x0200) // GET_REPORT LEDs
            return control_reply(&leds, 1, s.wLength);
        if (s.bRequest == 2 && s.wValue == 0 && s.wLength == 1) // GET_IDLE
            return control_reply(&idle_rate, 1, s.wLength);
        if (s.bRequest == 3 && s.wValue == 0 && s.wLength == 1) // GET_PROTOCOL
            return control_reply(&protocol, 1, s.wLength);
    }
    if (s.bmRequestType == 0x21) {
        if (s.bRequest == 9 && s.wValue == 0x0200 && s.wLength == 1) { // SET_REPORT LEDs
            rc = control_stage(false, 1);
            if (R_FAILED(rc)) return rc;
            leds = control_buffer[0] & 0x1f;
            return control_stage(true, 0);
        }
        if (s.bRequest == 10 && !(s.wValue & 0xff) && s.wLength == 0) { // SET_IDLE
            idle_rate = s.wValue >> 8;
            return control_stage(true, 0);
        }
        if (s.bRequest == 11 && s.wValue <= 1 && s.wLength == 0) { // SET_PROTOCOL
            protocol = s.wValue;
            return control_stage(true, 0);
        }
    }
    return usbDsInterface_StallCtrl(interface);
}

static Result read_state(UsbState *state) {
    Result rc = usbDsGetState(state);
    if (R_FAILED(rc)) return rc;
    if (*state == UsbState_Suspended) {
        // Keep the USB session and host LED/protocol state across selective suspend.
        // Drop held keys and deliver a neutral report before accepting a new sender.
        memset(last_report, 0, sizeof(last_report));
        release_pending = true;
    } else if (*state == UsbState_Configured) {
        was_ready = true;
    } else if (was_ready) {
        return io_error(); // A reset or unplug needs a fresh session, unlike suspend.
    }
    return 0;
}

static Result start_input(const u8 report[8], bool release) {
    // No writes to this DMA buffer until its matching completion has been consumed.
    if (input_pending) return io_error();
    Result rc = clear_event(&endpoint->CompletionEvent);
    if (R_FAILED(rc)) return rc;
    memcpy(input_buffer, report, 8);
    rc = usbDsEndpoint_PostBufferAsync(endpoint, input_buffer, 8, &input_id);
    if (R_FAILED(rc)) return rc;
    input_pending = true;
    input_is_release = release;
    return 0;
}

static Result poll_input(u64 timeout) {
    if (!input_pending) return 0;
    Result rc = eventWait(&endpoint->CompletionEvent, timeout);
    if (rc == KERNELRESULT(TimedOut)) return 0;
    if (R_FAILED(rc)) return rc;
    rc = clear_event(&endpoint->CompletionEvent);
    if (R_FAILED(rc)) return rc;
    UsbDsReportData reports;
    rc = usbDsEndpoint_GetReportData(endpoint, &reports);
    if (R_FAILED(rc)) return rc;
    u32 transferred;
    rc = usbDsParseReportData(&reports, input_id, NULL, &transferred);
    if (rc == MAKERESULT(Module_Libnx, LibnxError_NotFound)) return 0;
    if (R_FAILED(rc)) return rc;
    if (transferred != 8) return io_error();
    input_pending = false;
    if (input_is_release) release_pending = false;
    last_sent = milliseconds();
    return 0;
}

static Result wait_input(void) {
    u64 deadline = milliseconds() + REPORT_TIMEOUT_MS;
    while (input_pending) {
        Result rc = poll_input(EVENT_POLL_NS);
        if (R_FAILED(rc)) return rc;
        UsbState state;
        rc = read_state(&state);
        if (R_FAILED(rc)) return rc;
        if (state != UsbState_Configured) return io_error();
        if (!input_pending) return 0;
        if (milliseconds() >= deadline) return KERNELRESULT(TimedOut);
        rc = service_setup();
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

static Result write_report(const uint8_t report[8]) {
    UsbState state;
    Result rc = read_state(&state);
    if (R_FAILED(rc)) return rc;
    bool release = !memcmp(report, (const u8[8]){0}, 8);
    // server_drop releases keys while suspended; send that release after resume.
    if (state == UsbState_Suspended && release) return 0;
    if (state != UsbState_Configured) return io_error();
    rc = wait_input();
    if (R_FAILED(rc)) return rc;
    if (release_pending && !release) {
        rc = start_input((const u8[8]){0}, true);
        if (R_FAILED(rc)) return rc;
        rc = wait_input();
        if (R_FAILED(rc)) return rc;
    }
    rc = start_input(report, release);
    if (R_FAILED(rc)) return rc;
    rc = wait_input();
    if (R_FAILED(rc)) return rc;
    memmove(last_report, report, 8);
    return 0;
}

Result usb_keyboard_write(const uint8_t report[8]) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(fault)) return fault;
    return remember_error(write_report(report));
}

static Result tick(void) {
    UsbState state;
    Result rc = read_state(&state);
    if (R_FAILED(rc) || state == UsbState_Suspended) return rc;
    rc = service_setup();
    if (R_FAILED(rc)) return rc;
    rc = read_state(&state);
    if (R_FAILED(rc) || state != UsbState_Configured) return rc;
    rc = poll_input(0);
    if (R_FAILED(rc) || input_pending) return rc;
    if (release_pending) return start_input((const u8[8]){0}, true);
    // A configured host may stop polling while loading its HID driver or idling.
    // Keep one transfer pending without blocking control requests or resetting USB.
    if (idle_rate && milliseconds() - last_sent >= (u64)idle_rate * 4)
        return start_input(last_report, false);
    return 0;
}

Result usb_keyboard_tick(void) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(fault)) return fault;
    return remember_error(tick());
}

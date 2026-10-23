#include "usb_keyboard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UsbDsInterface intf;
static UsbDsEndpoint endpoint;
static UsbState state;
static Result state_error;
static u64 now_ns;
static u32 next_id;
static unsigned writes, stalls;
static bool suppress_completion, short_write, active, old_firmware;
static bool suppress_control, short_control, key_owned;
static unsigned init_calls, exits, init_step, fail_init_step, cache_flushes;
static u8 *key_buffer;
static Result key_status = 3;
static Result post_error, report_error;
static u64 complete_at, suspend_at, detach_at;
static struct usb_control_setup setup;
static struct usb_device_descriptor devices[5];
static u8 configurations[5][64], input[8], control_in[64], led_output;
static size_t config_sizes[5], control_size;
static UsbDsReportData in_report, out_report, key_report;

bool hosversionBefore(int major, int minor, int micro) {
    assert(major == 5 && minor == 0 && micro == 0);
    return old_firmware;
}
u64 armGetSystemTick(void) { return now_ns; }
u64 armTicksToNs(u64 ticks) { return ticks; }
void armDCacheFlush(void *buffer, size_t size) {
    assert((uintptr_t)buffer % 0x1000 == 0 && size == 1);
    cache_flushes++;
}
Result eventClear(Event *event) {
    bool was_signaled = event->signaled;
    event->signaled = false;
    return was_signaled ? 0 : KERNELRESULT(InvalidState); // Like svcResetSignal.
}
Result eventWait(Event *event, u64 timeout) {
    if (key_owned) assert(!memcmp(key_buffer, input, sizeof(input)));
    if (event->error) return event->error;
    if (event->signaled) return 0;
    now_ns += timeout;
    if (suspend_at && now_ns >= suspend_at) state = UsbState_Suspended;
    if (detach_at && now_ns >= detach_at) state = UsbState_Detached;
    if (event == &endpoint.CompletionEvent && complete_at && now_ns >= complete_at) {
        event->signaled = true;
        return 0;
    }
    return KERNELRESULT(TimedOut);
}
static Result init_result(void) { return ++init_step == fail_init_step ? 999 : 0; }
Result usbDsInitialize(void) {
    init_calls++;
    Result rc = init_result();
    if (R_SUCCEEDED(rc)) { assert(!active); active = true; }
    return rc;
}
void usbDsExit(void) {
    if (key_owned) assert(!memcmp(key_buffer, input, sizeof(input)));
    active = key_owned = false;
    exits++;
}
Result usbDsClearDeviceData(void) { memset(config_sizes, 0, sizeof(config_sizes)); return init_result(); }
Result usbDsAddUsbLanguageStringDescriptor(u8 *index, const u16 *languages, u16 count) {
    assert(count == 1 && languages[0] == 0x0409);
    *index = 0;
    return init_result();
}
Result usbDsAddUsbStringDescriptor(u8 *index, const char *text) { assert(*text); *index = 1; return init_result(); }
Result usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed speed, struct usb_device_descriptor *descriptor) {
    devices[speed] = *descriptor;
    return init_result();
}
Result usbDsSetBinaryObjectStore(const void *data, size_t size) {
    const u8 *bytes = data;
    assert(size == bytes[2] && bytes[1] == USB_DT_BOS);
    assert(bytes[4] == 2 && bytes[16] == 0x0e);
    assert(bytes[18] <= 3 && (bytes[16] & (1u << bytes[18])));
    return init_result();
}
Result usbDsRegisterInterfaceEx(UsbDsInterface **out, u8 index) {
    memset(&intf, 0, sizeof(intf));
    intf.interface_index = index;
    *out = &intf;
    return init_result();
}
Result usbDsInterface_AppendConfigurationData(UsbDsInterface *interface, UsbDeviceSpeed speed, const void *data, size_t size) {
    assert(interface == &intf && config_sizes[speed] + size <= sizeof(configurations[speed]));
    memcpy(configurations[speed] + config_sizes[speed], data, size);
    config_sizes[speed] += size;
    return init_result();
}
Result usbDsInterface_RegisterEndpoint(UsbDsInterface *interface, UsbDsEndpoint **out, u8 address) {
    assert(interface == &intf && address == 0x81);
    memset(&endpoint, 0, sizeof(endpoint));
    *out = &endpoint;
    return init_result();
}
Result usbDsInterface_EnableInterface(UsbDsInterface *interface) { assert(interface == &intf); return init_result(); }
Result usbDsEnable(void) { assert(active); return init_result(); }
Result usbDsGetState(UsbState *out) { *out = state; return state_error; }

static void post(Event *event, void *data, size_t size, u32 *id, UsbDsReportData *report) {
    assert((uintptr_t)data % 0x1000 == 0);
    *id = ++next_id;
    *report = (UsbDsReportData){.report_count = 1, .report = {{*id, size, size, 3}}};
    event->signaled = true;
}
Result usbDsInterface_CtrlInPostBufferAsync(UsbDsInterface *interface, void *data, size_t size, u32 *id) {
    assert(size <= sizeof(control_in));
    if (post_error) return post_error;
    if (size) {
        memcpy(control_in, data, size);
        control_size = size;
    }
    post(&interface->CtrlInCompletionEvent, data, size, id, &in_report);
    if (short_control && size) in_report.report[0].transferredSize--;
    if (suppress_control) interface->CtrlInCompletionEvent.signaled = false;
    return 0;
}
Result usbDsInterface_CtrlOutPostBufferAsync(UsbDsInterface *interface, void *data, size_t size, u32 *id) {
    assert(size <= 1);
    if (post_error) return post_error;
    if (size) *(u8 *)data = led_output;
    post(&interface->CtrlOutCompletionEvent, data, size, id, &out_report);
    if (short_control && size) out_report.report[0].transferredSize--;
    if (suppress_control) interface->CtrlOutCompletionEvent.signaled = false;
    return 0;
}
Result usbDsInterface_GetCtrlInReportData(UsbDsInterface *interface, UsbDsReportData *out) {
    assert(interface == &intf); *out = in_report; return report_error;
}
Result usbDsInterface_GetCtrlOutReportData(UsbDsInterface *interface, UsbDsReportData *out) {
    assert(interface == &intf); *out = out_report; return report_error;
}
Result usbDsParseReportData(UsbDsReportData *report, u32 id, u32 *requested, u32 *transferred) {
    for (u32 i = 0; i < report->report_count && i < 8; i++) {
        const UsbDsReportEntry *entry = &report->report[i];
        if (entry->id != id) continue;
        if (entry->urb_status != 3) return 904;
        if (requested) *requested = entry->requestedSize;
        if (transferred) *transferred = entry->transferredSize;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}
Result usbDsInterface_StallCtrl(UsbDsInterface *interface) { assert(interface == &intf); stalls++; return 0; }
Result usbDsInterface_GetSetupPacket(UsbDsInterface *interface, void *data, size_t size) {
    assert(interface == &intf && size == sizeof(setup));
    memcpy(data, &setup, size);
    return 0;
}
Result usbDsEndpoint_PostBufferAsync(UsbDsEndpoint *ep, void *data, size_t size, u32 *id) {
    assert(ep == &endpoint && size == sizeof(input));
    assert(!key_owned);
    if (post_error) return post_error;
    memcpy(input, data, size);
    key_buffer = data;
    key_owned = true;
    writes++;
    post(&ep->CompletionEvent, data, size, id, &key_report);
    key_report.report[0].urb_status = key_status;
    if (short_write) key_report.report[0].transferredSize--;
    if (suppress_completion) ep->CompletionEvent.signaled = false;
    return 0;
}
Result usbDsEndpoint_GetReportData(UsbDsEndpoint *ep, UsbDsReportData *out) {
    assert(ep == &endpoint && key_owned);
    assert(!memcmp(key_buffer, input, sizeof(input)));
    if (report_error) return report_error;
    *out = key_report;
    if (key_report.report_count) key_owned = false;
    return 0;
}

static Result request(u8 type, u8 code, u16 value, u16 length) {
    setup = (struct usb_control_setup){type, code, value, 0, length};
    intf.SetupEvent.signaled = true;
    return usb_keyboard_tick();
}

static void restart(void) {
    usb_keyboard_exit();
    state = UsbState_Address;
    state_error = post_error = report_error = 0;
    suppress_completion = short_write = suppress_control = short_control = false;
    complete_at = suspend_at = detach_at = now_ns = 0;
    key_status = 3;
    init_step = fail_init_step = 0;
    assert(usb_keyboard_init() == 0);
}

static void connect_host(void) {
    state = UsbState_Configured;
    assert(usb_keyboard_tick() == 0);
    assert(!usb_keyboard_ready());
    assert(usb_keyboard_tick() == 0);
    assert(usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
}

static void assert_fault(Result expected) {
    unsigned before = writes;
    assert(usb_keyboard_tick() == expected);
    assert(usb_keyboard_write((u8[8]){2, 0, 5}) == expected);
    assert(writes == before && !usb_keyboard_ready());
}

static void descriptors(void) {
    for (int speed = 2; speed <= 4; speed++) {
        assert(devices[speed].bNumConfigurations == 1);
        assert(devices[speed].bLength == 18);
        assert(config_sizes[speed] == (speed == 4 ? 31 : 25));
        const u8 *config = configurations[speed];
        assert(config[4] == 1 && config[5] == 3 && config[6] == 1 && config[7] == 1);
        assert(config[10] == USB_DT_HID && config[15] == USB_DT_REPORT);
        assert(config[20] == 0x81 && config[21] == USB_TRANSFER_TYPE_INTERRUPT);
        assert(config[22] == 8 && config[24] == (speed == 2 ? 10 : 7));
        assert(devices[speed].bcdUSB == (speed == 4 ? 0x0300 : 0x0200));
        assert(devices[speed].bMaxPacketSize0 == (speed == 4 ? 9 : 64));
        if (speed == 4) {
            assert(config[25] == 6 && config[26] == USB_DT_SS_ENDPOINT_COMPANION);
            assert(config[27] == 0 && config[28] == 0 && config[29] == 8 && config[30] == 0);
        }
    }
    assert(devices[4].bMaxPacketSize0 == 9);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 255) == 0);
    assert(control_size == configurations[2][16]);
    assert(control_in[0] == 5 && control_in[control_size - 1] == 0xC0);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 8) == 0);
    assert(control_size == 8);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 0) == 0 && stalls == 1);
    assert(request(0x81, 6, USB_DT_HID << 8, 255) == 0 && control_size == 9);
    assert(!memcmp(control_in, configurations[2] + 9, 9));
    assert(request(0x81, USB_REQUEST_GET_STATUS, 0, 2) == 0 && control_size == 2);
    assert(control_in[0] == 0 && control_in[1] == 0);
    assert(request(0x81, USB_REQUEST_GET_INTERFACE, 0, 1) == 0 && control_in[0] == 0);
    assert(request(0x01, USB_REQUEST_SET_INTERFACE, 0, 0) == 0);
    assert(request(0xA1, 2, 0, 2) == 0 && stalls == 2);
    assert(request(0xA1, 3, 0, 2) == 0 && stalls == 3);
    setup.wIndex = 1;
    intf.SetupEvent.signaled = true;
    assert(usb_keyboard_tick() == 0 && stalls == 4);
}
static void controls(void) {
    led_output = 0xE7;
    assert(request(0x21, 9, 0x0200, 1) == 0);
    assert(usb_keyboard_leds() == 7 && cache_flushes == 1);
    assert(request(0xA1, 1, 0x0200, 1) == 0 && control_in[0] == 7);
    assert(request(0x21, 11, 0, 0) == 0);
    assert(request(0xA1, 3, 0, 1) == 0 && control_in[0] == 0);
    assert(request(0x21, 10, 0x0300, 0) == 0);
    assert(request(0xA1, 2, 0, 1) == 0 && control_in[0] == 3);
    assert(request(0x21, 9, 0x0200, 4096) == 0 && stalls == 1);
    connect_host();
    assert(usb_keyboard_write((u8[8]){2, 0, 4}) == 0);
    assert(request(0xA1, 1, 0x0100, 8) == 0 && control_in[0] == 2 && control_in[2] == 4);
    unsigned before = writes;
    now_ns += 12000000;
    assert(usb_keyboard_tick() == 0 && writes == before + 1);
    state = UsbState_Detached;
    assert(R_FAILED(usb_keyboard_tick()));
    restart();
    assert(usb_keyboard_leds() == 0);
    connect_host();
}
static void errors(void) {
    state_error = 900;
    assert(usb_keyboard_tick() == 900);
    assert(usb_keyboard_write((u8[8]){0}) == 900);
    assert_fault(900);
    restart();
    intf.SetupEvent.error = 901;
    assert(usb_keyboard_tick() == 901);
    assert_fault(901);
    restart();
    intf.CtrlInCompletionEvent.error = 902;
    assert(request(0x81, 6, USB_DT_REPORT << 8, 8) == 902);
    assert_fault(902);
    restart();
    state = UsbState_Configured;
    endpoint.CompletionEvent.error = 903;
    assert(usb_keyboard_write((u8[8]){0}) == 903);
    assert(now_ns == 0); // An IPC failure must not be retried as a timeout.
    assert_fault(903);
    restart();
    state = UsbState_Configured;
    short_write = true;
    Result rc = usb_keyboard_write((u8[8]){0});
    assert(R_FAILED(rc));
    assert_fault(rc);
    restart();
    state = UsbState_Configured;
    suppress_completion = true;
    assert(usb_keyboard_write((u8[8]){0}) == KERNELRESULT(TimedOut));
    assert(now_ns == 250000000);
    assert_fault(KERNELRESULT(TimedOut));
    restart();
    state = UsbState_Configured;
    key_status = 5; // A signaled event with a failed URB is not successful delivery.
    assert(usb_keyboard_write((u8[8]){0}) == 904);
    assert_fault(904);
    restart();
    post_error = 905;
    state = UsbState_Configured;
    assert(usb_keyboard_write((u8[8]){0}) == 905);
    assert_fault(905);
    restart();
    report_error = 906;
    state = UsbState_Configured;
    assert(usb_keyboard_write((u8[8]){0}) == 906);
    assert_fault(906);
}

static void enumeration(void) {
    suppress_completion = true;
    state = UsbState_Configured;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    unsigned before = writes;
    now_ns += 5000000000; // Driver binding may take much longer than a keystroke.
    assert(usb_keyboard_tick() == 0 && writes == before);
    assert(!usb_keyboard_ready() && active);
    assert(request(0x81, 6, USB_DT_REPORT << 8, 255) == 0 && control_size > 8);
    led_output = 2;
    assert(request(0x21, 9, 0x0200, 1) == 0 && usb_keyboard_leds() == 2);
    assert(writes == before);
    endpoint.CompletionEvent.signaled = true;
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
}

static void idle_polling(void) {
    connect_host();
    assert(usb_keyboard_write((u8[8]){2, 0, 4}) == 0);
    assert(request(0x21, 10, 0x0300, 0) == 0);
    suppress_completion = true;
    unsigned before = writes;
    now_ns += 12000000;
    assert(usb_keyboard_tick() == 0 && writes == before + 1);
    assert(input[0] == 2 && input[2] == 4);
    now_ns += 5000000000;
    assert(usb_keyboard_tick() == 0 && writes == before + 1 && usb_keyboard_ready());
    assert(request(0x21, 10, 0, 0) == 0); // Disabling idle adds no more reports.
    endpoint.CompletionEvent.signaled = true;
    assert(usb_keyboard_tick() == 0 && writes == before + 1);
    suppress_completion = false;
    assert(usb_keyboard_write((u8[8]){0}) == 0);
    now_ns += 5000000000;
    assert(usb_keyboard_tick() == 0 && writes == before + 2);
}

static void suspend_resume(bool pending) {
    connect_host();
    assert(usb_keyboard_write((u8[8]){2, 0, 4}) == 0);
    led_output = 2;
    assert(request(0x21, 9, 0x0200, 1) == 0);
    assert(request(0x21, 11, 0, 0) == 0);
    assert(request(0x21, 10, 0x0300, 0) == 0);
    if (pending) {
        suppress_completion = true;
        now_ns += 12000000;
        assert(usb_keyboard_tick() == 0 && key_owned);
    }
    unsigned before = writes, exits_before = exits;
    state = UsbState_Suspended;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    assert(usb_keyboard_write((u8[8]){0}) == 0);
    now_ns += 5000000000;
    assert(usb_keyboard_tick() == 0 && writes == before && exits == exits_before);
    state = UsbState_Configured;
    if (pending) endpoint.CompletionEvent.signaled = true;
    suppress_completion = false;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    assert(!memcmp(input, (u8[8]){0}, 8));
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready() && exits == exits_before);
    assert(usb_keyboard_leds() == 2);
    assert(request(0xA1, 1, 0x0100, 8) == 0 && !memcmp(control_in, (u8[8]){0}, 8));
    assert(request(0xA1, 3, 0, 1) == 0 && control_in[0] == 0);
    assert(request(0xA1, 2, 0, 1) == 0 && control_in[0] == 3);
}

static void transfers(void) {
    connect_host();
    suppress_completion = true;
    complete_at = 200000000;
    setup = (struct usb_control_setup){0x21, 9, 0x0200, 0, 1};
    led_output = 2;
    intf.SetupEvent.signaled = true;
    assert(usb_keyboard_write((u8[8]){2, 0, 4}) == 0);
    assert(now_ns == complete_at && usb_keyboard_leds() == 2);
    restart();
    connect_host();
    suppress_completion = true;
    suspend_at = 2000000;
    assert(R_FAILED(usb_keyboard_write((u8[8]){2, 0, 4})));
    assert(now_ns == suspend_at);
    restart();
    connect_host();
    suppress_completion = true;
    detach_at = 3000000;
    assert(R_FAILED(usb_keyboard_write((u8[8]){2, 0, 4})));
    assert(now_ns == detach_at);
    restart();
    suppress_completion = true;
    state = UsbState_Configured;
    assert(usb_keyboard_tick() == 0);
    UsbDsReportData saved = key_report;
    key_report.report_count = 0; // A stale event is not completion of this URB.
    endpoint.CompletionEvent.signaled = true;
    assert(usb_keyboard_tick() == 0 && !usb_keyboard_ready());
    key_report = saved;
    endpoint.CompletionEvent.signaled = true;
    assert(usb_keyboard_tick() == 0 && usb_keyboard_ready());
}

static void control_errors(void) {
    suppress_control = true;
    assert(request(0x81, 6, USB_DT_REPORT << 8, 8) == KERNELRESULT(TimedOut));
    assert(now_ns == 500000000);
    assert_fault(KERNELRESULT(TimedOut));
    restart();
    short_control = true;
    Result rc = request(0x21, 9, 0x0200, 1);
    assert(R_FAILED(rc) && usb_keyboard_leds() == 0);
    assert_fault(rc);
    restart();
    short_control = true;
    rc = request(0x81, 6, USB_DT_REPORT << 8, 8);
    assert(R_FAILED(rc));
    assert_fault(rc);
    restart();
    intf.CtrlOutCompletionEvent.error = 907;
    assert(request(0x21, 9, 0x0200, 1) == 907);
    assert_fault(907);
}

static void lifecycle(void) {
    unsigned steps = init_step, before = init_calls;
    assert(usb_keyboard_init() == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized));
    assert(init_calls == before && active);
    usb_keyboard_exit();
    before = exits;
    usb_keyboard_exit();
    assert(exits == before); // Exit is idempotent.
    for (unsigned step = 1; step <= steps; step++) {
        init_step = 0;
        fail_init_step = step;
        assert(usb_keyboard_init() == 999);
        assert(!active && !usb_keyboard_ready());
        assert(R_FAILED(usb_keyboard_tick()));
        usb_keyboard_exit();
    }
    fail_init_step = 0;
    old_firmware = true;
    before = init_calls;
    assert(usb_keyboard_init() == MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer));
    assert(init_calls == before);
    old_firmware = false;
    restart();
    connect_host();
    state = UsbState_Default; // Host bus reset, with no physical cable removal.
    assert(R_FAILED(usb_keyboard_tick()));
    restart();
    connect_host();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(R_FAILED(usb_keyboard_tick()));
    assert(R_FAILED(usb_keyboard_write((u8[8]){0})));
    assert(usb_keyboard_init() == 0);
    if (!strcmp(argv[1], "descriptors")) descriptors();
    else if (!strcmp(argv[1], "controls")) controls();
    else if (!strcmp(argv[1], "errors")) errors();
    else if (!strcmp(argv[1], "enumeration")) enumeration();
    else if (!strcmp(argv[1], "idle")) idle_polling();
    else if (!strcmp(argv[1], "suspend")) suspend_resume(false);
    else if (!strcmp(argv[1], "suspend-pending")) suspend_resume(true);
    else if (!strcmp(argv[1], "transfers")) transfers();
    else if (!strcmp(argv[1], "control-errors")) control_errors();
    else if (!strcmp(argv[1], "lifecycle")) lifecycle();
    else return 2;
    usb_keyboard_exit();
    assert(!active && !usb_keyboard_ready());
    return 0;
}

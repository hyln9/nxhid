#pragma once
// Minimal libnx boundary for running the real USB backend in host tests.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u32 Result;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
#define MAKERESULT(module, error) ((Result)((module) * 100 + (error)))
#define KERNELRESULT(error) MAKERESULT(1, error)
enum { Module_Libnx = 2, LibnxError_IoError = 1, LibnxError_IncompatSysVer,
       LibnxError_NotInitialized, LibnxError_AlreadyInitialized, LibnxError_NotFound, TimedOut = 10, InvalidState = 11 };
enum { USB_DT_DEVICE_SIZE = 18, USB_DT_INTERFACE_SIZE = 9, USB_DT_ENDPOINT_SIZE = 7 };
enum { USB_DT_DEVICE = 1, USB_DT_INTERFACE = 4, USB_DT_ENDPOINT = 5,
       USB_DT_BOS = 15, USB_DT_DEVICE_CAPABILITY = 16, USB_DT_HID = 33,
       USB_DT_REPORT = 34, USB_DT_SS_ENDPOINT_COMPANION = 48 };
enum { USB_CLASS_HID = 3, USB_TRANSFER_TYPE_INTERRUPT = 3 };
enum { USB_REQUEST_GET_STATUS = 0, USB_REQUEST_GET_DESCRIPTOR = 6,
       USB_REQUEST_GET_INTERFACE = 10, USB_REQUEST_SET_INTERFACE = 11 };
typedef enum { UsbDeviceSpeed_Full = 2, UsbDeviceSpeed_High, UsbDeviceSpeed_Super } UsbDeviceSpeed;
typedef enum { UsbState_Detached, UsbState_Attached, UsbState_Powered, UsbState_Default,
               UsbState_Address, UsbState_Configured, UsbState_Suspended } UsbState;
typedef struct { bool signaled; Result error; } Event;
typedef struct {
    u8 interface_index;
    Event SetupEvent, CtrlInCompletionEvent, CtrlOutCompletionEvent;
} UsbDsInterface;
typedef struct { Event CompletionEvent; } UsbDsEndpoint;
typedef struct { u32 id, requestedSize, transferredSize, urb_status; } UsbDsReportEntry;
typedef struct { UsbDsReportEntry report[8]; u32 report_count; } UsbDsReportData;
struct usb_device_descriptor {
    u8 bLength, bDescriptorType;
    u16 bcdUSB;
    u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct, bcdDevice;
    u8 iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __attribute__((packed));
struct usb_interface_descriptor {
    u8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
};
struct usb_endpoint_descriptor {
    u8 bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8 bInterval;
} __attribute__((packed));
struct usb_ss_endpoint_companion_descriptor {
    u8 bLength, bDescriptorType, bMaxBurst, bmAttributes;
    u16 wBytesPerInterval;
} __attribute__((packed));
struct usb_control_setup { u8 bmRequestType, bRequest; u16 wValue, wIndex, wLength; };

bool hosversionBefore(int major, int minor, int micro);
u64 armGetSystemTick(void);
u64 armTicksToNs(u64 ticks);
void armDCacheFlush(void *buffer, size_t size);
Result eventClear(Event *event);
Result eventWait(Event *event, u64 timeout);
Result usbDsInitialize(void);
void usbDsExit(void);
Result usbDsClearDeviceData(void);
Result usbDsAddUsbLanguageStringDescriptor(u8 *index, const u16 *languages, u16 count);
Result usbDsAddUsbStringDescriptor(u8 *index, const char *text);
Result usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed speed, struct usb_device_descriptor *descriptor);
Result usbDsSetBinaryObjectStore(const void *data, size_t size);
Result usbDsRegisterInterfaceEx(UsbDsInterface **out, u8 index);
Result usbDsInterface_AppendConfigurationData(UsbDsInterface *intf, UsbDeviceSpeed speed, const void *data, size_t size);
Result usbDsInterface_RegisterEndpoint(UsbDsInterface *intf, UsbDsEndpoint **out, u8 address);
Result usbDsInterface_EnableInterface(UsbDsInterface *intf);
Result usbDsEnable(void);
Result usbDsGetState(UsbState *state);
Result usbDsInterface_CtrlInPostBufferAsync(UsbDsInterface *intf, void *data, size_t size, u32 *id);
Result usbDsInterface_CtrlOutPostBufferAsync(UsbDsInterface *intf, void *data, size_t size, u32 *id);
Result usbDsInterface_GetCtrlInReportData(UsbDsInterface *intf, UsbDsReportData *reports);
Result usbDsInterface_GetCtrlOutReportData(UsbDsInterface *intf, UsbDsReportData *reports);
Result usbDsParseReportData(UsbDsReportData *reports, u32 id, u32 *requested, u32 *transferred);
Result usbDsInterface_StallCtrl(UsbDsInterface *intf);
Result usbDsInterface_GetSetupPacket(UsbDsInterface *intf, void *data, size_t size);
Result usbDsEndpoint_PostBufferAsync(UsbDsEndpoint *ep, void *data, size_t size, u32 *id);
Result usbDsEndpoint_GetReportData(UsbDsEndpoint *ep, UsbDsReportData *reports);

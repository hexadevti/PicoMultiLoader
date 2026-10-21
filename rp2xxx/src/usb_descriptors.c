// usb_descriptors.c - USB device, configuration and string descriptors for USB drive mode.
// Laid out like TinyUSB's examples/device/cdc_msc/src/usb_descriptors.c (MIT, Ha Thach; see THIRD-PARTY.md).
#include <string.h>
#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0xCAFE // TinyUSB's test VID: fine for a hobby device, not for products
#define USB_PID 0x4003

static const tusb_desc_device_t kDevice = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&kDevice;
}

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t kConfig[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(0, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return kConfig;
}

static const char *const kStrings[] = {
    NULL, // language, handled below
    "PicoMultiLoader",
    "PicoCalc SD card",
    NULL, // serial: the flash/chip unique ID
};

static uint16_t s_desc[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *s;
    size_t n;
    if (index == 0) {
        s_desc[1] = 0x0409; // English
        n = 1;
    } else {
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            s = serial;
        } else if (index < sizeof(kStrings) / sizeof(kStrings[0])) {
            s = kStrings[index];
        } else {
            return NULL;
        }
        n = strlen(s);
        if (n > 31) {
            n = 31;
        }
        for (size_t i = 0; i < n; i++) {
            s_desc[1 + i] = (uint8_t)s[i];
        }
    }
    s_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return s_desc;
}

// usb_link.c - TinyUSB host stack + a raw byte pipe to the cart's CDC.
//
// The stack is IRQ-driven; usb_link_pump() only drains the event queue and runs
// enumeration callbacks, so it is cheap and safe to call anywhere.

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/usb.h"
#include "tusb.h"
#include "usb_link.h"

static usb_link_state_t st;
static bool inited;

// Descriptor strings arrive UTF-16LE behind a 2-byte header.
static void desc_str_to_ascii(const uint16_t *src, char *dst, size_t dst_len)
{
    const uint8_t *b = (const uint8_t *)src;
    size_t n = (b[0] > 2) ? (size_t)(b[0] - 2) / 2 : 0;
    if (n > dst_len - 1) n = dst_len - 1;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = src[1 + i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[n] = 0;
}

static void fetch_string(uint8_t daddr, uint8_t which, char *dst, size_t dst_len)
{
    static CFG_TUH_MEM_ALIGN uint16_t buf[64];
    uint8_t rc;

    dst[0] = 0;
    memset(buf, 0, sizeof(buf));
    switch (which) {
    case 0:  rc = tuh_descriptor_get_manufacturer_string_sync(daddr, 0x0409, buf, sizeof(buf)); break;
    case 1:  rc = tuh_descriptor_get_product_string_sync(daddr, 0x0409, buf, sizeof(buf)); break;
    default: rc = tuh_descriptor_get_serial_string_sync(daddr, 0x0409, buf, sizeof(buf)); break;
    }
    if (rc == XFER_RESULT_SUCCESS) desc_str_to_ascii(buf, dst, dst_len);
}

// Walk the configuration for the CDC data interface's bulk endpoints, so the
// console can show the packet size the throughput numbers have to live within.
static void fetch_bulk_mps(uint8_t daddr)
{
    static CFG_TUH_MEM_ALIGN uint8_t cfg[CFG_TUH_ENUMERATION_BUFSIZE];

    st.bulk_in_mps = st.bulk_out_mps = 0;
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg, sizeof(cfg)) != XFER_RESULT_SUCCESS)
        return;

    uint16_t total = tu_le16toh(((const tusb_desc_configuration_t *)cfg)->wTotalLength);
    if (total > sizeof(cfg)) total = sizeof(cfg);

    for (uint16_t off = 0; off + 2 <= total; ) {
        uint8_t len = cfg[off];
        if (len < 2) break;
        if (cfg[off + 1] == TUSB_DESC_ENDPOINT && off + sizeof(tusb_desc_endpoint_t) <= total) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)&cfg[off];
            if (ep->bmAttributes.xfer == TUSB_XFER_BULK) {
                uint16_t mps = tu_edpt_packet_size(ep);
                if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) st.bulk_in_mps = mps;
                else                                                  st.bulk_out_mps = mps;
            }
        }
        off += len;
    }
}

void tuh_mount_cb(uint8_t daddr)
{
    static CFG_TUH_MEM_ALIGN tusb_desc_device_t dev;

    st.daddr = daddr;
    st.attach_count++;
    tuh_vid_pid_get(daddr, &st.vid, &st.pid);
    st.speed = (uint8_t)tuh_speed_get(daddr);

    memset(&dev, 0, sizeof(dev));
    if (tuh_descriptor_get_device_sync(daddr, &dev, sizeof(dev)) == XFER_RESULT_SUCCESS) {
        st.bcd_usb      = tu_le16toh(dev.bcdUSB);
        st.bcd_device   = tu_le16toh(dev.bcdDevice);
        st.dev_class    = dev.bDeviceClass;
        st.dev_subclass = dev.bDeviceSubClass;
        st.dev_protocol = dev.bDeviceProtocol;
        st.ep0_size     = dev.bMaxPacketSize0;
        st.num_cfg      = dev.bNumConfigurations;
    }
    fetch_bulk_mps(daddr);
    fetch_string(daddr, 0, st.mfr,     sizeof(st.mfr));
    fetch_string(daddr, 1, st.product, sizeof(st.product));
    fetch_string(daddr, 2, st.serial,  sizeof(st.serial));
}

void tuh_umount_cb(uint8_t daddr)
{
    (void)daddr;
    st.remove_count++;
    st.mounted = false;
    st.daddr = 0;
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    tuh_itf_info_t info;

    st.cdc_idx = idx;
    st.cdc_itf = tuh_cdc_itf_get_info(idx, &info) ? info.desc.bInterfaceNumber : 0xFF;
    st.mounted = true;
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    if (idx == st.cdc_idx) st.mounted = false;
}

void usb_link_init(void)
{
    memset(&st, 0, sizeof(st));
    st.cdc_idx = 0xFF;
    st.cdc_itf = 0xFF;
    tuh_init(BOARD_TUH_RHPORT);
    inited = true;
}

void usb_link_pump(void)
{
    if (inited) tuh_task();
}

bool usb_link_ready(void)
{
    return inited && st.mounted && tuh_cdc_mounted(st.cdc_idx);
}

bool usb_link_wait_ready(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    while (!usb_link_ready()) {
        usb_link_pump();
        if (time_reached(end)) return false;
    }
    return true;
}

const usb_link_state_t *usb_link_state(void) { return &st; }

uint32_t usb_link_sie_status(void) { return usb_hw->sie_status; }

size_t usb_link_tx(const void *src, size_t n)
{
    if (!usb_link_ready()) return 0;
    return tuh_cdc_write(st.cdc_idx, src, (uint32_t)n);
}

size_t usb_link_rx(void *dst, size_t n)
{
    if (!usb_link_ready()) return 0;
    return tuh_cdc_read(st.cdc_idx, dst, (uint32_t)n);
}

size_t usb_link_rx_pending(void)
{
    if (!usb_link_ready()) return 0;
    return tuh_cdc_read_available(st.cdc_idx);
}

void usb_link_rx_purge(void)
{
    if (usb_link_ready()) tuh_cdc_read_clear(st.cdc_idx);
}

void usb_link_tx_flush(void)
{
    if (usb_link_ready()) tuh_cdc_write_flush(st.cdc_idx);
}

bool usb_link_tx_drain(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);

    for (;;) {
        if (!usb_link_ready()) return false;
        tuh_cdc_write_flush(st.cdc_idx);
        if (tuh_cdc_write_available(st.cdc_idx) >= CFG_TUH_CDC_TX_BUFSIZE) return true;
        usb_link_pump();
        if (time_reached(end)) return false;
    }
}

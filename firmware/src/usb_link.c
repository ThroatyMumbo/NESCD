// usb_link.c - TinyUSB host stack + a raw byte pipe to the cart's CDC.
//
// The stack is IRQ-driven; usb_link_pump() only drains the event queue and runs
// enumeration callbacks, so it is cheap and safe to call anywhere.

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/usb.h"
#include "tusb.h"
#include "usb_link.h"

#define LINK_STUCK_MS 5000u   // attached but unmounted this long: restart the host stack

static usb_link_state_t st;
static bool inited;
static bool cdc_up, desc_done;   // the link is ready only once both hold

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

static CFG_TUH_MEM_ALIGN tusb_desc_device_t dev_desc;
static CFG_TUH_MEM_ALIGN uint8_t cfg[CFG_TUH_ENUMERATION_BUFSIZE];
static CFG_TUH_MEM_ALIGN uint16_t str_buf[64];

enum { DS_DEV, DS_CFG, DS_MFR, DS_PRODUCT, DS_SERIAL, DS_DONE };

// Walk the configuration for the CDC data interface's bulk endpoints, so the
// console can show the packet size the throughput numbers have to live within.
static void parse_bulk_mps(void)
{
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

static void desc_store(uint32_t step)
{
    switch (step) {
    case DS_DEV:
        st.bcd_usb      = tu_le16toh(dev_desc.bcdUSB);
        st.bcd_device   = tu_le16toh(dev_desc.bcdDevice);
        st.dev_class    = dev_desc.bDeviceClass;
        st.dev_subclass = dev_desc.bDeviceSubClass;
        st.dev_protocol = dev_desc.bDeviceProtocol;
        st.ep0_size     = dev_desc.bMaxPacketSize0;
        st.num_cfg      = dev_desc.bNumConfigurations;
        break;
    case DS_CFG:     parse_bulk_mps(); break;
    case DS_MFR:     desc_str_to_ascii(str_buf, st.mfr,     sizeof(st.mfr));     break;
    case DS_PRODUCT: desc_str_to_ascii(str_buf, st.product, sizeof(st.product)); break;
    case DS_SERIAL:  desc_str_to_ascii(str_buf, st.serial,  sizeof(st.serial));  break;
    }
}

static void desc_cb(tuh_xfer_t *xfer);

// Async on purpose: a _sync read inside tuh_mount_cb spins forever on a lost
// transfer. Bulk stays off until the chain ends, since EPX carries both.
static void desc_next(uint8_t daddr, uint32_t step)
{
    for (;; step++) {
        bool ok;
        memset(str_buf, 0, sizeof(str_buf));
        switch (step) {
        case DS_DEV:
            ok = tuh_descriptor_get_device(daddr, &dev_desc, sizeof(dev_desc), desc_cb, step);
            break;
        case DS_CFG:
            ok = tuh_descriptor_get_configuration(daddr, 0, cfg, sizeof(cfg), desc_cb, step);
            break;
        case DS_MFR:
            ok = tuh_descriptor_get_manufacturer_string(daddr, 0x0409, str_buf, sizeof(str_buf), desc_cb, step);
            break;
        case DS_PRODUCT:
            ok = tuh_descriptor_get_product_string(daddr, 0x0409, str_buf, sizeof(str_buf), desc_cb, step);
            break;
        case DS_SERIAL:
            ok = tuh_descriptor_get_serial_string(daddr, 0x0409, str_buf, sizeof(str_buf), desc_cb, step);
            break;
        default:
            desc_done = true;
            st.mounted = cdc_up;
            return;
        }
        if (ok) return;
    }
}

static void desc_cb(tuh_xfer_t *xfer)
{
    if (xfer->daddr != st.daddr) return;
    if (xfer->result == XFER_RESULT_SUCCESS) desc_store((uint32_t)xfer->user_data);
    desc_next(xfer->daddr, (uint32_t)xfer->user_data + 1);
}

void tuh_mount_cb(uint8_t daddr)
{
    st.daddr = daddr;
    st.attach_count++;
    tuh_vid_pid_get(daddr, &st.vid, &st.pid);
    st.speed = (uint8_t)tuh_speed_get(daddr);

    memset(&dev_desc, 0, sizeof(dev_desc));
    st.bulk_in_mps = st.bulk_out_mps = 0;
    st.mfr[0] = st.product[0] = st.serial[0] = 0;
    desc_done = false;
    desc_next(daddr, DS_DEV);
}

void tuh_umount_cb(uint8_t daddr)
{
    (void)daddr;
    st.remove_count++;
    st.mounted = false;
    st.daddr = 0;
    cdc_up = desc_done = false;
}

void tuh_cdc_mount_cb(uint8_t idx)
{
    tuh_itf_info_t info;

    st.cdc_idx = idx;
    st.cdc_itf = tuh_cdc_itf_get_info(idx, &info) ? info.desc.bInterfaceNumber : 0xFF;
    cdc_up = true;
    st.mounted = desc_done;
}

void tuh_cdc_umount_cb(uint8_t idx)
{
    if (idx == st.cdc_idx) { st.mounted = false; cdc_up = false; }
}

void usb_link_init(void)
{
    memset(&st, 0, sizeof(st));
    st.cdc_idx = 0xFF;
    st.cdc_itf = 0xFF;
    cdc_up = desc_done = false;
    tuh_init(BOARD_TUH_RHPORT);
    inited = true;
}

// TinyUSB never re-enumerates a device that stays attached, and the cart's MCU
// is on molex VBUS, so a failed or hung enumeration is otherwise permanent.
static void link_watchdog(void)
{
    static bool armed;
    static absolute_time_t deadline;

    bool attached = usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS;
    if (!attached || st.mounted) { armed = false; return; }
    if (!armed) { armed = true; deadline = make_timeout_time_ms(LINK_STUCK_MS); return; }
    if (!time_reached(deadline)) return;

    armed = false;
    st.recover_count++;
    tuh_deinit(BOARD_TUH_RHPORT);
    st.mounted = false;
    st.daddr = 0;
    st.cdc_idx = st.cdc_itf = 0xFF;
    cdc_up = desc_done = false;
    tuh_init(BOARD_TUH_RHPORT);
}

void usb_link_pump(void)
{
    static bool busy;

    if (!inited || busy) return;
    busy = true;
    tuh_task();
    link_watchdog();
    busy = false;
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

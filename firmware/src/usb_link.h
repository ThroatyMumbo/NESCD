// usb_link.h - TinyUSB host stack ownership and a raw byte pipe to the
// EverDrive N8 Pro's CDC interface. The only module that touches tusb.h.
//
// Every primitive here is non-blocking; callers pump usb_link_pump() and build
// their own waits on top. Nothing in this module prints.
#ifndef USB_LINK_H
#define USB_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_LINK_STR_MAX 32

typedef struct {
    bool     mounted;          // CDC interface usable right now
    uint8_t  daddr;
    uint8_t  cdc_idx;
    uint8_t  cdc_itf;
    uint8_t  speed;            // tusb_speed_t
    uint16_t vid, pid;
    uint16_t bcd_usb, bcd_device;
    uint8_t  dev_class, dev_subclass, dev_protocol, ep0_size;
    uint8_t  num_cfg;
    uint16_t bulk_in_mps, bulk_out_mps;
    uint32_t attach_count, remove_count;
    char     mfr[USB_LINK_STR_MAX];
    char     product[USB_LINK_STR_MAX];
    char     serial[USB_LINK_STR_MAX];
} usb_link_state_t;

void usb_link_init(void);
void usb_link_pump(void);
bool usb_link_ready(void);
bool usb_link_wait_ready(uint32_t timeout_ms);

const usb_link_state_t *usb_link_state(void);
uint32_t usb_link_sie_status(void);

// Hand bytes to the outbound FIFO. Returns how many were accepted, which is
// routinely less than n; 0 means full or unmounted, not an error.
size_t usb_link_tx(const void *src, size_t n);

// Take up to n already-received bytes. Returns how many were copied.
size_t usb_link_rx(void *dst, size_t n);

size_t usb_link_rx_pending(void);
void   usb_link_rx_purge(void);

// Arm a transfer for whatever is buffered. The stream layer only self-arms on
// whole packets, so a short command sits in the FIFO until this is called.
void usb_link_tx_flush(void);

// Push the outbound FIFO onto the wire. False on timeout or link loss.
bool usb_link_tx_drain(uint32_t timeout_ms);

#endif

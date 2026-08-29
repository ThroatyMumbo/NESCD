// tusb_config.h - TinyUSB host stack config for the EverDrive N8 Pro leg.
//
// CFG_TUSB_MCU (=OPT_MCU_RP2040, correct for RP2350 -- the same hcd serves both
// parts), CFG_TUSB_OS, CFG_TUSB_DEBUG and RP2040_USB_HOST_MODE all arrive from
// the SDK's tinyusb cmake. Do not redefine them here.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must come from the tinyusb_host cmake target"
#endif

#define CFG_TUH_ENABLED             1
#define CFG_TUD_ENABLED             0
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED
#define BOARD_TUH_RHPORT            0

#define CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_ALIGN           __attribute__ ((aligned(4)))

#define CFG_TUH_HUB                 0   // one device, hard-wired
#define CFG_TUH_DEVICE_MAX          1
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_CDC                 1
#define CFG_TUH_HID                 0
#define CFG_TUH_MSC                 0
#define CFG_TUH_VENDOR              0

// The N8 Pro is a native CDC-ACM (STM32F401 OTG-FS); the vendor serial drivers
// only add VID/PID tables that can never match.
#define CFG_TUH_CDC_FTDI            0
#define CFG_TUH_CDC_CP210X          0
#define CFG_TUH_CDC_CH34X           0

// pyserial asserts DTR/RTS and sets 921600 8N1 on open. Doing it
// during enumeration avoids an app-level control transfer, which has no timeout.
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM \
        (CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM \
        { 921600, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

// EPSIZE is how many bytes the stream layer hands the hcd per request, not
// wMaxPacketSize; 512 cuts completion events ~8x versus the 64-byte default.
#define CFG_TUH_CDC_TX_EPSIZE       512
#define CFG_TUH_CDC_RX_EPSIZE       512
// Keep RX == TX: cdc_host.c sizes rx_ff_buf with the TX macro and the RX
// endpoint buffer with TX_EPSIZE, so a larger RX overruns a struct member.
#define CFG_TUH_CDC_TX_BUFSIZE      4096
#define CFG_TUH_CDC_RX_BUFSIZE      4096

#endif

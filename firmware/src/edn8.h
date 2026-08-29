// edn8.h - EverDrive-N8 PRO host protocol over the USB CDC link.
//
// Little-endian, protocol id 0x06, device id 0x17, "Gen2" status handshake.
// Nothing here prints; main.c formats the results.
#ifndef EDN8_H
#define EDN8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// -- device identity -------------------------------------------------------
#define EDN8_PROTOCOL_ID    0x06u
#define EDN8_DEVICE_ID      0x17u
#define EDN8_STATUS_KEY     0x5Au
#define EDN8_STATUS_KEY_OLD 0xA5u
// edn8_status() values, measured on the bench: the menu, or a ROM running.
#define EDN8_ST_MENU        0x00u
#define EDN8_ST_GAME        0x05u
#define EDN8_USB_VID        0x38DFu
#define EDN8_USB_PID        0x0017u

// -- MCU commands ----------------------------------------------------------
// CMD_HARD_RESET (0x12) is deliberately absent: it is not valid on this cart.
#define EDN8_CMD_STATUS   0x10u
#define EDN8_CMD_STATUS2  0x40u
#define EDN8_CMD_MEM_RD   0x19u
#define EDN8_CMD_MEM_WR   0x1Au
#define EDN8_CMD_SYS_INF  0x26u
#define EDN8_CMD_F_FOPN   0xC9u
#define EDN8_CMD_F_FWR    0xCCu
#define EDN8_CMD_F_FCLOSE 0xCEu
#define EDN8_CMD_F_DIR_MK 0xD2u

// FatFs open modes, as the cart's FS layer takes them.
#define EDN8_FA_READ         0x01u
#define EDN8_FA_WRITE        0x02u
#define EDN8_FA_CREATE_ALWAYS 0x08u

// One ack byte gates each block of an F_FWR payload.
#define EDN8_ACK_BLOCK    1024u

// F_DIR_MK on a directory that already exists.
#define EDN8_FS_EXIST     8u

#define EDN8_PATH_MAX     128u

// -- PI-bus cart-memory addresses -----------------------------------------
#define EDN8_ADDR_PRG      0x0000000u   // 8 MB PRG ROM/RAM
#define EDN8_ADDR_CHR      0x0800000u   // 8 MB CHR ROM/RAM
#define EDN8_ADDR_SRM      0x1000000u   // 256 KB battery RAM
#define EDN8_ADDR_FCI_CFG  0x1800000u   // mapper config; also the DMA threshold
#define EDN8_ADDR_FCI_FIFO 0x1810000u   // host->menu fifo
#define EDN8_ADDR_SCFG     0x1800020u
#define EDN8_ADDR_HOST_CTL (EDN8_ADDR_SCFG + 10u)

// -- timeouts --------------------------------------------------------------
#define EDN8_T_PROBE_MS      200u    // during wake + identify
#define EDN8_T_CMD_MS       2000u    // every command once open
#define EDN8_T_INSTALL_MS  30000u    // menu install
#define EDN8_WAKE_SETTLE_MS   50u
#define EDN8_RETRY_MS        100u

// -- errors ----------------------------------------------------------------
#define EDN8_OK        0
#define EDN8_ETIMEOUT (-1)
#define EDN8_ELINK    (-2)
#define EDN8_EPROTO   (-3)
#define EDN8_EPARAM   (-4)
#define EDN8_ESTATUS  (-5)
#define EDN8_EVERIFY  (-6)
#define EDN8_EWEDGED  (-7)

extern uint8_t  edn8_last_status;   // valid after EDN8_ESTATUS
extern size_t   edn8_last_short;    // bytes moved, after EDN8_ETIMEOUT
extern uint32_t edn8_bytes_owed;    // nonzero only in the wedged state

const char *edn8_strerror(int rc);

typedef struct { uint8_t protocol, device; } edn8_ident_t;

typedef struct {
    uint32_t serial_hi, serial_lo;
    uint32_t boot_ctr, game_ctr;
    uint16_t sw_ver, hw_ver, boot_ver;
    uint32_t flash_size;
    uint8_t  cart_form;
    uint8_t  raw[64];
} edn8_sysinfo_t;

typedef struct {
    uint32_t addr;
    size_t   size;
    bool     ok;
    size_t   first_diff;
    int      rc;
} edn8_rt_result_t;

typedef struct {
    size_t   size;
    uint32_t wr_us;          // until the outbound FIFO is empty
    uint32_t wr_barrier_us;  // until the MCU answers a STATUS behind the payload
    uint32_t rd_us;
    bool     match;
    size_t   first_diff;
    int      rc;
} edn8_bench_result_t;

// -- link ------------------------------------------------------------------
int  edn8_open(uint32_t wait_ms);
bool edn8_is_open(void);
void edn8_close(void);
int  edn8_ident(edn8_ident_t *out);
int  edn8_resync(void);

// -- commands --------------------------------------------------------------
int  edn8_status(uint8_t *st);
int  edn8_check(void);
int  edn8_sys_info(edn8_sysinfo_t *out);

// Region-checked: refuses anything at or above EDN8_ADDR_FCI_CFG.
int  edn8_mem_wr(uint32_t addr, const void *data, size_t len);
int  edn8_mem_rd(uint32_t addr, void *dst, size_t len);
int  edn8_mem_wr_pattern(uint32_t addr, size_t len, uint32_t seed);
int  edn8_mem_rd_verify(uint32_t addr, size_t len, uint32_t seed, size_t *first_diff);

// Escape hatch for the FCI region, which the two below are the users of.
int  edn8_mem_wr_raw(uint32_t addr, const void *data, size_t len);

// Push payload into the cart's 16 KiB host->mapper fifo. No framing, no ack:
// flow control is the caller's model of what the loader has drained.
int  edn8_fifo_send(const void *data, size_t len);

// The same, for a body plus a tail: one command instead of two, which keeps the
// cart MCU's per-command work off the streaming path.
int  edn8_fifo_send2(const void *a, size_t na, const void *b, size_t nb);

// Strobe cfg.host_ctl bit 0: rewinds the fifo and the stream parser so a
// streamer can attach to a loader that is already running.
int  edn8_host_resync(void);

// -- SD files, served by the cart's menu ROM -------------------------------
// All four need the cart sitting on the ROM-select menu; edn8_menu_test() is
// the gate. Paths are '/'-separated and are not created implicitly.
int  edn8_dir_make(const char *path);          // EDN8_OK when it already exists
int  edn8_make_path(const char *path);         // dir_make over every ancestor
int  edn8_file_open(const char *path, uint8_t mode);
int  edn8_file_close(void);

// Ack-gated payload for one F_FWR. src may point into XIP: nothing is staged.
int  edn8_file_write(const void *src, size_t len);

// -- menu ------------------------------------------------------------------
int  edn8_menu_test(void);                     // fails unless the menu is up
int  edn8_menu_install(const char *path, uint16_t *map_idx);
int  edn8_menu_start(void);

// -- helpers ---------------------------------------------------------------
uint32_t edn8_pattern(uint8_t *dst, size_t n, uint32_t x);
int  edn8_memtest(size_t size, edn8_rt_result_t out[2]);
int  edn8_bench(uint32_t addr, size_t size, bool do_read, edn8_bench_result_t *out);

// -- primitives, exported for the file/menu/fifo layers ---------------------
int    edn8_tx_cmd(uint8_t cmd);
int    edn8_tx(const void *src, size_t n);
int    edn8_tx8(uint8_t v);
int    edn8_tx32(uint32_t v);
int    edn8_rx(void *dst, size_t n, uint32_t idle_ms);
size_t edn8_rx_pending(void);

#endif

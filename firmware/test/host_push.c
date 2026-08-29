// host_push.c - run src/n8push.c against a packed image on the PC, so the
// container rules and the push order are checked without the cart.
//
//   gcc -O2 -I src -I test/stub -o /tmp/host_push test/host_push.c src/n8push.c src/cdcore.c
//   /tmp/host_push build/push.n8p
//
// The edn8 layer is stubbed to a transcript, which is the point: the one thing
// the bench cannot show cheaply is that the menu is tested before any file is
// spent on it, and that a failed write still closes its file.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edn8.h"
#include "n8push.h"

// -- edn8 stub -------------------------------------------------------------

uint8_t  edn8_last_status;
size_t   edn8_last_short;
uint32_t edn8_bytes_owed;

static char log_buf[8192];
static int  menu_up = 1;
static int  fail_at_write = -1, writes;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_buf);
    vsnprintf(log_buf + n, sizeof(log_buf) - n, fmt, ap);
    va_end(ap);
}

const char *edn8_strerror(int rc) { (void)rc; return "stub error"; }

int edn8_dir_make(const char *path) { logf_("dir %s\n", path); return EDN8_OK; }

int edn8_make_path(const char *path)
{
    char buf[EDN8_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return EDN8_EPARAM;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        int rc = edn8_dir_make(buf);
        buf[i] = '/';
        if (rc != EDN8_OK) return rc;
    }
    return EDN8_OK;
}

int edn8_file_open(const char *path, uint8_t mode)
{
    logf_("open %s %02X\n", path, mode);
    return EDN8_OK;
}

int edn8_file_write(const void *src, size_t len)
{
    (void)src;
    logf_("write %zu\n", len);
    if (writes++ == fail_at_write) return EDN8_ETIMEOUT;
    return EDN8_OK;
}

int edn8_file_close(void) { logf_("close\n"); return EDN8_OK; }

int edn8_menu_test(void)
{
    logf_("menu_test\n");
    return menu_up ? EDN8_OK : EDN8_EPROTO;
}

int edn8_menu_install(const char *path, uint16_t *idx)
{
    logf_("install %s\n", path);
    if (idx) *idx = 42;
    return EDN8_OK;
}

int edn8_menu_start(void) { logf_("start\n"); return EDN8_OK; }

int edn8_mem_wr(uint32_t addr, const void *src, size_t len)
{
    (void)src;
    logf_("mem_wr %08X %zu\n", addr, len);
    return EDN8_OK;
}


// -- checks ----------------------------------------------------------------

static int fails;

static void check(int cond, const char *what)
{
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static void reset_log(void) { log_buf[0] = 0; writes = 0; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s IMAGE.n8p\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)n);
    if (fread(img, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 2; }
    fclose(f);

    printf("%s: %ld B\n\ncontainer:\n", argv[1], n);

    check(n8push_open_at(img, (size_t)n, true) == CD_OK, "opens with the crc checked");
    const n8push_hdr_t *h = n8push_hdr();
    check(h != NULL, "header is published");
    if (!h) return 1;

    printf("  %lu file(s), %lu B payload, crc %08lX, boot %s\n",
           (unsigned long)h->nfiles, (unsigned long)h->payload_bytes,
           (unsigned long)h->crc32, h->boot[0] ? h->boot : "(none)");
    for (uint32_t i = 0; i < h->nfiles; i++)
        printf("    %-32s %lu B @ %lu\n", h->file[i].path,
               (unsigned long)h->file[i].len, (unsigned long)h->file[i].off);

    // Every file must sit inside the payload: the one malformation the firmware
    // cannot notice at push time, because it would just stream past the window.
    int inside = 1;
    for (uint32_t i = 0; i < h->nfiles; i++)
        if ((uint64_t)h->file[i].off + h->file[i].len > h->payload_bytes) inside = 0;
    check(inside, "every file lies inside the payload");
    check(n8push_data(&h->file[0]) == img + h->payload_off + h->file[0].off,
          "data pointer resolves against the payload base");

    printf("\nrejections:\n");
    uint8_t *bad = malloc((size_t)n);

    memcpy(bad, img, (size_t)n);
    bad[0] ^= 0xFF;
    check(n8push_open_at(bad, (size_t)n, false) == CD_ENOIMAGE, "bad magic");

    memcpy(bad, img, (size_t)n);
    bad[8] = 99;
    check(n8push_open_at(bad, (size_t)n, false) == CD_EVERSION, "wrong version");

    memcpy(bad, img, (size_t)n);
    bad[12] = N8PUSH_MAXFILE + 1;
    check(n8push_open_at(bad, (size_t)n, false) == CD_ESHAPE, "nfiles over the cap");

    memcpy(bad, img, (size_t)n);
    bad[(size_t)h->payload_off] ^= 0xFF;
    check(n8push_open_at(bad, (size_t)n, true) == CD_ECRC, "corrupt payload");
    check(n8push_open_at(bad, (size_t)n, false) == CD_OK, "  ...passes with no crc check");

    // A length that reaches past the payload, with a crc that still matches.
    memcpy(bad, img, (size_t)n);
    uint32_t huge = h->payload_bytes + 1;
    memcpy(bad + 100 + 4, &huge, 4);
    check(n8push_open_at(bad, (size_t)n, false) == CD_ESHAPE, "file runs past the payload");

    memcpy(bad, img, (size_t)n);
    memset(bad + 100 + 8, 'x', N8PUSH_PATH);        // never terminated
    check(n8push_open_at(bad, (size_t)n, false) == CD_ESHAPE, "unterminated path");

    memcpy(bad, img, (size_t)n);
    bad[100 + 8] = '/';
    check(n8push_open_at(bad, (size_t)n, false) == CD_ESHAPE, "absolute path");

    check(n8push_open_at(img, sizeof(n8push_hdr_t) - 1, false) == CD_ESHAPE,
          "window smaller than the header");

    printf("\npush order:\n");
    check(n8push_open_at(img, (size_t)n, true) == CD_OK, "reopens clean");

    reset_log();
    fail_at_write = -1;
    check(n8push_run() == CD_OK, "runs to completion");
    check(strncmp(log_buf, "menu_test\n", 10) == 0, "the menu is tested first");
    check(strstr(log_buf, "install ") != NULL, "installs the boot target");
    check(strstr(log_buf, "start\n") > strstr(log_buf, "install "), "starts after installing");
    check(strstr(log_buf, "mem_wr 00C01FF8 4\n") > strstr(log_buf, "install "),
          "seeds the mailbox between install and start");
    check(strstr(log_buf, "start\n") > strstr(log_buf, "mem_wr "),
          "  ...before the game can run");
    check(strstr(log_buf, "open ") < strstr(log_buf, "install "), "every file precedes the install");

    reset_log();
    menu_up = 0;
    check(n8push_run() == CD_EMENU, "refuses when the menu is gone");
    check(strstr(log_buf, "open ") == NULL, "  ...without spending a file on it");
    menu_up = 1;

    reset_log();
    fail_at_write = 0;
    check(n8push_run() == EDN8_ETIMEOUT, "propagates a write failure");
    check(strstr(log_buf, "close\n") != NULL, "  ...and still closes the file");
    check(strstr(log_buf, "install ") == NULL, "  ...and does not install");
    fail_at_write = -1;

    printf("\n%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}

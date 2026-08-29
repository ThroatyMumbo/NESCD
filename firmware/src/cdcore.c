#include "cdcore.h"
#include "edn8.h"

const char *cd_strerror(int rc)
{
    switch (rc) {
    case CD_OK:        return "ok";
    case CD_EDISCIO:   return "the drive failed a read we cannot skip";
    case CD_EMEDIUM:   return "the disc was removed";
    case CD_EPSRAM:    return "no psram: nothing to buffer the disc in";
    case CD_ESLOTS:    return "psram slots too small for this block width";
    case CD_EVERSION:  return "unsupported image version";
    case CD_ECRC:      return "image crc mismatch";
    case CD_ESHAPE:    return "malformed image header";
    case CD_ENOAUDIO:  return "item carries no audio";
    case CD_ENOGAME:   return "not a game disc";
    case CD_EGAMEFMT:  return "malformed game catalog";
    case CD_ENOITEM:   return "no such item on the disc";
    case CD_EROMBIG:   return "rom item too big to stage";
    case CD_ETRKFMT:   return "malformed track item";
    case CD_ENOIMAGE:  return "no push image staged in flash";
    case CD_EMENU:     return "the cart is not on its ROM-select menu";
    case CD_EMBOX:     return "the mailbox read caught a write in flight";
    default:           return edn8_strerror(rc);
    }
}

uint32_t cd_crc32_step(uint32_t c, const uint8_t *p, size_t n)
{
    static const uint32_t tab[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ tab[c & 0xF];
        c = (c >> 4) ^ tab[c & 0xF];
    }
    return c;
}

uint32_t cd_crc32(const uint8_t *p, size_t n)
{
    return ~cd_crc32_step(0xFFFFFFFFu, p, n);
}

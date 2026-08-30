// audiofmt.h - the block codecs a disc may carry. One block is `spr` mono
// samples, self-contained: the header holds decoder state, not sample 0, so a
// block decodes on its own wherever the producer joined the stream.
#ifndef AUDIOFMT_H
#define AUDIOFMT_H

#define AFMT_NONE   0u
#define AFMT_PCM16  1u              // mono s16le, 2 * spr bytes
#define AFMT_ADPCM4 2u              // ima adpcm 4-bit, see ima.h
#define AFMT_CDDA   3u              // stereo s16le interleaved, 4 * spr bytes: a raw CD-DA sector

#define CDDA_SPR    588u            // stereo frames in one 2352-byte sector

#endif

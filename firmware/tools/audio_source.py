#!/usr/bin/env python3
"""Decode a media file's audio track into blocks of exactly `spr` mono samples.

RAM is O(1) in the length of the source: ffmpeg is pulled, never pushed.

The filter chain is deliberately minimal, and that is a measured result rather
than a guess.  Plain `-vn -ac 1 -ar 44100` starts exactly at sample 0 on mp4
and Opus/WebM; adding `aresample=first_pts=0` *breaks* Opus by +7.0 ms, since
it keeps the pre-skip region the decoder is supposed to drop.

`async=1` is kept for a different reason: it hard-compensates mid-file
timestamp gaps over 0.1 s with silence, so a gap cannot slide every later
sample.  It measures identically to naive on every container tested.
"""
import os
import subprocess

import numpy as np

from audiofmt import AUDIO_RATE, AUDIO_SPR


def has_audio(media):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=index", "-of", "csv=p=0", media],
        stdout=subprocess.PIPE, text=True).stdout.strip()
    return bool(out)


def describe(media):
    """'aac 48000x2', or None when there is no audio track."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=codec_name,sample_rate,channels", "-of", "csv=p=0", media],
        stdout=subprocess.PIPE, text=True).stdout.strip()
    if not out:
        return None
    f = out.split(",")
    return f"{f[0]} {f[1]}x{f[2]}" if len(f) >= 3 else out


class AudioSource:
    """`read()` -> exactly `spr` int16 mono samples, forever.

    It never returns None: it keeps producing silence past its own EOF, so the
    caller's loop decides where the track ends.
    """

    def __init__(self, media, spr=AUDIO_SPR, rate=AUDIO_RATE, gain_db=None,
                 verify=True):
        del verify
        self.media = media
        self.spr = spr
        self.rate = rate
        self.nread = 0
        self.pad_blocks = 0
        self.nsamples = 0                       # real samples off the pipe
        self.exhausted = False
        self._buf = np.zeros(0, np.int16)

        # async=1 hard-compensates timestamp gaps over 0.1 s with silence, so a
        # mid-file gap cannot slide every later sample.  Nothing else: see the
        # module docstring for why first_pts=0 is absent.  Gain goes after the
        # resample, at 44.1k.
        self.afilter = "aresample=async=1"
        if gain_db:
            self.afilter += f",volume={gain_db}dB"

        self.desc = (f"{os.path.basename(media)}: {describe(media)} -> "
                     f"{rate} mono, {spr} samples/block"
                     + (f", {gain_db:+g} dB" if gain_db else ""))

        self.ap = subprocess.Popen(
            ["ffmpeg", "-nostdin", "-v", "error", "-i", media, "-vn",
             "-af", self.afilter, "-ac", "1", "-ar", str(rate),
             "-f", "s16le", "-"],
            stdout=subprocess.PIPE, bufsize=spr * 8)

    def read(self):
        # ffmpeg emits variable-size packets (925 then 941 samples, typically),
        # so accumulate rather than assuming a spr-aligned read.
        while not self.exhausted and len(self._buf) < self.spr:
            raw = self.ap.stdout.read((self.spr - len(self._buf)) * 2)
            if not raw:
                self.exhausted = True
                break
            got = np.frombuffer(raw, np.int16)
            self.nsamples += len(got)
            self._buf = np.concatenate((self._buf, got))

        self.nread += 1
        if len(self._buf) >= self.spr:
            out, self._buf = self._buf[:self.spr], self._buf[self.spr:]
            return out
        # Past EOF: pad the tail of the last real block, then pure silence.
        out = np.zeros(self.spr, np.int16)
        out[:len(self._buf)] = self._buf
        self._buf = np.zeros(0, np.int16)
        self.pad_blocks += 1
        return out

    def close(self):
        p = self.ap
        if p.poll() is None:
            # terminate before closing the pipe, or ffmpeg dies on SIGPIPE
            # mid-write and sprays muxer errors.
            p.terminate()
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
        p.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class SilentSource:
    """Stands in for AudioSource when a source has no audio track, so the
    mastering tools never need a `if audio is None` on the hot path."""

    def __init__(self, spr=AUDIO_SPR, rate=AUDIO_RATE):
        self.spr, self.rate = spr, rate
        self.nread = self.nsamples = 0
        self.pad_blocks = 0
        self.exhausted = True
        self.desc = "no audio track: silence"

    def read(self):
        self.nread += 1
        self.pad_blocks += 1
        return np.zeros(self.spr, np.int16)

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        pass


def open_audio(media, spr=AUDIO_SPR, rate=AUDIO_RATE, gain_db=None,
               required=False):
    """AudioSource, or SilentSource when the source carries no audio.  A tool
    that was *explicitly* asked for audio passes required=True and gets a hard
    error instead of silent silence."""
    if has_audio(media):
        return AudioSource(media, spr, rate, gain_db)
    if required:
        raise SystemExit(f"{media}: no audio track")
    return SilentSource(spr, rate)


def duration(media):
    """Source length in seconds, or 0 when the container does not say."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", media],
        stdout=subprocess.PIPE, text=True).stdout.strip()
    try:
        return float(out)
    except ValueError:
        return 0.0


if __name__ == "__main__":
    import argparse
    import time
    ap = argparse.ArgumentParser(
        description="Decode a media file's audio into blocks (a rate and "
                    "alignment probe)")
    ap.add_argument("media")
    ap.add_argument("--spr", type=int, default=AUDIO_SPR)
    ap.add_argument("--gain", type=float)
    ap.add_argument("--blocks", type=int, help="stop after N blocks")
    a = ap.parse_args()

    src = open_audio(a.media, spr=a.spr, gain_db=a.gain)
    print(f"  {src.desc}")
    t0 = time.perf_counter()
    peak = 0
    while a.blocks is None or src.nread < a.blocks:
        b = src.read()
        if src.exhausted and src.pad_blocks:
            break
        peak = max(peak, int(np.abs(b.astype(np.int32)).max()))
    dt = time.perf_counter() - t0
    src.close()

    d = duration(a.media)
    want = int(d * src.rate) if d else 0
    print(f"  {src.nsamples} samples in {dt:.2f} s"
          + (f" -- {src.nsamples / src.rate / dt:.0f}x realtime" if dt else ""))
    if want:
        print(f"  duration {d:.2f} s wants {want}, got {src.nsamples} "
              f"({src.nsamples - want:+d} = {(src.nsamples - want) / a.spr:+.2f} blocks)")
    print(f"  peak {peak} ({20 * np.log10(max(peak, 1) / 32768):.1f} dBFS)")

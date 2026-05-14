#!/usr/bin/env python3
"""Generate a sparse full-scale PCM marker WAV for latency measurements."""

import argparse
import struct
import wave


SAMPLE_RATE_HZ = 48_000
CHANNELS = 2
SAMPLE_WIDTH_BYTES = 2
PACKET_FRAMES = 48

MARKER_FRAMES = (
    (32767, 32767),
    (-32768, -32768),
    (32767, 32767),
    (-32768, -32768),
)


def write_silence(wav_file, frames):
    chunk_frames = SAMPLE_RATE_HZ
    silence_chunk = b"\x00" * (chunk_frames * CHANNELS * SAMPLE_WIDTH_BYTES)

    while frames > 0:
        n = min(frames, chunk_frames)
        wav_file.writeframesraw(silence_chunk[: n * CHANNELS * SAMPLE_WIDTH_BYTES])
        frames -= n


def write_marker(wav_file):
    for left, right in MARKER_FRAMES:
        wav_file.writeframesraw(struct.pack("<hh", left, right))


def main():
    parser = argparse.ArgumentParser(
        description="Generate a 48 kHz stereo PCM WAV with sparse bipolar latency markers."
    )
    parser.add_argument(
        "-o",
        "--output",
        default="latency_marker_48k_stereo.wav",
        help="output WAV path",
    )
    parser.add_argument(
        "--duration-s",
        type=int,
        default=60,
        help="total file duration in seconds",
    )
    parser.add_argument(
        "--interval-s",
        type=int,
        default=10,
        help="silence interval between markers in seconds",
    )
    args = parser.parse_args()

    if args.duration_s <= 0:
        raise SystemExit("--duration-s must be positive")
    if args.interval_s <= 0:
        raise SystemExit("--interval-s must be positive")

    total_frames = args.duration_s * SAMPLE_RATE_HZ
    interval_frames = args.interval_s * SAMPLE_RATE_HZ

    if interval_frames % PACKET_FRAMES != 0:
        raise SystemExit("--interval-s must align to 1 ms / 48-frame packet boundaries")

    frames_written = 0
    next_marker_frame = interval_frames

    with wave.open(args.output, "wb") as wav_file:
        wav_file.setnchannels(CHANNELS)
        wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(SAMPLE_RATE_HZ)

        while frames_written < total_frames:
            if frames_written == next_marker_frame:
                write_marker(wav_file)
                frames_written += len(MARKER_FRAMES)
                next_marker_frame += interval_frames
                continue

            silence_until = min(next_marker_frame, total_frames)
            silence_frames = silence_until - frames_written
            if silence_frames <= 0:
                next_marker_frame += interval_frames
                continue

            write_silence(wav_file, silence_frames)
            frames_written += silence_frames


if __name__ == "__main__":
    main()

# Opus receive-side interop

This tree keeps the existing Speex modes, bit packing and frame slicing
unchanged. The Opus receive path is an additional adapter:

- `include/speex/speex_opus.h` exposes packet parsing and PCM decoding.
- `libspeex/opus_adapter.c` owns TOC parsing, duration tracking, concealment
  state and the `SpeexMode` bridge.
- `speexdec` auto-detects an Ogg Opus stream and writes the same 16-bit PCM
  used by the existing playback path.

The PCM output is interleaved signed 16-bit. The output rate is selected when
the decoder is created (8, 12, 16, 24 or 48 kHz), so an existing fixed-rate
playback chain does not need to reconfigure itself for every packet.

## Packet timing

No fixed frame length or sample rate is assumed. For every packet:

1. Read the TOC byte.
2. Derive the frame duration (2.5, 5, 10, 20, 40 or 60 ms).
3. Derive the declared bandwidth rate (8, 12, 16, 24 or 48 kHz).
4. Parse the frame count and VBR/CBR/padding layout.
5. Compute packet samples as `packet_duration * output_rate / 1000000`.

`SpeexOpusPacketInfo` reports all of these values. A packet may contain up to
48 frames and up to 120 ms of audio. The decoder accepts the packet as one
unit; callers do not need to know the frame layout.

## Loss handling

`speex_opus_decode_loss()` can be called repeatedly. Each call advances the
same Opus concealment state, so consecutive losses do not reset the decoder
and a later good packet continues the stream. A corrupt packet is also
concealed rather than terminating the receive loop. `SPEEX_OPUS_GET_LOSS_STREAK`
reports the current run length and returns to zero after a good packet.

For Ogg input, `speexdec` compares decoded position with the page granule
position. Missing packets or whole pages are filled with PLC audio, which
keeps the output timeline at the duration declared by the stream.

## Latency

The adapter adds no jitter buffer and no extra packet buffering. It reports:

- `SPEEX_OPUS_GET_LOOKAHEAD`: 6.5 ms at every supported output rate
  (312 samples at 48 kHz).
- `speex_opus_decoder_get_latency_us()`: lookahead plus the duration of the
  last packet.

On the reference host the measured decode cost was about 25-40 us per packet
for 10-20 ms packets (floating-point build). Algorithmic receive latency is:

| Packet duration | Lookahead | Algorithmic latency |
| --- | ---: | ---: |
| 10 ms | 6.5 ms | 16.5 ms |
| 20 ms | 6.5 ms | 26.5 ms |
| 60 ms | 6.5 ms | 66.5 ms |

For a talkback path, 10 or 20 ms Opus packets are recommended. The adapter
does not increase the buffer size when switching between packet durations.

## Build

Meson:

```sh
meson setup build -Dopus=enabled
meson compile -C build
meson test -C build
```

Autotools:

```sh
./autogen.sh
./configure
make
make check
```

`--disable-opus` / `-Dopus=disabled` builds the base Speex library without the
Opus backend. Packet parsing remains available; decoder creation returns
`NULL` because no PCM backend is present.

## Interop checklist

Before the joint test on Wednesday, both sides should exchange:

- 10 ms and 20 ms packet sets, plus at least one 2.5 ms and one 60 ms set.
- Narrowband and wideband captures from the same speech source.
- A loss trace with one loss, three consecutive losses and five consecutive
  losses.

For each set, assert:

1. `frames_per_packet`, `frame_duration_us`, `sample_rate` and
   `packet_duration_us` match the sender annotation.
2. Total decoded samples equal the annotated duration times the output rate.
3. After each loss run, the next good packet produces non-silent PCM and the
   loss streak resets.
4. Both implementations report the same packet boundary for VBR, CBR and
   padded packets.
5. The measured algorithmic latency stays within the talkback budget for the
   chosen packet duration.

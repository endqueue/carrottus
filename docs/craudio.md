# Carrottus Audio

Thin audio endpoint I/O: WASAPI (Windows), ALSA (Linux; PipeWire planned), CoreAudio
(macOS), RemoteIO (iOS and iPadOS, one backend: the RemoteIO audio unit on the
process's AVAudioSession, which the caller owns; card L11, decided 2026-10-28, built
2026-11-02); a stub on watchOS. The public API and its per-backend mappings are documented in
`include/craudio.h`; this file holds design notes that don't belong in the header.

## Future intent: positional (spatialized) audio

> **Status: future intent — not implemented, and not part of Carrottus Audio.** Spatialization
> is rendering policy, so under Carrottus's zero-policy rule it lives *above* Carrottus —
> in whatever calls it. Carrottus Audio's only part is
> reporting which speaker each device channel feeds
> (`CRAUDIO_DEVICE_PROPERTIES.native_format.positions`, and the granted
> `CRAUDIO_STREAM_CONFIG.format.positions`), so the layer above can render for the
> real speaker layout.

Motivating case: an application plays a sound effect from a direction relative to
where something happened in its viewport.

1. **Direction** — the clicked point relative to the camera: azimuth (left/right),
   elevation (up/down), distance. Computed by whoever owns the camera and the clicked
   geometry, above Carrottus.
2. **Spatializer** — turns that direction into a signal per output channel, mixed into
   the stream callback's buffer:

| Method | What it does | Fits when |
|---|---|---|
| **Stereo panning** (constant-power) | Click left in the viewport → louder in the left speaker | Stereo speakers/headphones. Simplest; plenty for a UI click. |
| **Binaural / HRTF** | Filters the sound the way a head and ears would, so it seems to come from a direction — including above, below, behind | Headphones, when elevation/depth should actually be audible |
| **VBAP / multichannel panning** | Distributes the sound across the nearest real speakers | 5.1 / 7.1 / Atmos setups |
| **Ambisonics** | Encodes the sound into a sound field first, then decodes it to whatever output exists | Many simultaneous sources, head tracking, or one mix for every kind of output |

For one effect per placement, **direct panning to the device's actual layout** is the
right tool: `FL FR` → stereo panning, `FL FR FC LFE RL RR SL SR` → 7.1 panning, no
layout → a fallback the caller chooses. Ambisonics earns its complexity only with many
concurrent sources or a moving listener (VR).

Two practical constraints when this gets built:

- **Latency matters more than the panning method.** A click sound must land within
  roughly 20–30 ms of the click or it feels detached — favouring WASAPI's low-latency
  shared path (`IAudioClient3`) and small periods. `CRAUDIO_CALLBACK_INFO::host_time_ns`
  (same clock as `crperf_get_monotonic_time_ns`) lets the mixer schedule the sound
  against the actual click time.
- **It doubles as an accessibility feature** — hearing *where* something landed is a
  cue in its own right.

### Why ambisonics isn't a channel position

Every `CRAUDIO_CHANNEL_POSITION` names a speaker. Ambisonic channels (W
omnidirectional, X/Y/Z front-back/left-right/up-down; (order + 1)² channels in total)
describe a sound field and are each mixed into *every* speaker by a decoder, so
labelling them as positions would misdescribe them. CoreAudio's ambisonic labels
therefore read back as `AUX`. If ambisonic streams are ever carried as such, they need
their own per-format layout kind (speaker positions vs. ambisonic order + channel
ordering + normalization), not more position labels.

## TODO: let the caller see and decide what OS conversion does (CoreAudio)

`CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION` on CoreAudio puts an `AudioConverter`
between the stream's format and the device's. Today the flag is one bit: the caller
can allow conversion, but can't see what was converted or choose how. Each item below
is a decision the backend currently makes, or a fact it currently keeps to itself.

**Where this should go:** Carrottus Audio reports every one of these differences between
what was asked for and what the device does, and the caller makes every choice among
them. Nothing below should stay a built-in default.

**Decided 2026-10-28 with the user (card S4), for every backend** (CoreAudio's
converter, WASAPI's shared-mode conversion and its quality flag, ALSA's `plug`
layer and rate converter):
- **Reporting:** a chained output struct on the stream config, filled once the
  stream is open: whether it is converted, the device-side format, the
  channel routing in use, the device period granted, how the callback is
  driven (direct or re-chunked, and the device frames per I/O cycle), and the
  converter's delay. Periods and delays are in frames at a stated rate, so a
  caller can place audio against presentation time.
- **Control:** a chained input struct giving the channel map (or a mix) and
  the converter quality, **required** whenever `ALLOW_OS_CONVERSION` is set.
  No default routing, so mono into stereo is the caller's choice.
- What a backend can't see is reported as unknown, never guessed. A device
  rate change still ends the stream, reported; the caller opens another.
- A caller that opens its stream in the device's `native_format` without the
  flag has nothing converted at all, and needs none of this.
Not started; needs Windows and Linux runs.

- [ ] **Whether a stream is converted at all.** A format that already is the device's
  is not converted, flag or no flag, and `CRAUDIO_STREAM_CONFIG` doesn't say which
  happened. Report it, along with the device-side format (sample rate, channel count,
  layout) the stream is being converted to or from.
- [ ] **Channel routing when the channel counts differ.** No channel map and no mix
  are set, so the converter's default applies: stream channel n is device channel n,
  and channels past the device's are discarded (OUTPUT) or silent (INPUT). Mono into a
  stereo device plays on the left only; stereo into mono drops the right channel.
  Report the routing in use, and let the caller supply a channel map or ask for a mix
  (`kAudioConverterChannelMap`, `kAudioConverterPropertyPerformDownmix`,
  `kAudioConverterPropertyChannelMixMap`).
- [ ] **Channel layout.** A requested layout must equal the one that default routing
  gives (the device's positions for shared channels, `NA` for the rest) or the stream
  is refused. Let the caller hand both layouts to the converter
  (`kAudioConverterInputChannelLayout` / `kAudioConverterOutputChannelLayout`) when it
  wants a different one.
- [ ] **The device's I/O buffer size.** The stream's period is in its own frames; the
  device's buffer is set to the nearest whole number of device frames that lasts as
  long. The rounding is the backend's and the result isn't reported. Report the device
  period that was granted, and let the caller state it or the rounding.
- [ ] **How the callback is driven.** The callback always gets exactly `period_frames`
  from a buffer of the backend's, zero or more times per device I/O cycle, because with
  a rate change the two sets of frames don't line up. Report that the stream is driven
  this way, and how many device frames one I/O cycle is, so the caller can tell a
  re-chunked stream from a direct one.
- [ ] **Timing that leaves out the converter.** `host_time_ns` is the I/O cycle's host
  time moved by the period's offset in the cycle, and
  `craudio_stream_get_latency_frames()` is the HAL's total restated in stream frames.
  Neither includes the converter's own delay. Report that delay separately.
- [ ] **Converter quality and priming.** Sample-rate converter quality and the prime
  method are left at the converter's defaults. Report what is in use and let the caller
  set them.
- [ ] **A device rate change ends the stream.** The converter was built for the old
  rate, so the stream moves to `CRAUDIO_STREAM_STATE_ERROR` with
  `CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED`, the same as an unconverted stream. That is
  already reported; whether a converted stream could instead be offered the new rate
  is the caller's choice to add.

Verification still owed, separate from the above:

- [ ] The CoreAudio conversion path is compiled on macOS and has not been run.
- [ ] `CRAUDIO_SAMPLE_FORMAT_S24_32_LSB` may be refused: `AudioConverter` only
  promises non-packed integers that are high-aligned. If it is, the stream fails with
  `CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED`.

WASAPI (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`) and ALSA (plugins) convert inside the
platform and hide the same kinds of difference. Whatever shape the reporting takes
should cover them too, so one description of "what was converted and how" works on
every backend.

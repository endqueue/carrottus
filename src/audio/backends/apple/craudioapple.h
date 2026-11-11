#ifndef CARROTTUS_CRAUDIOAPPLE_H
#define CARROTTUS_CRAUDIOAPPLE_H

/*
 * Apple-platform helpers shared by every Apple audio backend: only
 * CoreAudioTypes and mach, plus AudioToolbox where it exists — macOS, iOS
 * and iPadOS have it; the watchOS SDK has no AudioToolbox framework at all.
 * So the macOS HAL backend (craudiocoreaudio.c), the iOS and iPadOS
 * backend (craudioremoteio.c) and a future watchOS one all build on it. Nothing here touches the
 * macOS-only HAL (AudioObject*).
 */

#include "../../craudiointernal.h"

#include <CoreAudioTypes/CoreAudioTypes.h>
#include <mach/mach_time.h>
#if !CRAUDIO_PLATFORM_WATCHOS
#include <AudioToolbox/AudioToolbox.h>
#define CRAUDIO_APPLE_HAS_AUDIOTOOLBOX 1
#else
#define CRAUDIO_APPLE_HAS_AUDIOTOOLBOX 0
#endif

#if __cplusplus
extern "C" {
#endif

// An AudioChannelLayout (descriptions, bitmap, or a predefined layout tag
// expanded through AudioToolbox) as Carrottus Audio positions. `out->channel_count`
// must already be set; if the layout doesn't describe exactly that many
// channels, the result is no layout (all UNSPECIFIED). Without AudioToolbox
// (watchOS) a tag can't be expanded, so a tag-only layout is no layout
// there rather than a guess. Expanding a tag takes a scratch block from
// `allocator`, freed before the call returns.
void craudio_apple_layout_to_positions(
    const CR_ALLOCATOR *allocator,
    const AudioChannelLayout *layout,
    CRAUDIO_FORMAT *out);

// mach_absolute_time units (AudioTimeStamp.mHostTime) to nanoseconds —
// the clock crperf_get_monotonic_time_ns reads on Apple platforms.
uint64_t craudio_apple_host_time_to_ns(uint64_t host_time, const mach_timebase_info_data_t *timebase);

/*
 * Float32 frames between a CoreAudio AudioBufferList — any number of
 * buffers, each interleaved over its own mNumberChannels — and Carrottus Audio's
 * callback layout: one interleaved buffer (`interleaved`), or one plane per
 * channel (`planes`, used when `interleaved` is NULL). Rearrangement only;
 * samples are never converted. Channels the list doesn't carry come out as
 * silence.
 */
void craudio_apple_copy_from_buffer_list(
    const AudioBufferList *list,
    uint32_t frames,
    uint32_t channels,
    float *interleaved,
    float *const *planes);
void craudio_apple_copy_to_buffer_list(
    AudioBufferList *list,
    uint32_t frames,
    uint32_t channels,
    const float *interleaved,
    float *const *planes);

#if __cplusplus
}
#endif

#endif // CARROTTUS_CRAUDIOAPPLE_H

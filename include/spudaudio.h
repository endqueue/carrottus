/**
 * @file spudaudio.h
 * @brief SpudAudio: thin audio endpoint I/O over WASAPI, ALSA, CoreAudio and
 * RemoteIO. Known problems: the PipeWire backend is planned and not built;
 * and watchOS has no backend yet, only an argument-checking stub.
 *
 * The overview is under @ref spudaudio "SpudAudio". In result codes, SAUD
 * stands for SpudAudio (`SPUDRESULT_SAUD_*`, in spudcore.h).
 */

#ifndef SPUDLIB_SPUDAUDIO_H
#define SPUDLIB_SPUDAUDIO_H

#include "spudcore.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @defgroup spudaudio SpudAudio
 * @brief Thin audio endpoint I/O (WASAPI / ALSA / CoreAudio / RemoteIO):
 * moves PCM frames between the caller and an audio endpoint. Known problems:
 * the PipeWire backend is planned and not built
 * (SPUDAUDIO_NATIVE_API_PIPEWIRE returns SPUDRESULT_NOT_IMPLEMENTED_YET);
 * and watchOS has no backend yet.
 *
 * A translation of the platform audio API, nothing more. No mixer, no
 * decoder, no resampler, no voice/sound objects, no 3D spatialisation, no
 * volume policy. All of that is the caller's problem, the same way SpudGPU
 * never decides what to draw.
 *
 * - Never picks a device. The OS default endpoints are reported as a
 *   property (SPUDAUDIO_DEVICE_PROPERTIES::default_roles); the caller
 *   decides whether to use them.
 * - Never silently converts formats. If the endpoint can't take the
 *   requested format, stream creation fails with
 *   SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED. Platform-side conversion (WASAPI
 *   AUTOCONVERTPCM, ALSA plugins, a CoreAudio AudioConverter) is only
 *   enabled when the caller
 *   explicitly sets SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION, and refused
 *   where it can't happen - see SPUDAUDIO_STREAM_FLAGS.
 * - The data path is a callback, because that's the only model every
 *   platform shares natively (CoreAudio and RemoteIO have no
 *   acquire/release buffer API). On WASAPI/ALSA the backend owns the
 *   event-wait thread that drives it - plumbing, not policy.
 *
 * @par Threads
 * The library rule holds (@ref spud_threads "Threads", spudcore.h): an
 * instance, a device handle and a stream are each used by one caller at a
 * time. What is particular to SpudAudio is that two of the caller's
 * functions run on threads that are not the caller's, each marked
 * `Thread:` where it is declared: the stream callback
 * (SPUDAUDIO_STREAM_CALLBACK), on a thread SpudAudio starts in
 * spudaudio_stream_start() on WASAPI and ALSA and on the platform's own
 * real-time thread on CoreAudio and RemoteIO; and the device-event callback
 * (SPUDAUDIO_DEVICE_EVENT_CALLBACK). spudaudio_stream_get_status() is the
 * one call that may overlap the others on its stream.
 * - Spatialisation is rendering policy and so is the caller's. SpudAudio's
 *   only part in it is reporting which speaker each device channel feeds
 *   (SPUDAUDIO_FORMAT::positions, in a device's native format and in a
 *   stream's granted format), so the caller can render for the real speaker
 *   layout.
 *
 * @par iOS and iPadOS: the audio session is the caller's
 * The RemoteIO backend (SPUDAUDIO_NATIVE_API_REMOTEIO) reads the process's
 * AVAudioSession as it finds it and changes nothing in it that a caller
 * could choose: it never sets a category, a mode or options, never
 * activates or deactivates the session, and never asks for record
 * permission. Before a stream, the caller:
 * - sets a category that has the stream's direction: any category plays,
 *   and only AVAudioSessionCategoryPlayAndRecord and
 *   AVAudioSessionCategoryRecord have input (with any other, the route has
 *   no input port, so spudaudio_enumerate_devices() lists no input
 *   device);
 * - activates the session (where it hasn't, what happens at
 *   spudaudio_stream_start() is the platform's: iOS may activate it
 *   itself, as it does for any audio object, or refuse with
 *   SPUDRESULT_SAUD_SESSION_INACTIVE);
 * - for input, has record permission granted
 *   (AVAudioApplication.recordPermission); without it an input stream is
 *   refused with SPUDRESULT_SAUD_ACCESS_DENIED.
 *
 * The session's sample rate and IO buffer duration are the caller's too:
 * they hold for every audio object of the process (an AVAudioEngine, an
 * AVAudioPlayer), so SpudAudio sets neither. A stream is granted the
 * session's rate and its IO buffer in force as its period
 * (SPUDAUDIO_PERIOD_MODE_FIXED); for another, the caller sets the session's
 * preferred sample rate or IO buffer duration
 * (setPreferredSampleRate, setPreferredIOBufferDuration), which the system
 * may grant differently, then reads the device's format and timing caps
 * again. spudaudio_set_device_sample_rate() is
 * SPUDRESULT_SAUD_SAMPLE_RATE_NOT_SETTABLE here.
 *
 * An interruption (a call, an alarm, another app's audio that doesn't mix)
 * ends a running stream in SPUDAUDIO_STREAM_STATE_ERROR with
 * SPUDRESULT_SAUD_INTERRUPTED. Nothing is restarted: whether and when to
 * open a stream again is the caller's, who is told when the interruption
 * ends by the session it owns (AVAudioSessionInterruptionNotification).
 */

/**
 * @defgroup spudaudio_config Compile-time configuration
 * @ingroup spudaudio
 * @brief Macros that say which platform this build is for and which native
 * audio APIs are built in. Known problem: SPUDAUDIO_COMPILE_PIPEWIRE is
 * never 1 yet, and SPUDAUDIO_PLATFORM_WATCHOS has no native API to go with
 * it.
 *
 * spudlib's CMake defines one `SPUDAUDIO_PLATFORM_*` for the target OS and
 * a `SPUDAUDIO_COMPILE_*` for each native audio API built in, as PUBLIC
 * definitions - so everything that includes this header sees the same
 * values and knows at compile time which SPUDAUDIO_NATIVE_API values
 * spudaudio_create_instance() accepts, rather than finding
 * SPUDRESULT_INVALID_API at runtime. Anything not defined is 0.
 *
 * | Platform macro             | Native API macro                                      |
 * |----------------------------|-------------------------------------------------------|
 * | SPUDAUDIO_PLATFORM_WINDOWS | SPUDAUDIO_COMPILE_WASAPI                              |
 * | SPUDAUDIO_PLATFORM_LINUX   | SPUDAUDIO_COMPILE_ALSA (SPUDAUDIO_COMPILE_PIPEWIRE: planned) |
 * | SPUDAUDIO_PLATFORM_MACOS   | SPUDAUDIO_COMPILE_COREAUDIO                           |
 * | SPUDAUDIO_PLATFORM_IOS     | SPUDAUDIO_COMPILE_REMOTEIO - iOS and iPadOS, one build |
 * | SPUDAUDIO_PLATFORM_WATCHOS | none yet                                              |
 *
 * iPhone and iPad software is built with one SDK, so iOS and iPadOS are one
 * platform here and one backend: nothing in SpudAudio tells them apart.
 * SPUDAUDIO_PLATFORM_APPLE is any of the three Apple platforms. With no
 * `SPUDAUDIO_COMPILE_*` at all, spudlib builds an argument-checking stub
 * whose calls return SPUDRESULT_NOT_IMPLEMENTED_YET.
 * @{
 */

#ifndef SPUDAUDIO_PLATFORM_WINDOWS
/** @brief 1 in a build for Windows, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_PLATFORM_WINDOWS 0
#endif
#ifndef SPUDAUDIO_PLATFORM_LINUX
/** @brief 1 in a build for Linux, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_PLATFORM_LINUX 0
#endif
#ifndef SPUDAUDIO_PLATFORM_MACOS
/** @brief 1 in a build for macOS, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_PLATFORM_MACOS 0
#endif
#ifndef SPUDAUDIO_PLATFORM_IOS
/** @brief 1 in a build for iOS or iPadOS, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_PLATFORM_IOS 0
#endif
#ifndef SPUDAUDIO_PLATFORM_WATCHOS
/**
 * @brief 1 in a build for watchOS, 0 otherwise. Defined by the build. Known
 * problem: no native audio API is built for it yet.
 */
#define SPUDAUDIO_PLATFORM_WATCHOS 0
#endif
/** @brief 1 in a build for any of the three Apple platforms, 0 otherwise. */
#define SPUDAUDIO_PLATFORM_APPLE (SPUDAUDIO_PLATFORM_MACOS || SPUDAUDIO_PLATFORM_IOS || SPUDAUDIO_PLATFORM_WATCHOS)

#ifndef SPUDAUDIO_COMPILE_WASAPI
/** @brief 1 when the WASAPI backend is built in, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_COMPILE_WASAPI 0
#endif
#ifndef SPUDAUDIO_COMPILE_ALSA
/** @brief 1 when the ALSA backend is built in, 0 otherwise. Defined by the build. */
#define SPUDAUDIO_COMPILE_ALSA 0
#endif
#ifndef SPUDAUDIO_COMPILE_PIPEWIRE
/**
 * @brief 1 when the PipeWire backend is built in, 0 otherwise. Known
 * problem: that backend is planned and not written, so this is always 0.
 */
#define SPUDAUDIO_COMPILE_PIPEWIRE 0
#endif
#ifndef SPUDAUDIO_COMPILE_COREAUDIO
/**
 * @brief 1 when the CoreAudio (HAL) backend is built in, 0 otherwise.
 * Defined by the build; macOS only.
 */
#define SPUDAUDIO_COMPILE_COREAUDIO 0
#endif
#ifndef SPUDAUDIO_COMPILE_REMOTEIO
/**
 * @brief 1 when the RemoteIO backend (AVAudioSession and the RemoteIO audio
 * unit) is built in, 0 otherwise. Defined by the build; iOS and iPadOS
 * only.
 */
#define SPUDAUDIO_COMPILE_REMOTEIO 0
#endif

#if SPUDAUDIO_PLATFORM_WINDOWS + SPUDAUDIO_PLATFORM_LINUX + SPUDAUDIO_PLATFORM_MACOS + SPUDAUDIO_PLATFORM_IOS + SPUDAUDIO_PLATFORM_WATCHOS > 1
#error "SpudAudio: more than one SPUDAUDIO_PLATFORM_* is defined"
#endif
#if SPUDAUDIO_COMPILE_WASAPI && !SPUDAUDIO_PLATFORM_WINDOWS
#error "SpudAudio: SPUDAUDIO_COMPILE_WASAPI needs SPUDAUDIO_PLATFORM_WINDOWS"
#endif
#if (SPUDAUDIO_COMPILE_ALSA || SPUDAUDIO_COMPILE_PIPEWIRE) && !SPUDAUDIO_PLATFORM_LINUX
#error "SpudAudio: SPUDAUDIO_COMPILE_ALSA / _PIPEWIRE need SPUDAUDIO_PLATFORM_LINUX"
#endif
#if SPUDAUDIO_COMPILE_COREAUDIO && !SPUDAUDIO_PLATFORM_MACOS
#error "SpudAudio: SPUDAUDIO_COMPILE_COREAUDIO (the HAL) needs SPUDAUDIO_PLATFORM_MACOS"
#endif
#if SPUDAUDIO_COMPILE_REMOTEIO && !SPUDAUDIO_PLATFORM_IOS
#error "SpudAudio: SPUDAUDIO_COMPILE_REMOTEIO needs SPUDAUDIO_PLATFORM_IOS"
#endif

/** @} */

/**
 * @defgroup spudaudio_formats Handles and formats
 * @ingroup spudaudio
 * @brief The opaque handles, and how a PCM format is described: sample
 * format, rate, channels and channel positions.
 * @{
 */

/** @brief Opaque handle to SpudAudio bound to one native audio API. */
typedef struct spudaudio_instance_t *spudaudio_instance;
/**
 * @brief Opaque handle to one audio endpoint in one direction.
 *
 * Owned by the instance that enumerated it; never destroyed by the caller.
 */
typedef struct spudaudio_device_t *spudaudio_device;
/** @brief Opaque handle to one stream of PCM frames to or from a device. */
typedef struct spudaudio_stream_t *spudaudio_stream;

/** @brief Which native audio API an instance is bound to. */
typedef uint32_t SPUDAUDIO_NATIVE_API;
/** @brief Values of SPUDAUDIO_NATIVE_API. */
enum {
	SPUDAUDIO_NATIVE_API_NONE      = 0, /**< No API; what a NULL instance reports. */
	SPUDAUDIO_NATIVE_API_WASAPI    = 1, /**< WASAPI (Windows). */
	/** PipeWire (Linux). Known problem: planned, not built;
	 * spudaudio_create_instance() returns SPUDRESULT_NOT_IMPLEMENTED_YET for
	 * it. */
	SPUDAUDIO_NATIVE_API_PIPEWIRE  = 2,
	SPUDAUDIO_NATIVE_API_ALSA      = 3, /**< ALSA (Linux). */
	SPUDAUDIO_NATIVE_API_COREAUDIO = 4, /**< CoreAudio, on the HAL (macOS). */
	/** The RemoteIO audio unit on the process's AVAudioSession (iOS and
	 * iPadOS). */
	SPUDAUDIO_NATIVE_API_REMOTEIO  = 5
};

/** @brief Which way frames move through a device or a stream. */
typedef uint32_t SPUDAUDIO_DIRECTION;
/** @brief Values of SPUDAUDIO_DIRECTION. */
enum {
	SPUDAUDIO_DIRECTION_OUTPUT = 0, /**< Render / playback. */
	SPUDAUDIO_DIRECTION_INPUT  = 1  /**< Capture. */
};

/**
 * @brief Sample formats, all native-endian.
 *
 * 24-bit audio has three real layouts and each one is its own format,
 * because the same "24 in 32" name means different bits on different
 * platforms:
 *
 * - S24_PACKED: 3 bytes per sample. WASAPI 24/24 PCM, ALSA
 *   SND_PCM_FORMAT_S24_3LE, CoreAudio packed 24-bit.
 * - S24_32_MSB: 24 bits in the high 3 bytes of a 32-bit container, low byte
 *   zero - bit-identical to an S32 sample with 24 significant bits. WASAPI
 *   32-bit container / 24 valid bits (WASAPI's only 24-in-32), ALSA
 *   SND_PCM_FORMAT_S32 (no separate ALSA name), CoreAudio
 *   kAudioFormatFlagIsAlignedHigh.
 * - S24_32_LSB: 24 bits in the low 3 bytes of a 32-bit container, high byte
 *   ignored. ALSA SND_PCM_FORMAT_S24, CoreAudio 24 bits unaligned-high in
 *   32. WASAPI has no equivalent.
 *
 * The CoreAudio backend presents each device's mixable virtual format, so
 * F32 is the only sample format a stream is granted there without
 * SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION. With it, any of these may be
 * asked for and the platform's AudioConverter decides; S24_32_LSB is the
 * one it may refuse, since it only promises non-packed integers that are
 * high-aligned. The RemoteIO backend presents the route's format as Float32
 * at the session's sample rate, so there too F32 is the only sample format
 * granted without the flag, and with it RemoteIO's own converter decides.
 */
typedef uint32_t SPUDAUDIO_SAMPLE_FORMAT;
/** @brief Values of SPUDAUDIO_SAMPLE_FORMAT. */
enum {
	SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN    = 0, /**< No format; never valid in a request. */
	SPUDAUDIO_SAMPLE_FORMAT_S16        = 1, /**< Signed 16-bit integer. */
	SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED = 2, /**< Signed 24-bit integer in 3 bytes. */
	SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB = 3, /**< Signed 24-bit integer in the high 3 bytes of 4. */
	SPUDAUDIO_SAMPLE_FORMAT_S24_32_LSB = 4, /**< Signed 24-bit integer in the low 3 bytes of 4. */
	SPUDAUDIO_SAMPLE_FORMAT_S32        = 5, /**< Signed 32-bit integer. */
	SPUDAUDIO_SAMPLE_FORMAT_F32        = 6  /**< 32-bit float. */
};

/**
 * @brief Returns how many bytes one sample of a format takes.
 *
 * @param[in] fmt The sample format.
 *
 * @return Bytes per sample: 2 for S16, 3 for S24_PACKED, 4 for the rest.
 * @retval 0 `fmt` is SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN or an unrecognised
 *         value.
 */
uint32_t spudaudio_sample_format_byte_size(SPUDAUDIO_SAMPLE_FORMAT fmt);

/**
 * @brief The most channels a SPUDAUDIO_FORMAT can describe.
 */
#define SPUDAUDIO_MAX_CHANNELS 64

/**
 * @brief Which speaker a channel feeds - one per channel, in channel order.
 * Known problem: ambisonic channels (W, X, Y, Z) describe a sound field and
 * not a speaker, so they have no position here; CoreAudio's ambisonic labels
 * read back as AUX, and an ambisonic stream can't be described as one. That
 * would need a layout kind of its own (ambisonic order, channel ordering
 * and normalisation), not more position labels.
 *
 * An array rather than WASAPI's speaker bitmask because it's the
 * representation every backend can express: ALSA (snd_pcm_chmap_t) and
 * PipeWire take position arrays natively, CoreAudio an AudioChannelLayout
 * of per-channel labels, and a mask can't describe unlabeled channels or a
 * non-canonical order.
 *
 * The labels are the union of what the platforms use - PipeWire's set,
 * which is ALSA's full set plus LFE2, and which contains all 18 WASAPI
 * speakers and nearly all CoreAudio speaker labels. Names follow
 * PipeWire/ALSA: "R" is rear (WASAPI calls it back), and BC/BLC/BRC are
 * *bottom* positions (WASAPI's back center is RC). The 18 WASAPI speakers
 * come first, in WASAPI's dwChannelMask bit order: a speaker's bit is its
 * position minus SPUDAUDIO_CHANNEL_POSITION_FL.
 *
 * - NA: a channel that exists but carries nothing (ALSA/PipeWire NA -
 *   common in HDMI maps). The only position that may repeat.
 * - AUX0..63: unlabeled channels that do carry signal (ALSA
 *   SND_CHMAP_UNKNOWN, PipeWire AUXn, WASAPI channels beyond the mask,
 *   CoreAudio Discrete_n).
 *
 * A format either has no layout (every position UNSPECIFIED) or a full
 * one (none UNSPECIFIED, no duplicates except NA, MONO only for 1
 * channel).
 *
 * A label a backend can't express makes the layout unsupported there -
 * never relabeled. The probe's suggestion then drops the layout (all
 * UNSPECIFIED), the one choice every backend accepts:
 *
 * - WASAPI: only MONO, FL..TRR, plus NA/AUX as trailing unassigned channels
 *   (WASAPI can't tell NA from AUX; both read back as AUX). Speakers must
 *   also be in ascending bit order with NA/AUX last - when only the order
 *   is wrong, the suggestion is the reordered layout instead.
 * - ALSA: everything except LFE2.
 * - CoreAudio: its non-speaker labels (matrix Lt/Rt, ambisonic W/X/Y/Z,
 *   haptic) have no position and read back as AUX.
 * - RemoteIO: a device's positions are its port's channel labels
 *   (AVAudioSessionChannelDescription::channelLabel), read as CoreAudio's
 *   are. A stream takes no layout but the port's own.
 */
typedef uint8_t SPUDAUDIO_CHANNEL_POSITION;
/** @brief Values of SPUDAUDIO_CHANNEL_POSITION. */
enum {
	SPUDAUDIO_CHANNEL_POSITION_UNSPECIFIED = 0, /**< No layout; every position of the format is this, or none is. */
	SPUDAUDIO_CHANNEL_POSITION_MONO        = 1, /**< The one channel of a 1-channel format. */

	/* The 18 WASAPI speakers, in dwChannelMask bit order (bit = pos - FL). */
	SPUDAUDIO_CHANNEL_POSITION_FL  = 2,  /**< Front left. */
	SPUDAUDIO_CHANNEL_POSITION_FR  = 3,  /**< Front right. */
	SPUDAUDIO_CHANNEL_POSITION_FC  = 4,  /**< Front center. */
	SPUDAUDIO_CHANNEL_POSITION_LFE = 5,  /**< Low-frequency effects. */
	SPUDAUDIO_CHANNEL_POSITION_RL  = 6,  /**< Rear left (WASAPI back left). */
	SPUDAUDIO_CHANNEL_POSITION_RR  = 7,  /**< Rear right (WASAPI back right). */
	SPUDAUDIO_CHANNEL_POSITION_FLC = 8,  /**< Front left of center. */
	SPUDAUDIO_CHANNEL_POSITION_FRC = 9,  /**< Front right of center. */
	SPUDAUDIO_CHANNEL_POSITION_RC  = 10, /**< Rear center (WASAPI back center). */
	SPUDAUDIO_CHANNEL_POSITION_SL  = 11, /**< Side left. */
	SPUDAUDIO_CHANNEL_POSITION_SR  = 12, /**< Side right. */
	SPUDAUDIO_CHANNEL_POSITION_TC  = 13, /**< Top center. */
	SPUDAUDIO_CHANNEL_POSITION_TFL = 14, /**< Top front left. */
	SPUDAUDIO_CHANNEL_POSITION_TFC = 15, /**< Top front center. */
	SPUDAUDIO_CHANNEL_POSITION_TFR = 16, /**< Top front right. */
	SPUDAUDIO_CHANNEL_POSITION_TRL = 17, /**< Top rear left (WASAPI top back left). */
	SPUDAUDIO_CHANNEL_POSITION_TRC = 18, /**< Top rear center (WASAPI top back center). */
	SPUDAUDIO_CHANNEL_POSITION_TRR = 19, /**< Top rear right (WASAPI top back right). */

	/* The rest of the PipeWire/ALSA set. */
	SPUDAUDIO_CHANNEL_POSITION_RLC  = 20, /**< Rear left of center. */
	SPUDAUDIO_CHANNEL_POSITION_RRC  = 21, /**< Rear right of center. */
	SPUDAUDIO_CHANNEL_POSITION_FLW  = 22, /**< Front left wide. */
	SPUDAUDIO_CHANNEL_POSITION_FRW  = 23, /**< Front right wide. */
	SPUDAUDIO_CHANNEL_POSITION_LFE2 = 24, /**< Second LFE (PipeWire, CoreAudio). */
	SPUDAUDIO_CHANNEL_POSITION_FLH  = 25, /**< Front left high. */
	SPUDAUDIO_CHANNEL_POSITION_FCH  = 26, /**< Front center high. */
	SPUDAUDIO_CHANNEL_POSITION_FRH  = 27, /**< Front right high. */
	SPUDAUDIO_CHANNEL_POSITION_TFLC = 28, /**< Top front left of center. */
	SPUDAUDIO_CHANNEL_POSITION_TFRC = 29, /**< Top front right of center. */
	SPUDAUDIO_CHANNEL_POSITION_TSL  = 30, /**< Top side left. */
	SPUDAUDIO_CHANNEL_POSITION_TSR  = 31, /**< Top side right. */
	SPUDAUDIO_CHANNEL_POSITION_LLFE = 32, /**< Left LFE. */
	SPUDAUDIO_CHANNEL_POSITION_RLFE = 33, /**< Right LFE. */
	SPUDAUDIO_CHANNEL_POSITION_BC   = 34, /**< Bottom center. */
	SPUDAUDIO_CHANNEL_POSITION_BLC  = 35, /**< Bottom left center. */
	SPUDAUDIO_CHANNEL_POSITION_BRC  = 36, /**< Bottom right center. */

	SPUDAUDIO_CHANNEL_POSITION_NA       = 63,  /**< Present but silent; may repeat. */
	SPUDAUDIO_CHANNEL_POSITION_AUX0     = 64,  /**< The first unlabeled channel; AUX0 + n for n below 64. */
	SPUDAUDIO_CHANNEL_POSITION_AUX_LAST = 127  /**< The last unlabeled channel, AUX63. */
};

/**
 * @brief One PCM format: sample format, rate, channels and where each
 * channel goes. Known problem: `positions` can only name speakers, so an
 * ambisonic layout can't be described (see SPUDAUDIO_CHANNEL_POSITION).
 *
 * `positions` is what lets a caller that spatialises render for the
 * device's real speaker layout: FL FR for stereo panning, FL FR FC LFE RL
 * RR SL SR for 7.1, and no layout for a fallback the caller chooses.
 */
/* Frozen: a value, held by value in other structs. It never changes; a new need gets a new type. */
typedef struct SPUDAUDIO_FORMAT {
	SPUDAUDIO_SAMPLE_FORMAT sample_format; /**< How one sample is stored. */
	uint32_t sample_rate;   /**< Frames per second, in Hz. */
	uint32_t channel_count; /**< 1..SPUDAUDIO_MAX_CHANNELS. */
	/** Which speaker each channel feeds. Only the first channel_count
	 * entries are meaningful. Either all UNSPECIFIED or a full layout; see
	 * SPUDAUDIO_CHANNEL_POSITION. */
	SPUDAUDIO_CHANNEL_POSITION positions[SPUDAUDIO_MAX_CHANNELS];
	bool interleaved;       /**< true: one buffer of interleaved frames. false: planar, one buffer per channel. */
} SPUDAUDIO_FORMAT;

/** @} */

/**
 * @defgroup spudaudio_devices Instance and device enumeration
 * @ingroup spudaudio
 * @brief Making an instance, listing its endpoints and reading what they
 * are.
 * @{
 */

/**
 * @brief What the caller decides when creating an instance. No field has a
 * default: a zeroed desc is refused, for its `base`, and one with only its
 * `base` set is refused for its `allocator`.
 */
typedef struct SPUDAUDIO_INSTANCE_DESC {
	/** `type` is SPUD_STRUCT_TYPE_AUDIO_INSTANCE_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Where the memory of the instance, and of every device handle and
	 * stream made from it, comes from (SPUD_ALLOCATOR). Copied, so the
	 * struct need not outlive the call; its functions and `user_data` must
	 * outlive the instance and every stream made from it. Known problem: on
	 * WASAPI the watch's last block may be freed after
	 * spudaudio_destroy_instance() has returned, if the system still holds
	 * the watch then (not verified either way), so there the allocator has
	 * to outlive that too.
	 *
	 * Thread: never called from a stream's callback. While a device-event
	 * callback is set (spudaudio_set_device_event_callback()), whatever
	 * watches the device list calls it at each change, at the same time as
	 * the caller's own calls: the system's notification thread on WASAPI
	 * (which may also make that last free), a serial dispatch queue the
	 * backend makes on CoreAudio and on RemoteIO, a thread the backend
	 * starts on ALSA. An allocator that can't take overlapping calls can't be used with a
	 * device-event callback. */
	const SPUD_ALLOCATOR *allocator;
	/** The native API to bind to. There is no "best available" value - the
	 * caller chooses. */
	SPUDAUDIO_NATIVE_API native_api;
} SPUDAUDIO_INSTANCE_DESC;

/**
 * @brief Creates a SpudAudio instance bound to one native audio API. Known
 * problem: SPUDAUDIO_NATIVE_API_PIPEWIRE is not built and returns
 * SPUDRESULT_NOT_IMPLEMENTED_YET, as does every API in a build with no
 * native audio API (watchOS).
 *
 * Makes no platform object: on RemoteIO, nothing of the audio session is
 * read or changed until a call needs it.
 *
 * @param[in]  desc         What the caller decides.
 * @param[out] out_instance Receives the new instance, the caller's to
 *                          destroy with spudaudio_destroy_instance().
 *
 * @retval SPUD_SUCCESS The instance was made.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_INSTANCE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_instance` is NULL.
 * @retval SPUDRESULT_NULL_ALLOCATOR `desc->allocator` is NULL.
 * @retval SPUDRESULT_INVALID_ALLOCATOR `desc->allocator` lacks a function.
 * @retval SPUDRESULT_INVALID_API `desc->native_api` isn't compiled in on
 *         this platform, or is SPUDAUDIO_NATIVE_API_NONE.
 * @retval SPUDRESULT_NOT_IMPLEMENTED_YET `desc->native_api` is
 *         SPUDAUDIO_NATIVE_API_PIPEWIRE on Linux, or this build has no
 *         native audio API at all.
 * @retval SPUDRESULT_OUT_OF_MEMORY The instance could not be allocated.
 * @return Another SPUDRESULT where the platform refused to start its audio
 *         API.
 */
SPUDRESULT spudaudio_create_instance(
    const SPUDAUDIO_INSTANCE_DESC *desc,
    spudaudio_instance *out_instance);

/**
 * @brief Destroys an instance and the device handles it enumerated.
 *
 * Stops device-change notification first, as
 * spudaudio_set_device_event_callback() with a NULL callback does, so it
 * returns once the instance's device-event callback is not running and will
 * not run again, and must not be called from inside that callback.
 *
 * @param[in] instance The instance to destroy.
 *
 * @retval SPUD_SUCCESS The instance was destroyed.
 * @retval SPUDRESULT_SAUD_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_GENERAL_FAILURE Called from inside the instance's
 *         device-event callback; the instance is not destroyed.
 */
SPUDRESULT spudaudio_destroy_instance(spudaudio_instance instance);

/**
 * @brief Returns the native API an instance is bound to.
 *
 * @param[in] instance The instance to ask.
 *
 * @return The instance's native API.
 * @retval SPUDAUDIO_NATIVE_API_NONE `instance` is NULL.
 */
SPUDAUDIO_NATIVE_API spudaudio_get_native_api(spudaudio_instance instance);

/**
 * @brief Enumerates the endpoints of one direction.
 *
 * RemoteIO: iOS has no list of devices to choose from, only the session's
 * current route, which the system and the person holding the device
 * choose. The endpoints are the route's ports in the direction
 * (AVAudioSessionRouteDescription::outputs or ::inputs; one port each, but
 * for AVAudioSessionCategoryMultiRoute): the speaker, or the headphones,
 * the Bluetooth device, the AirPlay receiver plugged in or chosen in its
 * place. A port that leaves the route is no longer enumerated, and one that
 * comes back gets the handle it had. Inputs are listed only when the
 * session's category has input and an input is available
 * (AVAudioSession::inputAvailable).
 *
 * @param[in]  instance         The SpudAudio instance to enumerate through.
 * @param[in]  direction        Output (render) or input (capture) endpoints.
 * @param[out] out_devices      Receives an array of device handles, owned by
 *                              the instance. The array stays valid until the
 *                              next enumeration of the same direction; the
 *                              handles in it stay valid until the instance
 *                              is destroyed, and the same endpoint keeps the
 *                              same handle across enumerations.
 * @param[out] out_device_count Receives the number of devices in
 *                              `out_devices`.
 *
 * @retval SPUD_SUCCESS The devices were enumerated.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_devices` or
 *         `out_device_count` is NULL.
 * @retval SPUDRESULT_SAUD_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `direction` is neither
 *         SPUDAUDIO_DIRECTION_OUTPUT nor SPUDAUDIO_DIRECTION_INPUT.
 * @retval SPUDRESULT_OUT_OF_MEMORY The array could not be allocated.
 * @return Another SPUDRESULT where the platform's enumeration failed.
 */
SPUDRESULT spudaudio_enumerate_devices(
    spudaudio_instance instance,
    SPUDAUDIO_DIRECTION direction,
    spudaudio_device **out_devices,
    uint32_t *out_device_count);

/**
 * @brief Which OS default(s) an endpoint currently is, as a set of bits.
 *
 * Platforms keep several defaults, not one, so this is a set of roles; a
 * backend only ever sets the roles its platform has.
 *
 * - GENERAL: WASAPI eConsole (Windows' "Default Device"), CoreAudio
 *   kAudioHardwarePropertyDefault{Output,Input}Device, PipeWire
 *   default.audio.{sink,source}, ALSA "default".
 * - MULTIMEDIA: WASAPI eMultimedia. Windows' settings UI moves it together
 *   with eConsole, but the API keeps it separate.
 * - COMMUNICATIONS: WASAPI eCommunications ("Default Communication
 *   Device").
 * - SYSTEM_SOUNDS: CoreAudio
 *   kAudioHardwarePropertyDefaultSystemOutputDevice (alerts / UI sounds;
 *   output only).
 *
 * RemoteIO: GENERAL is the route's port in a direction where the route has
 * exactly one; where it has several (AVAudioSessionCategoryMultiRoute) the
 * system plays to all of them and none holds it.
 */
typedef uint32_t SPUDAUDIO_DEFAULT_ROLES;
/** @brief Bits of SPUDAUDIO_DEFAULT_ROLES. */
enum {
	SPUDAUDIO_DEFAULT_ROLE_NONE           = 0x0, /**< Not a default of any kind. */
	SPUDAUDIO_DEFAULT_ROLE_GENERAL        = 0x1, /**< The platform's general default endpoint. */
	SPUDAUDIO_DEFAULT_ROLE_MULTIMEDIA     = 0x2, /**< WASAPI's multimedia default. */
	SPUDAUDIO_DEFAULT_ROLE_COMMUNICATIONS = 0x4, /**< WASAPI's communications default. */
	SPUDAUDIO_DEFAULT_ROLE_SYSTEM_SOUNDS  = 0x8  /**< CoreAudio's system-sound output. */
};

/**
 * @brief What an endpoint is: its name, identity, direction, default roles
 * and native format.
 *
 * Period/buffer limits aren't here: every backend's limits depend on the
 * format and sharing mode - see spudaudio_get_timing_caps().
 */
typedef struct SPUDAUDIO_DEVICE_PROPERTIES {
	/** `type` is SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES; `next` is NULL (nothing is chained onto
	 * this struct yet). The caller builds the chain and sets each type; SpudAudio fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** Human-readable, UTF-8, null-terminated, truncated to fit. WASAPI:
	 * PKEY_Device_FriendlyName. ALSA: the PCM's DESC hint (lines joined),
	 * else its name. RemoteIO: the port's portName. */
	char name[128];
	/** Stable across runs (IMMDevice ID / ALSA PCM name / PipeWire node
	 * name / CoreAudio UID / RemoteIO the port's UID), null-terminated, so
	 * the caller can persist its choice. */
	char persistent_id[256];
	/** Whether this is an output or an input endpoint. */
	SPUDAUDIO_DIRECTION direction;
	/** The default roles this endpoint holds right now. Informational;
	 * SpudAudio never acts on it. */
	SPUDAUDIO_DEFAULT_ROLES default_roles;
	/** The endpoint's own shared-mode format (WASAPI GetMixFormat). Zeroed
	 * where the backend has no single native format (ALSA). Its `positions`
	 * say which speaker each device channel feeds. RemoteIO: Float32 at the
	 * session's sampleRate, with the port's channels, interleaved (a
	 * stream is granted interleaved or planar). */
	SPUDAUDIO_FORMAT native_format;
	/** Whether the backend has an exclusive mode at all (WASAPI yes,
	 * CoreAudio yes - hog mode, ALSA no, RemoteIO no). Whether this device currently
	 * allows it is only known by probing:
	 * SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE. */
	bool supports_exclusive;
} SPUDAUDIO_DEVICE_PROPERTIES;

/**
 * @brief Reads an endpoint's properties as they are now.
 *
 * @param[in]  device         The device to ask.
 * @param[out] out_properties Receives the properties.
 *
 * @retval SPUD_SUCCESS The properties were written.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `out_properties->base.next` is not NULL.
 * @retval SPUDRESULT_SAUD_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_SAUD_DEVICE_LOST The device disappeared since
 *         enumeration (RemoteIO: its port is not in the current route).
 * @return Another SPUDRESULT where the platform failed to report a
 *         property.
 */
SPUDRESULT spudaudio_get_device_properties(
    spudaudio_device device,
    SPUDAUDIO_DEVICE_PROPERTIES *out_properties);

/**
 * @brief Sets a device-wide sample rate, where the platform has one -
 * CoreAudio only; WASAPI, ALSA and RemoteIO return
 * SPUDRESULT_SAUD_SAMPLE_RATE_NOT_SETTABLE.
 *
 * This changes the device for every application using it, which is why
 * it's a separate, explicit call: stream creation never changes it, and a
 * stream format at another rate is simply
 * SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED with the device's current rate as
 * the suggestion.
 *
 * - CoreAudio: kAudioDevicePropertyNominalSampleRate. The HAL applies it
 *   asynchronously; this returns once the device reports the new rate.
 * - WASAPI: not settable - the shared-mode rate is a user setting with no
 *   API, and exclusive streams carry their own rate.
 * - ALSA: not settable - the rate is per opened PCM (hw_params).
 * - RemoteIO: not settable here - the rate is the audio session's, which
 *   is the caller's (AVAudioSession setPreferredSampleRate), and holds for
 *   every port of the route and every audio object of the process.
 *
 * A running CoreAudio or RemoteIO stream whose device rate changes -
 * through this call or anything else (on iOS, the caller's session or a
 * route change) - ends in
 * SPUDAUDIO_STREAM_STATE_ERROR with error
 * SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED: its granted format no longer
 * describes the device.
 *
 * @param[in] device      The device whose rate to set.
 * @param[in] sample_rate The new rate in Hz.
 *
 * @retval SPUD_SUCCESS The device reports the new rate.
 * @retval SPUDRESULT_SAUD_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `sample_rate` is 0.
 * @retval SPUDRESULT_SAUD_SAMPLE_RATE_NOT_SETTABLE The platform has no
 *         device-wide rate to set here (WASAPI, ALSA, RemoteIO).
 * @retval SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED CoreAudio: `sample_rate` is
 *         not in kAudioDevicePropertyAvailableNominalSampleRates.
 * @retval SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE CoreAudio: another process
 *         holds the device in hog mode.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE CoreAudio: the device never
 *         reported the new rate.
 */
SPUDRESULT spudaudio_set_device_sample_rate(spudaudio_device device, uint32_t sample_rate);

/** @} */

/**
 * @defgroup spudaudio_streams Streams
 * @ingroup spudaudio
 * @brief Describing, probing, creating and running a stream of PCM frames.
 * @{
 */

/**
 * @brief How a stream shares its endpoint and whether the platform may
 * convert formats, as a set of bits.
 *
 * - EXCLUSIVE: sole use of the endpoint. See
 *   SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE under spudaudio_probe_stream().
 *   RemoteIO has no exclusive mode: the session's category decides
 *   whether other apps' audio mixes with it, and that is the caller's.
 * - ALLOW_OS_CONVERSION: lets the platform convert between the requested
 *   format and the endpoint's. Without it the format must match exactly.
 *   Where conversion can't exist for this request, the flag is refused,
 *   never ignored:
 *   - WASAPI: shared mode uses AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM +
 *     SRC_DEFAULT_QUALITY. Exclusive mode has no conversion:
 *     SPUDRESULT_SAUD_OS_CONVERSION_UNAVAILABLE.
 *   - ALSA: conversion lives in plugins, so it comes from the PCM name the
 *     caller picked (plughw:, default, ...); the flag turns on
 *     snd_pcm_hw_params_set_rate_resample. PCMs that never convert (hw:,
 *     dmix, dsnoop, dshare) refuse it with
 *     SPUDRESULT_SAUD_OS_CONVERSION_UNAVAILABLE.
 *   - CoreAudio: the HAL converts nothing, so a format that isn't the
 *     device's goes through an AudioConverter (AudioToolbox) given the two
 *     formats and nothing else - no channel map, no mix, no quality
 *     setting. Sample format, sample rate and channel count may all
 *     differ. With a different channel count the converter's default
 *     applies: stream channel n is device channel n, and channels past the
 *     device's are discarded (OUTPUT) or silent (INPUT), so their position
 *     is NA. A format the converter refuses is
 *     SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED. Works with EXCLUSIVE.
 *
 *     A converted CoreAudio stream is timed in its own frames: the callback
 *     always gets exactly period_frames, zero or more times per device I/O
 *     cycle, and the device's I/O buffer is set to the nearest whole number
 *     of device frames that lasts as long. host_time_ns and
 *     spudaudio_stream_get_latency_frames() leave out the converter's own
 *     delay. A format that already is the device's is not converted, flag
 *     or no flag.
 *   - RemoteIO: the audio unit's own converter, between the client format
 *     set on the unit and the route's, given the two formats and nothing
 *     else - no channel map, no quality setting. Sample format and sample
 *     rate may differ; the channel count may not (the converter's routing
 *     between counts is not documented, so it can't be reported), and a
 *     different count is SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED with the
 *     port's count suggested. A converted stream's callback is called with
 *     as many of its own frames as the unit asks for, which varies, and
 *     never more than max_callback_frames; host_time_ns and
 *     spudaudio_stream_get_latency_frames() leave out the converter's own
 *     delay. A format that already is the route's is not converted, flag
 *     or no flag.
 *   - PipeWire (planned): the graph converts natively.
 */
typedef uint32_t SPUDAUDIO_STREAM_FLAGS;
/** @brief Bits of SPUDAUDIO_STREAM_FLAGS. */
enum {
	SPUDAUDIO_STREAM_FLAG_NONE                = 0x0, /**< Shared mode, exact format. */
	SPUDAUDIO_STREAM_FLAG_EXCLUSIVE           = 0x1, /**< Sole use of the endpoint. */
	SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION = 0x2  /**< The platform may convert formats. */
};

/**
 * @brief How binding a requested period is.
 *
 * This differs by backend, not just the range, so it's reported rather
 * than hidden. In every mode, the callback's `frame_count` is the
 * authoritative current period and never exceeds
 * SPUDAUDIO_STREAM_CONFIG::max_callback_frames.
 */
typedef uint32_t SPUDAUDIO_PERIOD_MODE;
/** @brief Values of SPUDAUDIO_PERIOD_MODE. */
enum {
	/** Not determined - the zero value, e.g. in a probe suggestion that
	 * stopped before the period was checked. */
	SPUDAUDIO_PERIOD_MODE_UNKNOWN    = 0,
	/** Only one period exists (min == max == default). WASAPI shared via
	 * IAudioClient; RemoteIO, the session's IO buffer in force. */
	SPUDAUDIO_PERIOD_MODE_FIXED      = 1,
	/** Granted exactly as requested if spudaudio_probe_stream() accepts it.
	 * WASAPI shared via IAudioClient3, WASAPI exclusive, ALSA, CoreAudio. */
	SPUDAUDIO_PERIOD_MODE_EXACT      = 2,
	/** A request only - a shared graph picks the actual period and may
	 * change it while the stream runs. PipeWire (planned). */
	SPUDAUDIO_PERIOD_MODE_NEGOTIATED = 3
};

/**
 * @brief Timing limits for one device + format + sharing mode.
 *
 * A guide, not a guarantee: some constraints can't be expressed as
 * min/max/step (WASAPI exclusive buffer alignment, ALSA driver-specific
 * rules) and only show up when the OS is asked about a concrete request.
 * spudaudio_probe_stream() is the authoritative check on every backend.
 *
 * - WASAPI: IAudioClient::GetDevicePeriod (100 ns, converted at the
 *   format's rate) / IAudioClient3::GetSharedModeEnginePeriod.
 * - ALSA: snd_pcm_hw_params_get_{period,buffer}_size_{min,max} after
 *   format/rate/channels are applied.
 * - CoreAudio: kAudioDevicePropertyBufferFrameSizeRange. With
 *   ALLOW_OS_CONVERSION and a format that isn't the device's, the range is
 *   restated in the format's frames and `variable_callback_frames` is
 *   false, since a converted stream's callback always gets one period.
 * - PipeWire (planned): clock.min-quantum / clock.max-quantum /
 *   clock.quantum.
 * - RemoteIO: FIXED: the session's IO buffer in force (AVAudioSession
 *   IOBufferDuration at its sampleRate, in the format's frames where
 *   ALLOW_OS_CONVERSION gives it another rate, rounded to the nearest) is
 *   min, max and default. Another is the caller's to ask of the session it
 *   owns (setPreferredIOBufferDuration), then to read here again.
 *   `variable_callback_frames` is true: the system changes the IO buffer
 *   while a stream runs (when the screen locks, on a route change), and a
 *   converted stream's frames vary. No buffer separate from the period.
 */
typedef struct SPUDAUDIO_TIMING_CAPS {
	/** `type` is SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS; `next` is NULL (nothing is chained onto
	 * this struct yet). The caller builds the chain and sets each type; SpudAudio fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	SPUDAUDIO_PERIOD_MODE period_mode; /**< How binding a requested period is. */
	uint32_t min_period_frames;        /**< The shortest period, in frames. */
	uint32_t max_period_frames;        /**< The longest period, in frames. */
	/** Valid periods are the multiples of granularity within [min, max].
	 * WASAPI3's fundamental period; 1 where the platform reports no step. */
	uint32_t period_granularity_frames;
	uint32_t default_period_frames; /**< OS-reported default, 0 if none (ALSA). */
	/** Lower end of the range for SPUDAUDIO_STREAM_DESC::buffer_frames.
	 * Both ends are 0 when the backend has no buffer separate from the
	 * period (CoreAudio, RemoteIO, PipeWire). */
	uint32_t min_buffer_frames;
	/** Upper end of the range for SPUDAUDIO_STREAM_DESC::buffer_frames. */
	uint32_t max_buffer_frames;
	/** The callback's frame_count may differ from the period between
	 * callbacks (WASAPI shared padding, CoreAudio variable-buffer devices,
	 * RemoteIO, PipeWire). */
	bool variable_callback_frames;
} SPUDAUDIO_TIMING_CAPS;

/**
 * @brief Reads a device's timing limits for one format and sharing mode.
 *
 * A guide only; see SPUDAUDIO_TIMING_CAPS.
 *
 * @param[in]  device   The device to ask.
 * @param[in]  format   The format the limits are for.
 * @param[in]  flags    The stream flags the limits are for; exclusive vs
 *                      shared changes the answer.
 * @param[out] out_caps Receives the limits.
 *
 * @retval SPUD_SUCCESS The limits were written.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_caps` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_caps->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `out_caps->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_DESC `format` is NULL.
 * @retval SPUDRESULT_SAUD_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_SAUD_DEVICE_LOST The device disappeared since
 *         enumeration.
 * @return Another SPUDRESULT where the platform refused the format or the
 *         sharing mode, as spudaudio_probe_stream() would.
 */
SPUDRESULT spudaudio_get_timing_caps(
    spudaudio_device device,
    const SPUDAUDIO_FORMAT *format,
    SPUDAUDIO_STREAM_FLAGS flags,
    SPUDAUDIO_TIMING_CAPS *out_caps);

/**
 * @brief Per-callback glitch flags, as a set of bits.
 *
 * A stream is single-direction, so one DISCONTINUITY flag covers both
 * cases: on an OUTPUT stream it means the device ran dry (underflow) before
 * this callback; on an INPUT stream it means captured data was dropped
 * (overflow). The same events also bump SPUDAUDIO_STREAM_STATUS's counters,
 * for callers that only poll.
 */
typedef uint32_t SPUDAUDIO_CALLBACK_FLAGS;
/** @brief Bits of SPUDAUDIO_CALLBACK_FLAGS. */
enum {
	SPUDAUDIO_CALLBACK_FLAG_NONE            = 0x0, /**< Nothing to report. */
	/** Frames were lost before this buffer. WASAPI
	 * AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY / ALSA -EPIPE xrun / PipeWire
	 * xrun / CoreAudio overload / RemoteIO a render that skipped frames, or
	 * one asking for more than max_callback_frames (silence, the callback
	 * not called). */
	SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY   = 0x1,
	/** WASAPI AUDCLNT_BUFFERFLAGS_SILENT (capture) - the OS says the packet
	 * is silence; SpudAudio zero-fills `frames` so the data matches the
	 * flag. */
	SPUDAUDIO_CALLBACK_FLAG_SILENT          = 0x2,
	/** SPUDAUDIO_CALLBACK_INFO::host_time_ns is meaningful. Clear when the
	 * platform gives no timestamp for this buffer or flags it bad (WASAPI
	 * AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR). */
	SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID = 0x4
};

/** @brief What a stream callback is told about the buffer it is given. */
/* Frozen: handed to the data callback every period. It never changes; a new need gets a new type. */
typedef struct SPUDAUDIO_CALLBACK_INFO {
	/** Glitch and timestamp flags for this buffer. */
	SPUDAUDIO_CALLBACK_FLAGS flags;

	/** Position of this buffer's first frame on the stream's sample clock.
	 * Starts at 0 on each spudaudio_stream_start() and is monotonic within a
	 * run; after a DISCONTINUITY it jumps forward by the frames lost (as
	 * measured by WASAPI, or estimated from the xrun's timestamp on ALSA).
	 * WASAPI u64DevicePosition / IAudioClock, CoreAudio and RemoteIO
	 * AudioTimeStamp.mSampleTime, PipeWire pw_time.ticks, ALSA status. On a
	 * converted CoreAudio stream it counts the stream's own frames handed
	 * to the callback, and a jump is the lost time in those frames. */
	uint64_t device_position_frames;

	/** Host monotonic clock in nanoseconds - the same clock as
	 * spudperf_get_monotonic_time_ns() - at which this buffer's first frame
	 * was captured (INPUT) or will reach the endpoint (OUTPUT). Only valid
	 * when SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID is set. It is what lets
	 * a caller schedule a sound against the time of the event that caused
	 * it. Known problem: on a converted CoreAudio stream it leaves out the
	 * AudioConverter's own delay. */
	uint64_t host_time_ns;
} SPUDAUDIO_CALLBACK_INFO;

/**
 * @brief The caller's function that fills or reads a stream's frames.
 *
 * Must not block or allocate - that's the caller's contract, SpudAudio
 * doesn't guard it.
 *
 * Thread: runs on a thread that is not the caller's, while the stream
 * runs, at the same time as the caller's own calls: on WASAPI and ALSA a
 * thread SpudAudio starts in spudaudio_stream_start() and ends in
 * spudaudio_stream_stop(), which also runs the OUTPUT prefill calls (see
 * spudaudio_stream_start()); on CoreAudio the HAL's real-time IOProc
 * thread; on RemoteIO the audio unit's real-time render thread. Calls for
 * one stream never overlap each other. From inside it,
 * spudaudio_stream_get_status() is the only call on its stream allowed.
 *
 * @param[in]     stream      The stream the buffer belongs to.
 * @param[in,out] frames      The buffer. For OUTPUT, fill it; for INPUT,
 *                            read it. Interleaved: one buffer; planar: an
 *                            array of `void *`, one per channel.
 * @param[in]     frame_count How many frames the buffer holds. It can differ
 *                            between calls (WASAPI shared padding, PipeWire
 *                            quantum changes) - never assume it equals the
 *                            requested period - but never exceeds the
 *                            stream's max_callback_frames, so scratch
 *                            buffers can be sized once up front.
 * @param[in]     info        Flags and timing for this buffer. Only valid
 *                            for the duration of the call.
 * @param[in]     user_data   The SPUDAUDIO_STREAM_DESC::user_data the stream
 *                            was created with.
 */
typedef void (*SPUDAUDIO_STREAM_CALLBACK)(
    spudaudio_stream stream,
    void *frames,
    uint32_t frame_count,
    const SPUDAUDIO_CALLBACK_INFO *info,
    void *user_data);

/**
 * @brief Scheduling for the thread that runs the stream callback.
 *
 * It names the platform *mechanism* and the caller supplies its
 * parameters, because any mapping from abstract tiers ("audio", "pro
 * audio") to a SCHED_FIFO number or an MMCSS task would be SpudAudio
 * deciding. UNSPECIFIED (0) is rejected, so a zeroed desc never silently
 * gets a default.
 *
 * A mechanism the backend doesn't have fails probe/create with
 * SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED. An elevation the OS refuses
 * when the thread starts (no MMCSS task of that name, no RT permission)
 * fails spudaudio_stream_start() with
 * SPUDRESULT_SAUD_THREAD_PRIORITY_DENIED - never silently run at a lower
 * priority; retrying with NORMAL is the caller's call.
 */
typedef uint32_t SPUDAUDIO_THREAD_PRIORITY;
/** @brief Values of SPUDAUDIO_THREAD_PRIORITY. */
enum {
	/** No mechanism chosen; refused. */
	SPUDAUDIO_THREAD_PRIORITY_UNSPECIFIED = 0,
	/** SpudAudio's own thread at OS-default scheduling. WASAPI, ALSA. */
	SPUDAUDIO_THREAD_PRIORITY_NORMAL      = 1,
	/** WASAPI: AvSetMmThreadCharacteristicsW(mmcss_task) +
	 * AvSetMmThreadPriority(mmcss_relative_priority). */
	SPUDAUDIO_THREAD_PRIORITY_MMCSS       = 2,
	/** ALSA: pthread_setschedparam(SCHED_FIFO, sched_fifo_priority). Needs
	 * RLIMIT_RTPRIO / CAP_SYS_NICE; rtkit is not used (it would add a D-Bus
	 * dependency). */
	SPUDAUDIO_THREAD_PRIORITY_SCHED_FIFO  = 3,
	/** The platform's own real-time thread runs the callback and SpudAudio
	 * has no thread to configure. CoreAudio (HAL IOProc) and RemoteIO (the
	 * audio unit's render thread), where it is the only mechanism, and
	 * PipeWire (data loop) once that backend exists. */
	SPUDAUDIO_THREAD_PRIORITY_PLATFORM    = 4
};

/** @brief Room for an MMCSS task name, with its terminator. */
#define SPUDAUDIO_MAX_MMCSS_TASK_NAME 64

/** @brief Which scheduling mechanism runs the stream callback, and its parameters. */
/* Frozen: held by value in SPUDAUDIO_STREAM_DESC. It never changes; a new need gets a new type. */
typedef struct SPUDAUDIO_THREAD_DESC {
	/** The mechanism. Required; see SPUDAUDIO_THREAD_PRIORITY. */
	SPUDAUDIO_THREAD_PRIORITY priority;
	/** MMCSS only: a task under HKLM\\SOFTWARE\\Microsoft\\Windows NT\\
	 * CurrentVersion\\Multimedia\\SystemProfile\\Tasks, e.g. "Pro Audio",
	 * "Audio". UTF-8, null-terminated, copied at creation. */
	const char *mmcss_task;
	/** MMCSS only: AVRT_PRIORITY, -2 (very low) .. 2 (critical). */
	int32_t mmcss_relative_priority;
	/** SCHED_FIFO only: within sched_get_priority_min/max(SCHED_FIFO),
	 * 1..99 on Linux. */
	int32_t sched_fifo_priority;
} SPUDAUDIO_THREAD_DESC;

/**
 * @brief One stream, as the caller asks for it.
 *
 * Nothing in it has a default: a field left out is refused, not filled in.
 */
typedef struct SPUDAUDIO_STREAM_DESC {
	/** `type` is SPUD_STRUCT_TYPE_AUDIO_STREAM_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	spudaudio_device device; /**< The endpoint the stream is on. */
	SPUDAUDIO_FORMAT format; /**< The format of the frames the callback is given. */
	uint32_t period_frames;  /**< How often the callback runs, in frames. Required - no "pick something sensible" value. */

	/** Total frames the OS queues for this stream: sets latency, while
	 * period_frames sets how often the callback runs. Must be >=
	 * period_frames, and is typically a whole multiple of it.
	 *
	 * - WASAPI: shared mode (IAudioClient3, Windows 10 1607+) has no
	 *   separate buffer, must be 0. Shared mode on older Windows:
	 *   hnsBufferDuration. Exclusive mode: hnsBufferDuration, and must
	 *   equal period_frames (the device double-buffers internally).
	 * - ALSA: snd_pcm_hw_params buffer_size.
	 * - CoreAudio: no separate buffer - the I/O buffer is the period.
	 * - RemoteIO: no separate buffer - the session's IO buffer is the
	 *   period.
	 * - PipeWire (planned): no client buffer - only node.latency (the
	 *   period).
	 *
	 * Must be 0 exactly when spudaudio_get_timing_caps() reports
	 * min/max_buffer_frames of 0; any mismatch fails with
	 * SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE rather than being ignored. */
	uint32_t buffer_frames;
	SPUDAUDIO_STREAM_FLAGS flags;       /**< Sharing mode and OS conversion. */
	SPUDAUDIO_THREAD_DESC thread;       /**< Scheduling for the stream callback. Required - see SPUDAUDIO_THREAD_PRIORITY. */
	SPUDAUDIO_STREAM_CALLBACK callback; /**< The data callback. Required to create a stream; ignored by the probe. */
	void *user_data;                    /**< Handed to `callback` unchanged. Never dereferenced. */
} SPUDAUDIO_STREAM_DESC;

/**
 * @brief What a stream is (or would be) granted.
 *
 * Returned by both spudaudio_probe_stream() and
 * spudaudio_stream_get_config().
 */
typedef struct SPUDAUDIO_STREAM_CONFIG {
	/** `type` is SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG; `next` is NULL (nothing is chained onto
	 * this struct yet). The caller builds the chain and sets each type; SpudAudio fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	SPUDAUDIO_FORMAT format;           /**< The granted format; its `positions` are the stream's real channel layout. */
	SPUDAUDIO_PERIOD_MODE period_mode; /**< How binding the period is. */
	uint32_t period_frames; /**< The granted period. NEGOTIATED: the value at creation time. */
	uint32_t buffer_frames; /**< The granted buffer; 0 on backends with no separate buffer. */
	/** Hard upper bound on the callback's frame_count for the stream's
	 * whole lifetime, in every period mode.
	 *
	 * - WASAPI: IAudioClient::GetBufferSize.
	 * - ALSA: period_frames (SpudAudio's thread transfers per period).
	 * - CoreAudio: kAudioDevicePropertyBufferFrameSize /
	 *   kAudioUnitProperty_MaximumFramesPerSlice; period_frames for a
	 *   stream converted under ALLOW_OS_CONVERSION.
	 * - RemoteIO: the unit's kAudioUnitProperty_MaximumFramesPerSlice, as
	 *   the platform sets it (SpudAudio doesn't set it), in the stream's
	 *   frames; for a converted stream, restated at the stream's rate,
	 *   rounded down.
	 * - PipeWire (planned): clock.max-quantum, or node.max-latency if
	 *   lower. */
	uint32_t max_callback_frames;
} SPUDAUDIO_STREAM_CONFIG;

/**
 * @brief Asks the OS whether `desc` would be accepted exactly as written,
 * without creating a stream.
 *
 * The one authoritative check on every backend - constraints that caps
 * can't express (WASAPI exclusive alignment, ALSA driver rules) are only
 * discoverable this way. `desc->callback` and `desc->user_data` are
 * ignored.
 *
 * Each backend really configures the device with the request (WASAPI
 * Initialize on a throwaway IAudioClient, ALSA snd_pcm_hw_params on a
 * freshly opened PCM) and then releases it, so the answer is the OS's,
 * not a SpudAudio model of it. If the OS grants anything other than
 * exactly what was asked, that's a rejection.
 *
 * On a rejection of the format, period or buffer, checking stops at the
 * first one, in the order format -> period -> buffer. One exception: ALSA
 * can only confirm channel positions once the configuration is installed,
 * so a layout it can't apply is a SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED
 * reported after period and buffer. `out_config` then holds the OS's
 * nearest acceptable value for every field checked so far (including the
 * rejected one), and 0 for fields not reached or that the OS offered
 * nothing for - so resubmitting a suggestion can surface the next
 * constraint. It's a suggestion only: SpudAudio never applies it. Where the
 * suggestion comes from:
 *
 * - WASAPI: IsFormatSupported closest match (shared); GetBufferSize after
 *   AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED (exclusive);
 *   GetSharedModeEnginePeriod range/step (IAudioClient3); whatever
 *   Initialize actually granted.
 * - ALSA: snd_pcm_hw_params_set_{format_first,channels,rate,
 *   period_size,buffer_size}_near on a scratch hw_params; the first
 *   matching snd_pcm_query_chmaps entry, or the PCM's current map
 *   (snd_pcm_get_chmap), for positions.
 * - CoreAudio: BufferFrameSizeRange clamp; device nominal format. With
 *   ALLOW_OS_CONVERSION, a format that isn't the device's is put to
 *   AudioConverterNew, and the period is checked as the device frames it
 *   comes to.
 * - RemoteIO: the route's format; with ALLOW_OS_CONVERSION, a format that
 *   isn't the route's is set on a RemoteIO unit made for the probe. The
 *   period must be the session's IO buffer in force, as the timing caps
 *   give it, which is the suggestion; where that IO buffer is longer than
 *   the unit renders at once (its MaximumFramesPerSlice), no period is
 *   suggested (0). Nothing of the session is changed.
 * - PipeWire (planned): clamp to clock.min/max-quantum.
 *
 * Probing briefly opens the device (exclusively, in exclusive mode).
 * Acceptance isn't a reservation: creation can still fail if the device
 * changes in between.
 *
 * @param[in]  desc       The stream to ask about.
 * @param[out] out_config Receives what creation would grant, on success, or
 *                        the OS's suggestion, on a rejection.
 *
 * @retval SPUD_SUCCESS Accepted; `out_config` is what creation would grant.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_config` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_config->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG; or, checked after `desc` is
 *         found not NULL, `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_STREAM_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `out_config->base.next` is not
 *         NULL; or, checked after `desc->base.type`, `desc->base.next` is not
 *         NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_SAUD_INVALID_DEVICE `desc->device` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS The format's channel count,
 *         rate or sample format is out of range, its positions break the
 *         layout rules, or `desc->thread.priority` is UNSPECIFIED or not a
 *         SPUDAUDIO_THREAD_PRIORITY value.
 * @retval SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED The endpoint can't take the
 *         format as written.
 * @retval SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE `desc->period_frames` is 0 or
 *         a period the OS doesn't grant.
 * @retval SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE `desc->buffer_frames` is below
 *         the period, set where the backend has no separate buffer, or a
 *         size the OS doesn't grant.
 * @retval SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED The backend doesn't
 *         have the scheduling mechanism asked for.
 * @retval SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE The device is held by
 *         another client (WASAPI AUDCLNT_E_DEVICE_IN_USE /
 *         EXCLUSIVE_MODE_NOT_ALLOWED, ALSA -EBUSY, CoreAudio hog mode), or
 *         the backend has no exclusive mode (ALSA - exclusivity there is
 *         the choice of a hw: device name, not a flag; RemoteIO).
 * @retval SPUDRESULT_SAUD_OS_CONVERSION_UNAVAILABLE ALLOW_OS_CONVERSION was
 *         set where the platform can't convert for this request (WASAPI
 *         exclusive mode, an ALSA PCM with no conversion plugin). Retry
 *         without it.
 * @retval SPUDRESULT_SAUD_ACCESS_DENIED RemoteIO: an INPUT stream, and
 *         record permission is not granted (undetermined or denied;
 *         AVAudioApplication.recordPermission). Asking for it is the
 *         caller's.
 * @retval SPUDRESULT_NOT_IMPLEMENTED_YET This build has no native audio
 *         API; or, RemoteIO, the route has more than one port in the
 *         stream's direction (AVAudioSessionCategoryMultiRoute), where one
 *         port's channels would have to be chosen by a channel map, which
 *         is not built.
 * @retval SPUDRESULT_SAUD_DEVICE_LOST The device disappeared since
 *         enumeration (RemoteIO: its port is not in the current route).
 */
SPUDRESULT spudaudio_probe_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    SPUDAUDIO_STREAM_CONFIG *out_config);

/**
 * @brief Creates a stream, configured but stopped.
 *
 * Same rules as the probe - both run the identical configuration path, so
 * a desc the probe accepts creates exactly the probed configuration, and
 * one it rejects fails here with the same error. Never rounds or
 * substitutes.
 *
 * The new stream is configured but STOPPED, and already holds the device
 * the way it will while running: an exclusive-mode WASAPI stream owns the
 * endpoint, an ALSA hw: PCM is busy to other processes, and an ALSA layout
 * is applied to the PCM's channel map (the previous map is restored by
 * spudaudio_destroy_stream()).
 *
 * RemoteIO: makes and initializes a RemoteIO audio unit for the route, and
 * changes nothing of the session. Each stream is its own unit; the route
 * plays every running one.
 *
 * @param[in]  desc       The stream to create. `desc->callback` is
 *                        required. Only read during the call;
 *                        `desc->thread.mmcss_task` is copied.
 * @param[out] out_stream Receives the new stream, the caller's to destroy
 *                        with spudaudio_destroy_stream().
 *
 * @retval SPUD_SUCCESS The stream was created.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_stream` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `desc->callback` is NULL, or
 *         one of the reasons spudaudio_probe_stream() gives.
 * @retval SPUDRESULT_OUT_OF_MEMORY The stream could not be allocated.
 * @return Otherwise whatever spudaudio_probe_stream() returns for the same
 *         desc.
 */
SPUDRESULT spudaudio_create_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    spudaudio_stream *out_stream);

/**
 * @brief Stops a stream if it is running, releases the device and frees the
 * stream.
 *
 * Must not be called from inside the stream's own callback.
 *
 * @param[in] stream The stream to destroy.
 */
void spudaudio_destroy_stream(spudaudio_stream stream);

/**
 * @brief Starts a stream; returns once it is running, or with the reason it
 * couldn't start.
 *
 * On backends with their own thread (WASAPI, ALSA), launches it, applies
 * its priority, and for OUTPUT prefills the device buffer by calling the
 * callback (those prefill calls run before the device clock starts, so
 * they carry no HOST_TIME_VALID) before starting the device. On
 * PLATFORM-thread backends (CoreAudio, RemoteIO) it starts the device (the
 * audio unit, AudioOutputUnitStart) and the platform's real-time thread
 * pulls from the callback - no prefill.
 *
 * Thread: on WASAPI and ALSA this starts a thread of SpudAudio's own,
 * which runs the stream callback until spudaudio_stream_stop() or
 * spudaudio_destroy_stream() ends it (see SPUDAUDIO_STREAM_CALLBACK).
 *
 * A stream that couldn't start is left STOPPED (or in its terminal state).
 * Starting a RUNNING stream is a no-op.
 *
 * Underruns/overruns don't stop a stream: the backend recovers (WASAPI and
 * CoreAudio do so themselves; ALSA re-prepares and restarts) and the next
 * callback carries SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY.
 *
 * Start, stop and destroy must not be called from inside the stream's own
 * callback. Calls on one stream are one caller at a time, as for every
 * object (@ref spud_threads "Threads").
 *
 * @param[in] stream The stream to start.
 *
 * @retval SPUD_SUCCESS The stream is running.
 * @retval SPUDRESULT_SAUD_INVALID_STREAM `stream` is NULL.
 * @retval SPUDRESULT_GENERAL_FAILURE Called from inside the stream's own
 *         callback.
 * @retval SPUDRESULT_SAUD_THREAD_PRIORITY_DENIED The OS refused the
 *         callback thread its scheduling (no MMCSS task of that name, no
 *         real-time permission).
 * @retval SPUDRESULT_SAUD_DEVICE_LOST The device is gone.
 * @retval SPUDRESULT_SAUD_INTERRUPTED RemoteIO: the session is interrupted,
 *         or another app holds audio that doesn't mix with this session's
 *         category (AVAudioSessionErrorCodeCannotInterruptOthers,
 *         CannotStartPlaying, InsufficientPriority). The stream is left
 *         STOPPED.
 * @retval SPUDRESULT_SAUD_SESSION_INACTIVE RemoteIO: the platform won't
 *         start a unit on the inactive session
 *         (AVAudioSessionErrorCodeSessionNotActive). The stream is left
 *         STOPPED.
 * @retval SPUDRESULT_SAUD_ACCESS_DENIED RemoteIO: an INPUT stream, and
 *         record permission was taken back since creation.
 * @return For a stream already in SPUDAUDIO_STREAM_STATE_DEVICE_LOST or
 *         SPUDAUDIO_STREAM_STATE_ERROR, the error stored in its status;
 *         otherwise another SPUDRESULT from the platform.
 */
SPUDRESULT spudaudio_stream_start(spudaudio_stream stream);

/**
 * @brief Stops a stream; returns once no callback is running or will run.
 *
 * Stops the device and ends the thread; the stream can be started again.
 * On a stream that already ended in DEVICE_LOST/ERROR it just reaps the
 * thread. Must not be called from inside the stream's own callback.
 *
 * @param[in] stream The stream to stop.
 *
 * @retval SPUD_SUCCESS The stream is stopped.
 * @retval SPUDRESULT_SAUD_INVALID_STREAM `stream` is NULL.
 * @retval SPUDRESULT_GENERAL_FAILURE Called from inside the stream's own
 *         callback.
 */
SPUDRESULT spudaudio_stream_stop(spudaudio_stream stream);

/**
 * @brief Reads what the stream was granted at creation.
 *
 * @param[in]  stream     The stream to ask.
 * @param[out] out_config Receives the granted configuration.
 *
 * @retval SPUD_SUCCESS The configuration was written.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_config` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_config->base.type` is not
 *         SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `out_config->base.next` is not NULL.
 * @retval SPUDRESULT_SAUD_INVALID_STREAM `stream` is NULL.
 */
SPUDRESULT spudaudio_stream_get_config(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_CONFIG *out_config);

/**
 * @brief Reads the stream's current end-to-end latency.
 *
 * Can change while running on NEGOTIATED backends; for per-buffer timing
 * use SPUDAUDIO_CALLBACK_INFO::host_time_ns.
 *
 * CoreAudio: the HAL's device latency + safety offset + I/O buffer + the
 * first stream's latency. Known problem: on a converted CoreAudio stream
 * that total is restated in the stream's frames and leaves out the
 * AudioConverter's own delay.
 *
 * RemoteIO: the session's outputLatency (OUTPUT) or inputLatency (INPUT)
 * plus its IOBufferDuration, at the stream's rate, as the session reports
 * them now. Known problem: on a converted stream it leaves out the unit's
 * converter's own delay.
 *
 * @param[in]  stream             The stream to ask.
 * @param[out] out_latency_frames Receives the latency, in the stream's
 *                                frames.
 *
 * @retval SPUD_SUCCESS The latency was written.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_latency_frames` is NULL.
 * @retval SPUDRESULT_SAUD_INVALID_STREAM `stream` is NULL.
 * @return Another SPUDRESULT where the platform failed to report it.
 */
SPUDRESULT spudaudio_stream_get_latency_frames(
    spudaudio_stream stream,
    uint32_t *out_latency_frames);

/** @} */

/**
 * @defgroup spudaudio_status Stream status
 * @ingroup spudaudio
 * @brief A stream's state and glitch counts - polled, not pushed.
 *
 * A dying device stops the stream callback, so the callback can never
 * deliver its own failure, and a failure listener would run somewhere
 * different per backend (COM's notification thread, a CoreAudio listener
 * queue, the PipeWire loop, SpudAudio's own stream thread). Instead the
 * backend records failures atomically and the caller reads them whenever
 * it likes, e.g. once per frame.
 *
 * Thread: the backend writes a stream's status from those places while
 * the caller's calls run; spudaudio_stream_get_status() reads it without
 * a lock.
 *
 * RemoteIO writes it from a serial dispatch queue of the stream's, on the
 * session's notifications, and stops the unit there:
 * - its port leaves the route (AVAudioSessionRouteChangeNotification), or
 *   the media services are lost or reset
 *   (AVAudioSessionMediaServicesWereLostNotification /
 *   ...WereResetNotification): DEVICE_LOST, SPUDRESULT_SAUD_DEVICE_LOST;
 * - the session's sampleRate, or the port's channel count, is no longer
 *   the granted format's: ERROR, SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
 * - an interruption begins while the stream runs
 *   (AVAudioSessionInterruptionNotification): ERROR,
 *   SPUDRESULT_SAUD_INTERRUPTED. Its end changes nothing here.
 *
 * Nothing is lost between polls: DEVICE_LOST and ERROR are terminal and
 * stay set until the stream is destroyed, and glitches are monotonic
 * counters - diff against the previous poll.
 * @{
 */

/** @brief What a stream is doing, or how it ended. */
typedef uint32_t SPUDAUDIO_STREAM_STATE;
/** @brief Values of SPUDAUDIO_STREAM_STATE. */
enum {
	SPUDAUDIO_STREAM_STATE_STOPPED     = 0, /**< Configured and not running. */
	SPUDAUDIO_STREAM_STATE_RUNNING     = 1, /**< The callback is being called. */
	SPUDAUDIO_STREAM_STATE_DEVICE_LOST = 2, /**< Terminal: destroy and recreate. */
	SPUDAUDIO_STREAM_STATE_ERROR       = 3  /**< Terminal: see SPUDAUDIO_STREAM_STATUS::error. */
};

/** @brief A stream's state and glitch counts at the moment of the poll. */
/* Frozen: polled; a new need gets a new query. It never changes; a new need gets a new type. */
typedef struct SPUDAUDIO_STREAM_STATUS {
	SPUDAUDIO_STREAM_STATE state; /**< What the stream is doing. */
	SPUDRESULT error;             /**< Cause of DEVICE_LOST / ERROR, else SPUD_SUCCESS. */
	/** OUTPUT: how many times the device ran out of frames. Monotonic; one
	 * increment per callback that carried
	 * SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY. Only the counter matching the
	 * stream's direction ever moves. */
	uint64_t underflow_count;
	/** INPUT: how many times captured frames were dropped. Counted as
	 * `underflow_count` is. */
	uint64_t overflow_count;
} SPUDAUDIO_STREAM_STATUS;

/**
 * @brief Reads a stream's status.
 *
 * Thread: may overlap any other call on the same stream except
 * spudaudio_destroy_stream(), and may be called from inside the stream
 * callback. Lock-free.
 *
 * @param[in]  stream     The stream to ask.
 * @param[out] out_status Receives the status.
 *
 * @retval SPUD_SUCCESS The status was written.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_status` is NULL.
 * @retval SPUDRESULT_SAUD_INVALID_STREAM `stream` is NULL.
 */
SPUDRESULT spudaudio_stream_get_status(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_STATUS *out_status);

/** @} */

/**
 * @defgroup spudaudio_events Device-change notification
 * @ingroup spudaudio
 * @brief Being told when endpoints come, go or change default - reported,
 * never acted on. Known problem: on ALSA, events depend on a running udev
 * daemon; without one (some containers) none is delivered and nothing says
 * so. A lost device is always seen by polling
 * spudaudio_stream_get_status().
 *
 * An event is the platform's own notification, passed on:
 *
 * | Backend   | ADDED / REMOVED from                              | DEFAULT_CHANGED from                    |
 * |-----------|---------------------------------------------------|-----------------------------------------|
 * | WASAPI    | IMMNotificationClient (endpoint state, add, remove) | IMMNotificationClient::OnDefaultDeviceChanged |
 * | CoreAudio | kAudioHardwarePropertyDevices                     | kAudioHardwarePropertyDefault{Output,SystemOutput,Input}Device |
 * | ALSA      | udev "sound" events, then the PCM name hints      | never - ALSA has no default to change   |
 * | RemoteIO  | AVAudioSessionRouteChangeNotification, and the media services' reset, then the route's ports | the same: the GENERAL port of a direction |
 *
 * What each backend can't see:
 * - CoreAudio: a device that stays but gains or loses its streams in one
 *   direction is not reported until the device list next changes.
 * - ALSA: a PCM name that comes from the ALSA configuration and not from a
 *   card only changes when a card does, since udev is the only trigger.
 *
 * ADDED and REMOVED are about the list spudaudio_enumerate_devices()
 * returns: ADDED is an endpoint that list now has and didn't, REMOVED one it
 * had and no longer does. Where the platform only says that something
 * changed (CoreAudio, ALSA, RemoteIO), the backend compares the endpoints against the
 * ones it last saw and reports each difference. An endpoint that is both an
 * output and an input is two endpoints here, as it is when enumerated, and
 * gets an event per direction.
 *
 * Events start from the endpoints present when the callback is set: nothing
 * is reported for those. Set the callback first and enumerate second, and a
 * change in between shows up as an event for something the enumeration
 * already reflects, never as a change that was missed.
 *
 * An event only says that something changed. To get a handle for an added
 * endpoint, enumerate again: endpoints that are still there keep the handles
 * they had. Enumerations of one instance are not synchronized with each
 * other, so do that where the caller enumerates, not inside the callback.
 *
 * Events and streams are independent. A stream on a removed device moves to
 * SPUDAUDIO_STREAM_STATE_DEVICE_LOST by itself (see
 * spudaudio_stream_get_status()), before or after REMOVED is reported, and
 * the caller decides whether and where to reopen it.
 * @{
 */

/** @brief What happened to an endpoint. */
typedef uint32_t SPUDAUDIO_DEVICE_EVENT;
/** @brief Values of SPUDAUDIO_DEVICE_EVENT. */
enum {
	SPUDAUDIO_DEVICE_EVENT_ADDED           = 0, /**< An endpoint appeared: spudaudio_enumerate_devices() now returns it. */
	SPUDAUDIO_DEVICE_EVENT_REMOVED         = 1, /**< An endpoint went away: spudaudio_enumerate_devices() no longer returns it. */
	SPUDAUDIO_DEVICE_EVENT_DEFAULT_CHANGED = 2  /**< A default role now points at another endpoint, or at none. Never reported on ALSA. */
};

/**
 * @brief The caller's function that is told of a device change.
 *
 * Thread: runs on a thread that is not the caller's, at the same time as
 * the caller's own calls - the system's notification thread on WASAPI, a
 * serial dispatch queue the backend makes on CoreAudio and on RemoteIO, a
 * thread the backend starts for it on ALSA - and not necessarily the same
 * thread
 * each time. Calls for one instance never overlap each other. Don't block
 * in it.
 *
 * It must not call spudaudio_set_device_event_callback() or
 * spudaudio_destroy_instance() on its own instance, which wait for it to
 * return, and must not call spudaudio_enumerate_devices(), which is not
 * synchronized with the caller's own enumerations. Hand the event to the
 * caller's own code and return.
 *
 * @param[in] event         What happened.
 * @param[in] persistent_id The endpoint's
 *                          SPUDAUDIO_DEVICE_PROPERTIES::persistent_id. Only
 *                          valid for the duration of the call. NULL for a
 *                          DEFAULT_CHANGED that leaves `roles` with no
 *                          endpoint at all; never NULL for ADDED/REMOVED.
 * @param[in] direction     Whether it is an output or an input endpoint;
 *                          for DEFAULT_CHANGED, the direction whose default
 *                          changed.
 * @param[in] roles         For DEFAULT_CHANGED, the role(s) that now point
 *                          at `persistent_id` (WASAPI reports one role per
 *                          change, CoreAudio one per default property);
 *                          SPUDAUDIO_DEFAULT_ROLE_NONE for ADDED/REMOVED.
 * @param[in] user_data     The `user_data` given to
 *                          spudaudio_set_device_event_callback().
 */
typedef void (*SPUDAUDIO_DEVICE_EVENT_CALLBACK)(
    SPUDAUDIO_DEVICE_EVENT event,
    const char *persistent_id,
    SPUDAUDIO_DIRECTION direction,
    SPUDAUDIO_DEFAULT_ROLES roles,
    void *user_data);

/**
 * @brief Sets the function an instance calls on a device change.
 *
 * An instance has one callback or none. A non-NULL `callback` starts
 * notification, replacing any callback already set, and events are counted
 * from the endpoints present at that moment. A NULL `callback` stops it;
 * with none set that is a no-op.
 *
 * Returns once the callback it replaced or cleared is not running and will
 * not run again, so the caller may then free whatever its `user_data`
 * pointed at. Because it waits, it must not be called from inside the
 * instance's device-event callback. If it fails, the callback that was set
 * stays set.
 *
 * Thread: a non-NULL `callback` starts the watch of the device list that
 * calls it (see SPUDAUDIO_DEVICE_EVENT_CALLBACK); on ALSA that is a thread
 * the backend starts here and ends when the callback is cleared or the
 * instance destroyed.
 *
 * @param[in] instance  The instance whose devices to be told about.
 * @param[in] callback  The function to call, or NULL to stop being told.
 * @param[in] user_data Handed to `callback` unchanged. Never dereferenced.
 *
 * @retval SPUD_SUCCESS `callback` is now the instance's callback, or
 *         notification is stopped.
 * @retval SPUDRESULT_SAUD_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_GENERAL_FAILURE Called from inside the instance's
 *         device-event callback.
 * @retval SPUDRESULT_OUT_OF_MEMORY The backend's record of the endpoints
 *         could not be allocated.
 * @retval SPUDRESULT_NOT_IMPLEMENTED_YET This build has no native audio
 *         API.
 * @return Another SPUDRESULT where the platform refused the registration
 *         (WASAPI RegisterEndpointNotificationCallback, a CoreAudio
 *         property listener) or the backend couldn't start listening (ALSA:
 *         the udev monitor or the watcher thread; RemoteIO: its queue).
 */
SPUDRESULT spudaudio_set_device_event_callback(
    spudaudio_instance instance,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // SPUDLIB_SPUDAUDIO_H

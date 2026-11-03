#if SPUDAUDIO_COMPILE_COREAUDIO

/*
 * CoreAudio backend for macOS, on the HAL (AudioObject* / AudioDevice*).
 *
 * The HAL is macOS-only: iOS, iPadOS and watchOS have no AudioObject API.
 * iOS and iPadOS have their own backend (spudaudioremoteio.c, RemoteIO on
 * AVAudioSession), sharing the platform-neutral pieces in
 * spudaudioapple.{h,c}; watchOS has none yet. This whole file is
 * SPUDAUDIO_COMPILE_COREAUDIO, which spudaudio.h only allows with
 * SPUDAUDIO_PLATFORM_MACOS.
 *
 * Model:
 * - A SpudAudio device is a HAL device in one direction (scope): one HAL
 *   device with both input and output streams enumerates once per
 *   direction, under the same UID.
 * - Streams are HAL IOProcs. The HAL's own real-time thread calls them, so
 *   the thread priority must be SPUDAUDIO_THREAD_PRIORITY_PLATFORM.
 * - Clients see each device stream's virtual format, which for mixable
 *   streams is always Float32 at the device's nominal rate. So F32 is the
 *   only sample format, the rate is the device's (spudaudio_set_device_
 *   sample_rate changes it), and the channel count is the device's.
 * - With ALLOW_OS_CONVERSION a stream in any other format goes through an
 *   AudioConverter (AudioToolbox) in the IOProc. The converter is given the
 *   two formats and nothing else - no channel map, no mix, no quality
 *   setting - so what it does with them is its own default. See "A
 *   converting stream" above ca_ioproc.
 * - A device can expose several HAL streams in one direction (a buffer
 *   each in the IOProc's AudioBufferList); they're presented as one
 *   SpudAudio channel range, rearranged (never converted) through a
 *   scratch buffer.
 * - The period is kAudioDevicePropertyBufferFrameSize, which the HAL keeps
 *   per process per device: two SpudAudio streams on the same device in
 *   one process share it.
 * - EXCLUSIVE is hog mode.
 * - Device-change notification is listeners on the system object: the
 *   device list and the three default-device properties. The device list
 *   is the only trigger for ADDED/REMOVED, so a device that stays but gains
 *   or loses its streams in one direction is not reported until the list
 *   next changes.
 */

#include "spudaudioapple.h"

#include <Block.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../../spudallocator.h"
#include "../../../spudstructbase.h"

#if __cplusplus
extern "C" {
#endif

struct spudaudio_device_t {
	spud_object_head head; // allocator: a copy of its instance's
	AudioObjectID id;
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256]; // kAudioDevicePropertyDeviceUID
	struct spudaudio_device_t *next; // instance-owned list
};

// One endpoint as the device-change watch last saw it. The UID is kept
// because a device that has gone can no longer be asked for it.
struct ca_endpoint {
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256]; // kAudioDevicePropertyDeviceUID
};

/*
 * Device-change notification for one instance: exists while a callback is
 * set. The HAL only says that the device list changed, so the watch keeps
 * the endpoints it last saw and reports the difference. That record is the
 * watch's own and is only touched on its queue - never the instance's
 * device list, which belongs to the thread that enumerates.
 */
struct ca_watch {
	// The instance's: the watch and its endpoint lists are its memory.
	SPUD_ALLOCATOR allocator;
	SPUDAUDIO_DEVICE_EVENT_CALLBACK callback;
	void *user_data;
	// Listeners run on this private serial queue, so destroy can drain it
	// after removing them.
	dispatch_queue_t queue;
	AudioObjectPropertyListenerBlock listener;
	bool listening;
	// Queue-only state.
	bool have_baseline; // changes before the baseline is taken are not events
	SPUDRESULT baseline;
	struct ca_endpoint *endpoints;
	uint32_t endpoint_count;
	uint32_t endpoint_capacity; // what `endpoints` was allocated, and is freed, with
};

struct spudaudio_instance_t {
	spud_object_head head;
	struct spudaudio_device_t *devices; // every device ever enumerated
	spudaudio_device *enumerated[2];    // latest array per direction
	uint32_t enumerated_capacity[2];    // what each was allocated, and is freed, with
	struct ca_watch *watch;             // NULL while no device-event callback is set
};

struct spudaudio_stream_t {
	spud_object_head head; // allocator: a copy of its device's
	AudioObjectID device;
	SPUDAUDIO_DIRECTION direction;
	SPUDAUDIO_STREAM_CONFIG config;
	SPUDAUDIO_STREAM_CALLBACK callback;
	void *user_data;
	AudioDeviceIOProcID proc;
	bool hogged;
	mach_timebase_info_data_t timebase;
	float *scratch;   // max_callback_frames x channels; converting: device_max_frames x device channels, interleaved
	float **planes;   // planar: one pointer per channel into scratch; unused when converting
	size_t scratch_count;           // floats in scratch, which it is freed with
	uint32_t planes_count;          // pointers in planes
	SPUDAUDIO_FORMAT device_format; // the device's own format when the stream was created
	uint32_t device_max_frames;     // the most frames one IOProc call brings

	// ALLOW_OS_CONVERSION, when the stream's format isn't the device's: an
	// AudioConverter between the two. NULL otherwise, and the rest of this
	// group unused. The callback then always gets one whole period, held in
	// `client`, however many frames the device asked for or brought.
	AudioConverterRef converter;
	uint8_t *client;             // one period in the stream's format
	void **client_planes;        // planar: one pointer per channel into client
	uint32_t client_sample_bytes; // bytes of one sample
	size_t client_samples;        // samples in client (frames x channels), which it is freed with
	uint32_t client_planes_count; // pointers in client_planes

	// Device-change listeners (alive, nominal rate) run on this private
	// serial queue, so destroy can drain it after removing them.
	dispatch_queue_t listener_queue;
	AudioObjectPropertyListenerBlock listener;
	bool listening;

	atomic_uint state; // SPUDAUDIO_STREAM_STATE
	atomic_int error;  // SPUDRESULT
	atomic_uint_least64_t underflow_count;
	atomic_uint_least64_t overflow_count;

	// Which thread is inside the callback right now, for the start/stop
	// re-entrancy guard. The HAL's IOProc thread isn't known in advance.
	atomic_bool in_callback;
	pthread_t callback_thread;

	// IOProc-only state.
	bool have_origin;
	double origin_sample_time;   // device sample time of the first frame since start
	double expected_sample_time; // where the next buffer should start
	uint64_t position_frames;    // fallback when a timestamp has no sample time

	// IOProc-only state of a converting stream.
	uint32_t client_used;        // OUTPUT: frames of `client` the converter has taken; a full period = empty
	uint32_t client_filled;      // INPUT: frames of `client` the converter has written
	uint32_t device_given;       // INPUT: frames of scratch the converter has taken this cycle
	uint32_t device_left;        // INPUT: frames of scratch it hasn't
	uint64_t client_position;    // stream frames handed to the callback since start
	bool pending_discontinuity;  // the device skipped; flag the next callback
	bool cycle_host_valid;       // this IO cycle's timestamp has a host time
	uint64_t cycle_host_ns;      // that host time
	int64_t cycle_offset_frames; // where the next callback's first frame sits relative to it, in stream frames
};

// --------------------------------------------------------------------------
// HAL helpers
// --------------------------------------------------------------------------

static AudioObjectPropertyScope ca_scope(SPUDAUDIO_DIRECTION d) {
	return d == SPUDAUDIO_DIRECTION_OUTPUT ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
}

static OSStatus ca_get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 size, void *out) {
	AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
	return AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, out);
}

static OSStatus ca_set(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 size, const void *in) {
	AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
	return AudioObjectSetPropertyData(obj, &a, 0, NULL, size, in);
}

// A variable-size property, in a block of the caller's allocator.
struct ca_property {
	void *data;      // NULL if absent, empty or unreadable
	UInt32 size;     // bytes the HAL wrote
	UInt32 capacity; // bytes allocated, which the block is freed with
};

// The HAL may write fewer bytes than it said it would, so `size` can be
// below `capacity`; the property's contents are the first `size` bytes.
static struct ca_property ca_get_alloc(
    const SPUD_ALLOCATOR *allocator,
    AudioObjectID obj,
    AudioObjectPropertySelector sel,
    AudioObjectPropertyScope scope) {
	struct ca_property p         = {NULL, 0, 0};
	AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
	UInt32 size                  = 0;
	if (!AudioObjectHasProperty(obj, &a) || AudioObjectGetPropertyDataSize(obj, &a, 0, NULL, &size) != noErr || size == 0)
		return p;
	// Cast to HAL structs (AudioBufferList, AudioChannelLayout, ...).
	void *data = spud_allocate(allocator, size, SPUD_MAX_ALIGN);
	if (!data)
		return p;
	const UInt32 capacity = size;
	if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, data) != noErr) {
		spud_free(allocator, data, capacity, SPUD_MAX_ALIGN);
		return p;
	}
	p.data     = data;
	p.size     = size;
	p.capacity = capacity;
	return p;
}

// NULL data is nothing to free.
static void ca_property_free(
    const SPUD_ALLOCATOR *allocator,
    const struct ca_property *p) {
	spud_free(allocator, p->data, p->capacity, SPUD_MAX_ALIGN);
}

static void ca_get_string(AudioObjectID obj, AudioObjectPropertySelector sel, char *out, size_t out_size) {
	out[0]       = '\0';
	CFStringRef s = NULL;
	if (ca_get(obj, sel, kAudioObjectPropertyScopeGlobal, sizeof(CFStringRef), &s) == noErr && s) {
		CFStringGetCString(s, out, (CFIndex)out_size, kCFStringEncodingUTF8);
		CFRelease(s);
	}
}

static SPUDRESULT ca_result(OSStatus status) {
	switch (status) {
	case noErr:
		return SPUD_SUCCESS;
	case kAudioHardwareBadDeviceError:
	case kAudioHardwareBadObjectError:
	case kAudioHardwareBadStreamError:
		return SPUDRESULT_SAUD_DEVICE_LOST;
	case kAudioDevicePermissionsError: // another process holds hog mode
		return SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;
	case kAudioHardwareIllegalOperationError:
	case kAudioDeviceUnsupportedFormatError:
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	default:
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

// Total channels in one direction, summed over the device's HAL streams,
// and whether they arrive as a single interleaved buffer.
static uint32_t ca_channel_count(
    const SPUD_ALLOCATOR *allocator,
    AudioObjectID dev,
    SPUDAUDIO_DIRECTION dir,
    bool *out_single_buffer) {
	const struct ca_property p  = ca_get_alloc(allocator, dev, kAudioDevicePropertyStreamConfiguration, ca_scope(dir));
	const AudioBufferList *list = (const AudioBufferList *)p.data;
	uint32_t channels           = 0;
	if (list) {
		for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
			channels += list->mBuffers[b].mNumberChannels;
		if (out_single_buffer)
			*out_single_buffer = list->mNumberBuffers == 1;
	}
	ca_property_free(allocator, &p);
	return channels;
}

static uint32_t ca_nominal_rate(AudioObjectID dev) {
	Float64 rate = 0;
	ca_get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(rate), &rate);
	return (uint32_t)lround(rate);
}

// The device's own format in one direction, as clients see it (the HAL
// virtual format: Float32 at the nominal rate), with its preferred channel
// layout as positions.
static void ca_device_format(
    const SPUD_ALLOCATOR *allocator,
    AudioObjectID dev,
    SPUDAUDIO_DIRECTION dir,
    SPUDAUDIO_FORMAT *out) {
	memset(out, 0, sizeof(*out));
	bool single         = true;
	out->sample_format  = SPUDAUDIO_SAMPLE_FORMAT_F32;
	out->sample_rate    = ca_nominal_rate(dev);
	out->channel_count  = ca_channel_count(allocator, dev, dir, &single);
	out->interleaved    = single || out->channel_count == 1;
	if (out->channel_count > SPUDAUDIO_MAX_CHANNELS)
		out->channel_count = SPUDAUDIO_MAX_CHANNELS;
	const struct ca_property layout = ca_get_alloc(allocator, dev, kAudioDevicePropertyPreferredChannelLayout, ca_scope(dir));
	spudaudio_apple_layout_to_positions(allocator, (const AudioChannelLayout *)layout.data, out);
	ca_property_free(allocator, &layout);
}

// A SpudAudio format as linear PCM in native byte order. False for a sample
// format with no PCM description.
static bool ca_format_to_asbd(const SPUDAUDIO_FORMAT *format, AudioStreamBasicDescription *out) {
	UInt32 bits            = 0;
	AudioFormatFlags flags = 0;
	switch (format->sample_format) {
	case SPUDAUDIO_SAMPLE_FORMAT_S16:
		bits  = 16;
		flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED:
		bits  = 24;
		flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB:
		bits  = 24;
		flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsAlignedHigh;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_32_LSB:
		bits  = 24;
		flags = kAudioFormatFlagIsSignedInteger; // neither packed nor high: the low 3 bytes
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S32:
		bits  = 32;
		flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_F32:
		bits  = 32;
		flags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		break;
	default:
		return false;
	}
	const UInt32 sample_bytes = spudaudio_sample_format_byte_size(format->sample_format);
	memset(out, 0, sizeof(*out));
	out->mFormatID         = kAudioFormatLinearPCM;
	out->mSampleRate       = format->sample_rate;
	out->mFormatFlags      = flags | kAudioFormatFlagsNativeEndian | (format->interleaved ? 0 : kAudioFormatFlagIsNonInterleaved);
	out->mChannelsPerFrame = format->channel_count;
	out->mBitsPerChannel   = bits;
	out->mFramesPerPacket  = 1;
	// Non-interleaved: each buffer holds one channel, so a "frame" is a sample.
	out->mBytesPerFrame  = format->interleaved ? sample_bytes * format->channel_count : sample_bytes;
	out->mBytesPerPacket = out->mBytesPerFrame;
	return true;
}

// frames at rate `from` as frames at rate `to`. round: -1 down, 0 nearest,
// 1 up.
static uint32_t ca_scale_frames(uint32_t frames, uint32_t from, uint32_t to, int round) {
	const uint64_t scaled = (uint64_t)frames * to;
	if (round > 0)
		return (uint32_t)((scaled + from - 1) / from);
	if (round == 0)
		return (uint32_t)((scaled + from / 2) / from);
	return (uint32_t)(scaled / from);
}

// Whether a stream of `format` on a device of `device_format` goes through
// an AudioConverter. Interleaving is never a reason: the IOProc rearranges.
static bool ca_format_needs_conversion(const SPUDAUDIO_FORMAT *format, const SPUDAUDIO_FORMAT *device_format) {
	return format->sample_format != SPUDAUDIO_SAMPLE_FORMAT_F32 || format->sample_rate != device_format->sample_rate ||
	       format->channel_count != device_format->channel_count;
}

/*
 * Where each channel of a converted stream goes. No channel map and no mix
 * are set on the converter, so its default applies: stream channel n is
 * device channel n, and channels past the device's are discarded (OUTPUT)
 * or silent (INPUT) - NA. No layout where the device has none, or where its
 * one channel is MONO and the stream has more (MONO can't sit in a layout).
 */
static void ca_converted_positions(const SPUDAUDIO_FORMAT *device_format, uint32_t channel_count, SPUDAUDIO_CHANNEL_POSITION *out) {
	memset(out, SPUDAUDIO_CHANNEL_POSITION_UNSPECIFIED, SPUDAUDIO_MAX_CHANNELS * sizeof(*out));
	if (!spudaudio_format_has_layout(device_format))
		return;
	if (device_format->positions[0] == SPUDAUDIO_CHANNEL_POSITION_MONO && channel_count > 1)
		return;
	for (uint32_t ch = 0; ch < channel_count; ++ch)
		out[ch] = ch < device_format->channel_count ? device_format->positions[ch] : SPUDAUDIO_CHANNEL_POSITION_NA;
}

static void ca_sleep_ms(long ms) {
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

// An empty task: run synchronously on a serial queue, it returns once
// everything queued before it has finished.
static void ca_drain(void *context) { (void)context; }

// The system object's default-device properties, as SpudAudio roles.
static const struct {
	AudioObjectPropertySelector selector;
	SPUDAUDIO_DIRECTION direction;
	SPUDAUDIO_DEFAULT_ROLES role;
} ca_defaults[] = {
    {kAudioHardwarePropertyDefaultOutputDevice, SPUDAUDIO_DIRECTION_OUTPUT, SPUDAUDIO_DEFAULT_ROLE_GENERAL},
    {kAudioHardwarePropertyDefaultSystemOutputDevice, SPUDAUDIO_DIRECTION_OUTPUT, SPUDAUDIO_DEFAULT_ROLE_SYSTEM_SOUNDS},
    {kAudioHardwarePropertyDefaultInputDevice, SPUDAUDIO_DIRECTION_INPUT, SPUDAUDIO_DEFAULT_ROLE_GENERAL},
};

// --------------------------------------------------------------------------
// Device-change notification
// --------------------------------------------------------------------------

static const AudioObjectPropertyAddress ca_hardware_watched[] = {
    {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
};

// Its address is the queue-specific key that marks a watch's queue; the
// value stored under it is the watch. Only the address is used.
static const char ca_watch_key = 0;

// Every endpoint there is now, in a block of the watch's allocator with
// room for `*out_capacity`: the same test and the same identity (UID +
// direction) as spudaudio_enumerate_devices, for both directions.
static SPUDRESULT ca_list_endpoints(
    const SPUD_ALLOCATOR *allocator,
    struct ca_endpoint **out_endpoints,
    uint32_t *out_count,
    uint32_t *out_capacity) {
	static const SPUDAUDIO_DIRECTION directions[] = {SPUDAUDIO_DIRECTION_OUTPUT, SPUDAUDIO_DIRECTION_INPUT};
	*out_endpoints                                = NULL;
	*out_count                                    = 0;
	*out_capacity                                 = 0;

	const struct ca_property p = ca_get_alloc(allocator, kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	const AudioObjectID *ids   = (const AudioObjectID *)p.data;
	uint32_t count             = ids ? p.size / (uint32_t)sizeof(AudioObjectID) : 0;
	if (count == 0) {
		ca_property_free(allocator, &p);
		return SPUD_SUCCESS;
	}
	const uint32_t capacity  = count * 2;
	struct ca_endpoint *list = SPUD_NEW_ARRAY_ZEROED(allocator, struct ca_endpoint, capacity);
	if (!list) {
		ca_property_free(allocator, &p);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	uint32_t written = 0;
	for (uint32_t i = 0; i < count; ++i) {
		for (size_t d = 0; d < sizeof(directions) / sizeof(directions[0]); ++d) {
			if (ca_channel_count(allocator, ids[i], directions[d], NULL) == 0)
				continue; // no streams in this direction
			list[written].direction = directions[d];
			ca_get_string(ids[i], kAudioDevicePropertyDeviceUID, list[written].persistent_id, sizeof(list[written].persistent_id));
			++written;
		}
	}
	ca_property_free(allocator, &p);
	*out_endpoints = list;
	*out_count     = written;
	*out_capacity  = capacity;
	return SPUD_SUCCESS;
}

static bool ca_endpoint_in(const struct ca_endpoint *endpoint, const struct ca_endpoint *list, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i)
		if (list[i].direction == endpoint->direction && strcmp(list[i].persistent_id, endpoint->persistent_id) == 0)
			return true;
	return false;
}

// Runs on the watch's queue: kAudioHardwarePropertyDevices changed. Reports
// what left and then what arrived since the last record, and keeps the new
// one. If the endpoints can't be listed the old record stays, and the next
// change is compared against it.
static void ca_watch_devices_changed(struct ca_watch *w) {
	struct ca_endpoint *now = NULL;
	uint32_t now_count      = 0;
	uint32_t now_capacity   = 0;
	if (SPUDFAIL(ca_list_endpoints(&w->allocator, &now, &now_count, &now_capacity)))
		return;
	struct ca_endpoint *before     = w->endpoints;
	uint32_t before_count          = w->endpoint_count;
	const uint32_t before_capacity = w->endpoint_capacity;
	w->endpoints                   = now;
	w->endpoint_count              = now_count;
	w->endpoint_capacity           = now_capacity;

	for (uint32_t i = 0; i < before_count; ++i)
		if (!ca_endpoint_in(&before[i], now, now_count))
			w->callback(SPUDAUDIO_DEVICE_EVENT_REMOVED, before[i].persistent_id, before[i].direction, SPUDAUDIO_DEFAULT_ROLE_NONE, w->user_data);
	for (uint32_t i = 0; i < now_count; ++i)
		if (!ca_endpoint_in(&now[i], before, before_count))
			w->callback(SPUDAUDIO_DEVICE_EVENT_ADDED, now[i].persistent_id, now[i].direction, SPUDAUDIO_DEFAULT_ROLE_NONE, w->user_data);
	SPUD_DELETE_ARRAY(&w->allocator, struct ca_endpoint, before, before_capacity);
}

// Runs on the watch's queue: one of ca_defaults changed. A role left with
// no device is reported with a NULL id.
static void ca_watch_default_changed(struct ca_watch *w, size_t which) {
	AudioObjectID id = kAudioObjectUnknown;
	char uid[256];
	const char *persistent_id = NULL;
	if (ca_get(kAudioObjectSystemObject, ca_defaults[which].selector, kAudioObjectPropertyScopeGlobal, sizeof(id), &id) == noErr && id != kAudioObjectUnknown) {
		ca_get_string(id, kAudioDevicePropertyDeviceUID, uid, sizeof(uid));
		persistent_id = uid;
	}
	w->callback(SPUDAUDIO_DEVICE_EVENT_DEFAULT_CHANGED, persistent_id, ca_defaults[which].direction, ca_defaults[which].role, w->user_data);
}

// Runs on the watch's queue. The device list is handled first, so an
// endpoint is reported ADDED before it is reported as a default.
static void ca_on_hardware_change(struct ca_watch *w, UInt32 count, const AudioObjectPropertyAddress *addresses) {
	if (!w->have_baseline)
		return;
	for (UInt32 i = 0; i < count; ++i)
		if (addresses[i].mSelector == kAudioHardwarePropertyDevices) {
			ca_watch_devices_changed(w);
			break;
		}
	for (UInt32 i = 0; i < count; ++i)
		for (size_t d = 0; d < sizeof(ca_defaults) / sizeof(ca_defaults[0]); ++d)
			if (addresses[i].mSelector == ca_defaults[d].selector)
				ca_watch_default_changed(w, d);
}

// Runs on the watch's queue, once, after the listeners are registered.
static void ca_watch_take_baseline(void *context) {
	struct ca_watch *w = (struct ca_watch *)context;
	w->baseline        = ca_list_endpoints(&w->allocator, &w->endpoints, &w->endpoint_count, &w->endpoint_capacity);
	w->have_baseline   = SPUD_SUCCESS == w->baseline;
}

// Returns once no listener is running or will run. NULL is a no-op.
static void ca_watch_destroy(struct ca_watch *w) {
	if (!w)
		return;
	if (w->listening) {
		for (size_t i = 0; i < sizeof(ca_hardware_watched) / sizeof(ca_hardware_watched[0]); ++i)
			AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &ca_hardware_watched[i], w->queue, w->listener);
		// Removal doesn't wait for a listener already queued: running an
		// empty task on the same serial queue does.
		dispatch_sync_f(w->queue, NULL, ca_drain);
	}
	if (w->listener)
		Block_release(w->listener);
	if (w->queue)
		dispatch_release(w->queue);
	const SPUD_ALLOCATOR allocator = w->allocator;
	SPUD_DELETE_ARRAY(&allocator, struct ca_endpoint, w->endpoints, w->endpoint_capacity);
	SPUD_DELETE(&allocator, struct ca_watch, w);
}

static SPUDRESULT ca_watch_create(
    const SPUD_ALLOCATOR *allocator,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data,
    struct ca_watch **out_watch) {
	struct ca_watch *w = SPUD_NEW_ZEROED(allocator, struct ca_watch);
	if (!w)
		return SPUDRESULT_OUT_OF_MEMORY;
	w->allocator = *allocator;
	w->callback  = callback;
	w->user_data = user_data;
	w->queue     = dispatch_queue_create("spudaudio.coreaudio.devices", DISPATCH_QUEUE_SERIAL);
	if (w->queue)
		w->listener = Block_copy(^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
			ca_on_hardware_change(w, count, addresses);
		});
	if (!w->queue || !w->listener) {
		ca_watch_destroy(w);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	dispatch_queue_set_specific(w->queue, &ca_watch_key, w, NULL);

	// Listeners first, baseline second, and the baseline on the queue: a
	// change in between is then queued behind the baseline and compared
	// against it, instead of falling in a gap before the listeners existed.
	SPUDRESULT r = SPUD_SUCCESS;
	w->listening = true;
	for (size_t i = 0; i < sizeof(ca_hardware_watched) / sizeof(ca_hardware_watched[0]) && SPUD_SUCCESS == r; ++i)
		r = ca_result(AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &ca_hardware_watched[i], w->queue, w->listener));
	if (SPUD_SUCCESS == r) {
		dispatch_sync_f(w->queue, w, ca_watch_take_baseline);
		r = w->baseline;
	}
	if (SPUDFAIL(r)) {
		ca_watch_destroy(w);
		return r;
	}
	*out_watch = w;
	return SPUD_SUCCESS;
}

// True on the instance's own device-event callback, where waiting for that
// callback to return would never end.
static bool ca_in_device_event_callback(spudaudio_instance instance) {
	return instance->watch && dispatch_get_specific(&ca_watch_key) == instance->watch;
}

// --------------------------------------------------------------------------
// Instance / devices
// --------------------------------------------------------------------------

SPUDRESULT spudaudio_create_instance(
    const SPUDAUDIO_INSTANCE_DESC *desc,
    spudaudio_instance *out_instance) {
	const SPUDRESULT desc_result = spudaudio_validate_instance_desc(desc, out_instance);
	if (desc_result != SPUD_SUCCESS)
		return desc_result;
	if (desc->native_api != SPUDAUDIO_NATIVE_API_COREAUDIO)
		return SPUDRESULT_INVALID_API;
	struct spudaudio_instance_t *inst = SPUD_NEW_ZEROED(desc->allocator, struct spudaudio_instance_t);
	if (!inst)
		return SPUDRESULT_OUT_OF_MEMORY;
	inst->head.allocator = *desc->allocator;
	SPUD_BIRTH(inst);
	*out_instance        = inst;
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_destroy_instance(spudaudio_instance instance) {
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_destroy_instance", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (ca_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;
	ca_watch_destroy(instance->watch);
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	struct spudaudio_device_t *dev = instance->devices;
	while (dev) {
		struct spudaudio_device_t *next = dev->next;
		SPUD_KILL(dev);
		spud_string_free(&allocator, dev->head.debug_name);
		SPUD_DELETE(&allocator, struct spudaudio_device_t, dev);
		dev = next;
	}
	SPUD_DELETE_ARRAY(&allocator, spudaudio_device, instance->enumerated[0], instance->enumerated_capacity[0]);
	SPUD_DELETE_ARRAY(&allocator, spudaudio_device, instance->enumerated[1], instance->enumerated_capacity[1]);
	spud_string_free(&allocator, instance->head.debug_name);
	SPUD_DELETE(&allocator, struct spudaudio_instance_t, instance);
	return SPUD_SUCCESS;
}

SPUDAUDIO_NATIVE_API spudaudio_get_native_api(spudaudio_instance instance) {
	if (!instance)
		return SPUDAUDIO_NATIVE_API_NONE;
	if (SPUD_STALE(instance, "spudaudio_get_native_api", "instance"))
		return SPUDAUDIO_NATIVE_API_NONE;
	return SPUDAUDIO_NATIVE_API_COREAUDIO;
}

// Stable handles across enumerations: keyed by UID + direction.
static struct spudaudio_device_t *ca_find_or_add_device(
    spudaudio_instance instance,
    AudioObjectID id,
    const char *uid,
    SPUDAUDIO_DIRECTION direction) {
	for (struct spudaudio_device_t *d = instance->devices; d; d = d->next)
		if (d->direction == direction && strcmp(d->persistent_id, uid) == 0) {
			d->id = id; // the HAL may renumber a device that came back
			return d;
		}
	struct spudaudio_device_t *d = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudaudio_device_t);
	if (!d)
		return NULL;
	d->head.allocator = instance->head.allocator;
	SPUD_BIRTH(d);
	d->id             = id;
	d->direction      = direction;
	strncpy(d->persistent_id, uid, sizeof(d->persistent_id) - 1);
	d->next           = instance->devices;
	instance->devices = d;
	return d;
}

SPUDRESULT spudaudio_enumerate_devices(
    spudaudio_instance instance,
    SPUDAUDIO_DIRECTION direction,
    spudaudio_device **out_devices,
    uint32_t *out_device_count) {
	if (!out_devices || !out_device_count)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_devices      = NULL;
	*out_device_count = 0;
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_enumerate_devices", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (direction != SPUDAUDIO_DIRECTION_OUTPUT && direction != SPUDAUDIO_DIRECTION_INPUT)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	const SPUD_ALLOCATOR *allocator = &instance->head.allocator;
	const struct ca_property p      = ca_get_alloc(allocator, kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
	const AudioObjectID *ids        = (const AudioObjectID *)p.data;
	uint32_t count                  = ids ? p.size / (uint32_t)sizeof(AudioObjectID) : 0;
	spudaudio_device *list          = count ? SPUD_NEW_ARRAY_ZEROED(allocator, spudaudio_device, count) : NULL;
	if (count && !list) {
		ca_property_free(allocator, &p);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	uint32_t written = 0;
	SPUDRESULT r     = SPUD_SUCCESS;
	for (uint32_t i = 0; i < count; ++i) {
		if (ca_channel_count(allocator, ids[i], direction, NULL) == 0)
			continue; // no streams in this direction
		char uid[256];
		ca_get_string(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof(uid));
		struct spudaudio_device_t *d = ca_find_or_add_device(instance, ids[i], uid, direction);
		if (!d) {
			r = SPUDRESULT_OUT_OF_MEMORY;
			break;
		}
		list[written++] = d;
	}
	ca_property_free(allocator, &p);
	if (SPUDFAIL(r)) {
		SPUD_DELETE_ARRAY(allocator, spudaudio_device, list, count);
		return r;
	}
	SPUD_DELETE_ARRAY(allocator, spudaudio_device, instance->enumerated[direction], instance->enumerated_capacity[direction]);
	instance->enumerated[direction]          = list;
	instance->enumerated_capacity[direction] = count;
	*out_devices                             = list;
	*out_device_count                        = written;
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_get_device_properties(
    spudaudio_device device,
    SPUDAUDIO_DEVICE_PROPERTIES *out_properties) {
	if (!out_properties)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_properties->base, SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	memset(out_properties, 0, sizeof(*out_properties));
	out_properties->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_device_properties", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;

	UInt32 alive = 0;
	if (ca_get(device->id, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, sizeof(alive), &alive) != noErr || !alive)
		return SPUDRESULT_SAUD_DEVICE_LOST;

	ca_get_string(device->id, kAudioObjectPropertyName, out_properties->name, sizeof(out_properties->name));
	memcpy(out_properties->persistent_id, device->persistent_id, sizeof(out_properties->persistent_id));
	out_properties->direction          = device->direction;
	out_properties->supports_exclusive = true; // hog mode
	ca_device_format(&device->head.allocator, device->id, device->direction, &out_properties->native_format);

	for (size_t i = 0; i < sizeof(ca_defaults) / sizeof(ca_defaults[0]); ++i) {
		if (ca_defaults[i].direction != device->direction)
			continue;
		AudioObjectID id = kAudioObjectUnknown;
		if (ca_get(kAudioObjectSystemObject, ca_defaults[i].selector, kAudioObjectPropertyScopeGlobal, sizeof(id), &id) == noErr && id == device->id)
			out_properties->default_roles |= ca_defaults[i].role;
	}
	return SPUD_SUCCESS;
}

/*
 * Device-wide, so explicit. Validated against the device's available rates
 * first; the HAL applies a new nominal rate asynchronously, so this waits
 * (bounded) until the device reports it.
 */
SPUDRESULT spudaudio_set_device_sample_rate(spudaudio_device device, uint32_t sample_rate) {
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_set_device_sample_rate", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (sample_rate == 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;

	const struct ca_property p =
	    ca_get_alloc(&device->head.allocator, device->id, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal);
	const AudioValueRange *ranges = (const AudioValueRange *)p.data;
	bool available                = false;
	for (uint32_t i = 0; ranges && i < p.size / sizeof(AudioValueRange); ++i)
		if (sample_rate >= ranges[i].mMinimum && sample_rate <= ranges[i].mMaximum)
			available = true;
	ca_property_free(&device->head.allocator, &p);
	if (!available)
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	if (ca_nominal_rate(device->id) == sample_rate)
		return SPUD_SUCCESS;

	Float64 rate      = sample_rate;
	OSStatus status   = ca_set(device->id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(rate), &rate);
	if (status != noErr)
		return ca_result(status);
	for (int waited = 0; waited < 2000; waited += 10) {
		if (ca_nominal_rate(device->id) == sample_rate)
			return SPUD_SUCCESS;
		ca_sleep_ms(10);
	}
	return SPUDRESULT_API_SPECIFIC_FAILURE; // the device never reported the new rate
}

// --------------------------------------------------------------------------
// Timing / probe
// --------------------------------------------------------------------------

static bool ca_period_range(AudioObjectID dev, uint32_t *out_min, uint32_t *out_max) {
	AudioValueRange range = {0, 0};
	if (ca_get(dev, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, sizeof(range), &range) != noErr)
		return false;
	*out_min = (uint32_t)ceil(range.mMinimum);
	*out_max = (uint32_t)floor(range.mMaximum);
	return true;
}

// kAudioDevicePropertyUsesVariableBufferFrameSizes: when present, buffers
// vary slightly and its value is the largest the IOProc will be given.
static uint32_t ca_variable_max_frames(AudioObjectID dev) {
	AudioObjectPropertyAddress a = {kAudioDevicePropertyUsesVariableBufferFrameSizes, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	UInt32 max_frames            = 0;
	if (AudioObjectHasProperty(dev, &a))
		ca_get(dev, a.mSelector, a.mScope, sizeof(max_frames), &max_frames);
	return max_frames;
}

SPUDRESULT spudaudio_get_timing_caps(
    spudaudio_device device,
    const SPUDAUDIO_FORMAT *format,
    SPUDAUDIO_STREAM_FLAGS flags,
    SPUDAUDIO_TIMING_CAPS *out_caps) {
	if (!out_caps)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_caps->base, SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	memset(out_caps, 0, sizeof(*out_caps));
	out_caps->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (!format)
		return SPUDRESULT_NULL_DESC;
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_timing_caps", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (!ca_period_range(device->id, &out_caps->min_period_frames, &out_caps->max_period_frames))
		return SPUDRESULT_SAUD_DEVICE_LOST;
	UInt32 current = 0;
	ca_get(device->id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(current), &current);
	out_caps->period_mode               = SPUDAUDIO_PERIOD_MODE_EXACT;
	out_caps->period_granularity_frames = 1;
	out_caps->default_period_frames     = current;
	out_caps->variable_callback_frames  = ca_variable_max_frames(device->id) != 0;
	// min/max_buffer_frames stay 0: no buffer separate from the period.

	// Hog mode doesn't change the HAL's buffer-size range; conversion does
	// change what it means. The range above is in device frames, and a
	// converted stream's period is in its own, handed over whole each time.
	if (flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION) {
		SPUDAUDIO_FORMAT device_format;
		ca_device_format(&device->head.allocator, device->id, device->direction, &device_format);
		if (format->sample_rate != 0 && device_format.sample_rate != 0 && ca_format_needs_conversion(format, &device_format)) {
			out_caps->min_period_frames        = ca_scale_frames(out_caps->min_period_frames, device_format.sample_rate, format->sample_rate, 1);
			out_caps->max_period_frames        = ca_scale_frames(out_caps->max_period_frames, device_format.sample_rate, format->sample_rate, -1);
			out_caps->default_period_frames    = ca_scale_frames(out_caps->default_period_frames, device_format.sample_rate, format->sample_rate, 0);
			out_caps->variable_callback_frames = false;
		}
	}
	return SPUD_SUCCESS;
}

static SPUDRESULT ca_check_thread(const SPUDAUDIO_THREAD_DESC *t) {
	// The HAL's own real-time IO thread runs the IOProc.
	return t->priority == SPUDAUDIO_THREAD_PRIORITY_PLATFORM ? SPUD_SUCCESS : SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED;
}

// What ca_check_desc worked out about the device side of a stream.
struct ca_plan {
	SPUDAUDIO_FORMAT device_format; // the device's own format in the stream's direction
	uint32_t device_period;         // kAudioDevicePropertyBufferFrameSize to set
	uint32_t device_max_frames;     // the most frames one IOProc call brings
	AudioConverterRef converter;    // NULL when the stream's format is the device's
};

/*
 * Format, period and buffer are properties of the device the HAL reports
 * directly, so they're compared exactly here; the suggestion for each is the
 * device's own value. With ALLOW_OS_CONVERSION a format that isn't the
 * device's is put to AudioConverterNew instead, and whether it converts is
 * the converter's answer. Hog mode is checked, not taken — taking it would
 * silence every other app on the device for the probe's duration; create
 * takes it. On failure `plan->converter` may be left set.
 */
static SPUDRESULT ca_plan_stream(const SPUDAUDIO_STREAM_DESC *desc, SPUDAUDIO_STREAM_CONFIG *cfg, struct ca_plan *plan) {
	const AudioObjectID dev = desc->device->id;
	UInt32 alive            = 0;
	if (ca_get(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, sizeof(alive), &alive) != noErr || !alive)
		return SPUDRESULT_SAUD_DEVICE_LOST;

	// --- Format.
	const SPUDAUDIO_FORMAT *device_format = &plan->device_format;
	ca_device_format(&desc->device->head.allocator, dev, desc->device->direction, &plan->device_format);
	cfg->format = desc->format;
	if ((desc->flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION) && ca_format_needs_conversion(&desc->format, device_format)) {
		// A layout must be the one the conversion gives.
		SPUDAUDIO_CHANNEL_POSITION positions[SPUDAUDIO_MAX_CHANNELS];
		ca_converted_positions(device_format, desc->format.channel_count, positions);
		if (spudaudio_format_has_layout(&desc->format) && memcmp(desc->format.positions, positions, desc->format.channel_count) != 0) {
			memcpy(cfg->format.positions, positions, sizeof(cfg->format.positions));
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
		// The device side of the converter is the scratch buffer: the
		// device's rate and channels, Float32, interleaved.
		SPUDAUDIO_FORMAT scratch_format = *device_format;
		scratch_format.interleaved      = true;
		AudioStreamBasicDescription stream_asbd, scratch_asbd;
		const bool out      = desc->device->direction == SPUDAUDIO_DIRECTION_OUTPUT;
		const bool describe = ca_format_to_asbd(&desc->format, &stream_asbd) && ca_format_to_asbd(&scratch_format, &scratch_asbd);
		if (!describe || AudioConverterNew(out ? &stream_asbd : &scratch_asbd, out ? &scratch_asbd : &stream_asbd, &plan->converter) != noErr) {
			// The converter won't do it: suggest what needs no conversion.
			plan->converter           = NULL;
			cfg->format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
			cfg->format.sample_rate   = device_format->sample_rate;
			cfg->format.channel_count = device_format->channel_count;
			memcpy(cfg->format.positions, device_format->positions, sizeof(cfg->format.positions));
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
	} else {
		if (desc->format.sample_format != SPUDAUDIO_SAMPLE_FORMAT_F32) {
			cfg->format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
		if (desc->format.sample_rate != device_format->sample_rate) {
			cfg->format.sample_rate = device_format->sample_rate; // or spudaudio_set_device_sample_rate
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
		if (desc->format.channel_count != device_format->channel_count) {
			cfg->format.channel_count = device_format->channel_count;
			memcpy(cfg->format.positions, device_format->positions, sizeof(cfg->format.positions));
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
		// A layout must be the device's own (its preferred layout is the
		// user's speaker setup in Audio MIDI Setup — not something to change).
		if (spudaudio_format_has_layout(&desc->format) &&
		    memcmp(desc->format.positions, device_format->positions, desc->format.channel_count) != 0) {
			memcpy(cfg->format.positions, device_format->positions, sizeof(cfg->format.positions));
			return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
		}
	}

	// --- Period. The HAL's range is in device frames. A converted stream's
	// period is in its own frames, so the device gets the nearest whole
	// number of its frames that lasts as long.
	uint32_t min_period = 0, max_period = 0;
	if (!ca_period_range(dev, &min_period, &max_period))
		return SPUDRESULT_SAUD_DEVICE_LOST;
	cfg->period_mode = SPUDAUDIO_PERIOD_MODE_EXACT;
	if (plan->converter) {
		const uint32_t rate        = desc->format.sample_rate;
		const uint32_t device_rate = device_format->sample_rate;
		const uint64_t in_device   = ((uint64_t)desc->period_frames * device_rate + rate / 2) / rate;
		if (in_device < min_period || in_device > max_period) {
			cfg->period_frames = in_device < min_period ? ca_scale_frames(min_period, device_rate, rate, 1) : ca_scale_frames(max_period, device_rate, rate, -1);
			return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
		}
		plan->device_period = (uint32_t)in_device;
	} else {
		if (desc->period_frames < min_period || desc->period_frames > max_period) {
			cfg->period_frames = desc->period_frames < min_period ? min_period : max_period;
			return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
		}
		plan->device_period = desc->period_frames;
	}
	cfg->period_frames = desc->period_frames;

	// --- Buffer: none separate from the period.
	if (desc->buffer_frames != 0) {
		cfg->buffer_frames = 0;
		return SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
	}

	// --- Exclusive: hog mode must be free (-1) or already ours.
	if (desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE) {
		pid_t owner = -1;
		if (ca_get(dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(owner), &owner) != noErr)
			return SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;
		if (owner != -1 && owner != getpid())
			return SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;
	}

	uint32_t variable        = ca_variable_max_frames(dev);
	plan->device_max_frames  = variable > plan->device_period ? variable : plan->device_period;
	// A converted stream's callback always gets one whole period.
	cfg->max_callback_frames = plan->converter ? desc->period_frames : plan->device_max_frames;
	return SPUD_SUCCESS;
}

// The one check behind probe and create. On success the caller owns
// `plan->converter`, if there is one.
static SPUDRESULT ca_check_desc(const SPUDAUDIO_STREAM_DESC *desc, SPUDAUDIO_STREAM_CONFIG *cfg, struct ca_plan *plan) {
	memset(plan, 0, sizeof(*plan));
	SPUDRESULT r = ca_plan_stream(desc, cfg, plan);
	if (SPUDFAIL(r) && plan->converter) {
		AudioConverterDispose(plan->converter);
		plan->converter = NULL;
	}
	return r;
}

SPUDRESULT spudaudio_probe_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_config->base, SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	memset(out_config, 0, sizeof(*out_config));
	out_config->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	SPUDRESULT r = spudaudio_validate_stream_desc(desc);
	if (SPUDFAIL(r))
		return r;
	r = ca_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;
	struct ca_plan plan;
	r = ca_check_desc(desc, out_config, &plan);
	if (plan.converter)
		AudioConverterDispose(plan.converter);
	return r;
}

// --------------------------------------------------------------------------
// IOProc
// --------------------------------------------------------------------------

/*
 * A converting stream (ALLOW_OS_CONVERSION). The device works in its own
 * frames and the caller in the stream's, and with a rate change the two
 * don't line up cycle for cycle. So the callback is always given exactly
 * one period, held in `client`, and the AudioConverter sits between that
 * and the device-format scratch buffer:
 *
 * - OUTPUT: the IOProc asks the converter for the device's frames; the
 *   converter pulls stream frames through ca_converter_take_client, which
 *   calls the callback whenever the period it holds is used up.
 * - INPUT: the IOProc hands the converter the device's frames through
 *   ca_converter_take_device and asks it for stream frames until those are
 *   used up, calling the callback each time a period is full.
 *
 * Either way the callback runs zero or more times per IO cycle.
 */

// Returned by ca_converter_take_device when this cycle's frames are used
// up. Any error with no packets tells the converter "none for now" without
// ending its stream; AudioConverterFillComplexBuffer hands it back.
#define CA_CONVERTER_NO_MORE_INPUT ((OSStatus)'spnm')

// The stream's buffer list over `client`, from frame `first` for `frames`.
static void ca_client_buffer_list(struct spudaudio_stream_t *s, AudioBufferList *list, uint32_t first, uint32_t frames) {
	const uint32_t channels = s->config.format.channel_count;
	if (s->config.format.interleaved) {
		const uint32_t frame_bytes        = s->client_sample_bytes * channels;
		list->mNumberBuffers              = 1;
		list->mBuffers[0].mNumberChannels = channels;
		list->mBuffers[0].mDataByteSize   = frames * frame_bytes;
		list->mBuffers[0].mData           = s->client + (size_t)first * frame_bytes;
		return;
	}
	list->mNumberBuffers = channels;
	for (uint32_t ch = 0; ch < channels; ++ch) {
		list->mBuffers[ch].mNumberChannels = 1;
		list->mBuffers[ch].mDataByteSize   = frames * s->client_sample_bytes;
		list->mBuffers[ch].mData           = (uint8_t *)s->client_planes[ch] + (size_t)first * s->client_sample_bytes;
	}
}

// One period to or from the caller. The host time is this IO cycle's, moved
// by where the period's first frame sits in the cycle; the converter's own
// delay isn't in it.
static void ca_call_client(struct spudaudio_stream_t *s) {
	SPUDAUDIO_CALLBACK_INFO info = {0};
	if (s->pending_discontinuity) {
		info.flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
		s->pending_discontinuity = false;
	}
	info.device_position_frames = s->client_position;
	if (s->cycle_host_valid) {
		info.host_time_ns = (uint64_t)((int64_t)s->cycle_host_ns + s->cycle_offset_frames * 1000000000LL / (int64_t)s->config.format.sample_rate);
		info.flags |= SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
	}
	s->callback(s, s->config.format.interleaved ? (void *)s->client : (void *)s->client_planes, s->config.period_frames, &info, s->user_data);
	s->client_position += s->config.period_frames;
	s->cycle_offset_frames += s->config.period_frames;
}

// OUTPUT: the converter wants stream frames. What it was given last time
// has been consumed by the time it asks again, so the period can be refilled.
static OSStatus ca_converter_take_client(
    AudioConverterRef converter,
    UInt32 *io_packets,
    AudioBufferList *io_data,
    AudioStreamPacketDescription **out_descriptions,
    void *client) {
	(void)converter;
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)client;
	if (out_descriptions)
		*out_descriptions = NULL;
	if (s->client_used == s->config.period_frames) {
		ca_call_client(s);
		s->client_used = 0;
	}
	uint32_t frames = s->config.period_frames - s->client_used;
	if (frames > *io_packets)
		frames = *io_packets;
	ca_client_buffer_list(s, io_data, s->client_used, frames);
	s->client_used += frames;
	*io_packets = frames;
	return noErr;
}

// INPUT: the converter wants device frames; it gets what this cycle brought.
static OSStatus ca_converter_take_device(
    AudioConverterRef converter,
    UInt32 *io_packets,
    AudioBufferList *io_data,
    AudioStreamPacketDescription **out_descriptions,
    void *client) {
	(void)converter;
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)client;
	if (out_descriptions)
		*out_descriptions = NULL;
	if (s->device_left == 0) {
		*io_packets = 0;
		return CA_CONVERTER_NO_MORE_INPUT;
	}
	const uint32_t channels = s->device_format.channel_count;
	uint32_t frames         = s->device_left < *io_packets ? s->device_left : *io_packets;
	io_data->mNumberBuffers              = 1;
	io_data->mBuffers[0].mNumberChannels = channels;
	io_data->mBuffers[0].mDataByteSize   = frames * channels * (uint32_t)sizeof(float);
	io_data->mBuffers[0].mData           = s->scratch + (size_t)s->device_given * channels;
	s->device_given += frames;
	s->device_left -= frames;
	*io_packets = frames;
	return noErr;
}

// One IO cycle of a converting stream: `frames` device frames to fill
// (`output`) or to take (`input`).
static void ca_ioproc_converted(
    struct spudaudio_stream_t *s,
    const AudioBufferList *input,
    AudioBufferList *output,
    uint32_t frames,
    const AudioTimeStamp *time) {
	const bool out                 = s->direction == SPUDAUDIO_DIRECTION_OUTPUT;
	const uint32_t device_channels = s->device_format.channel_count;
	const uint32_t period          = s->config.period_frames;

	// A skipped cycle shows on the device clock, as in ca_ioproc; the
	// stream's position moves on by the same time in its own frames.
	if (time && (time->mFlags & kAudioTimeStampSampleTimeValid)) {
		if (s->have_origin && time->mSampleTime > s->expected_sample_time + 0.5) {
			s->pending_discontinuity = true;
			atomic_fetch_add(out ? &s->underflow_count : &s->overflow_count, 1);
			s->client_position += (uint64_t)((time->mSampleTime - s->expected_sample_time) * s->config.format.sample_rate / s->device_format.sample_rate);
		}
		s->have_origin          = true;
		s->expected_sample_time = time->mSampleTime + frames;
	}
	s->cycle_host_valid = time && (time->mFlags & kAudioTimeStampHostTimeValid);
	if (s->cycle_host_valid)
		s->cycle_host_ns = spudaudio_apple_host_time_to_ns(time->mHostTime, &s->timebase);
	// OUTPUT: frames still held from the last callback play first. INPUT:
	// frames already written to the period were captured before this cycle.
	s->cycle_offset_frames = out ? (int64_t)(period - s->client_used) : -(int64_t)s->client_filled;

	if (out) {
		AudioBufferList device_list;
		device_list.mNumberBuffers              = 1;
		device_list.mBuffers[0].mNumberChannels = device_channels;
		device_list.mBuffers[0].mDataByteSize   = frames * device_channels * (uint32_t)sizeof(float);
		device_list.mBuffers[0].mData           = s->scratch;
		UInt32 produced                         = frames;
		if (AudioConverterFillComplexBuffer(s->converter, ca_converter_take_client, s, &produced, &device_list, NULL) != noErr || produced > frames)
			produced = 0;
		// What the converter didn't produce plays as silence.
		if (produced < frames)
			memset(s->scratch + (size_t)produced * device_channels, 0, (size_t)(frames - produced) * device_channels * sizeof(float));
		spudaudio_apple_copy_to_buffer_list(output, frames, device_channels, s->scratch, NULL);
		return;
	}

	spudaudio_apple_copy_from_buffer_list(input, frames, device_channels, s->scratch, NULL);
	s->device_given = 0;
	s->device_left  = frames;
	for (;;) {
		// Room for one buffer per channel of a planar stream.
		struct {
			AudioBufferList list;
			AudioBuffer more[SPUDAUDIO_MAX_CHANNELS - 1];
		} client_list;
		UInt32 produced = period - s->client_filled;
		ca_client_buffer_list(s, &client_list.list, s->client_filled, produced);
		const UInt32 wanted   = produced;
		const OSStatus status = AudioConverterFillComplexBuffer(s->converter, ca_converter_take_device, s, &produced, &client_list.list, NULL);
		if (produced > wanted)
			produced = 0;
		s->client_filled += produced;
		if (s->client_filled == period) {
			ca_call_client(s);
			s->client_filled = 0;
		}
		// CA_CONVERTER_NO_MORE_INPUT: the cycle's frames are used up.
		if (status != noErr || produced == 0)
			break;
	}
}

/*
 * Runs on the HAL's real-time IO thread. No allocation, no locks: the
 * scratch buffer and planes were sized at create for max_callback_frames.
 *
 * Glitches: the HAL keeps the device clock running through an overload and
 * skips the IO cycle, so a skipped cycle shows up as the timestamp's
 * sample time jumping past where the previous buffer ended. That gap is
 * the DISCONTINUITY, and the position jumps by exactly the frames missed.
 */
static OSStatus ca_ioproc(
    AudioObjectID device,
    const AudioTimeStamp *now,
    const AudioBufferList *input,
    const AudioTimeStamp *input_time,
    AudioBufferList *output,
    const AudioTimeStamp *output_time,
    void *client) {
	(void)device, (void)now;
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)client;
	s->callback_thread           = pthread_self();
	atomic_store(&s->in_callback, true);

	const bool out              = s->direction == SPUDAUDIO_DIRECTION_OUTPUT;
	const AudioBufferList *list = out ? output : input;
	const AudioTimeStamp *time  = out ? output_time : input_time;
	const uint32_t channels     = s->config.format.channel_count;
	float *interleaved          = s->config.format.interleaved ? s->scratch : NULL;
	void *frames_ptr            = s->config.format.interleaved ? (void *)s->scratch : (void *)s->planes;

	if (atomic_load(&s->state) != SPUDAUDIO_STREAM_STATE_RUNNING || !list || list->mNumberBuffers == 0 || list->mBuffers[0].mNumberChannels == 0) {
		// Stopping, or ended (device lost / rate changed): output silence.
		if (out && output)
			for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
				if (output->mBuffers[b].mData)
					memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
		atomic_store(&s->in_callback, false);
		return noErr;
	}

	uint32_t frames = list->mBuffers[0].mDataByteSize / (list->mBuffers[0].mNumberChannels * (uint32_t)sizeof(float));
	if (frames > s->device_max_frames)
		frames = s->device_max_frames; // never past the promised bound

	if (s->converter) {
		ca_ioproc_converted(s, input, output, frames, time);
		atomic_store(&s->in_callback, false);
		return noErr;
	}

	SPUDAUDIO_CALLBACK_INFO info = {0};
	if (time && (time->mFlags & kAudioTimeStampSampleTimeValid)) {
		if (!s->have_origin) {
			s->have_origin          = true;
			s->origin_sample_time   = time->mSampleTime;
			s->expected_sample_time = time->mSampleTime;
		}
		if (time->mSampleTime > s->expected_sample_time + 0.5) {
			info.flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
			atomic_fetch_add(out ? &s->underflow_count : &s->overflow_count, 1);
		}
		info.device_position_frames = (uint64_t)(time->mSampleTime - s->origin_sample_time);
		s->expected_sample_time     = time->mSampleTime + frames;
	} else {
		info.device_position_frames = s->position_frames;
	}
	s->position_frames = info.device_position_frames + frames;
	if (time && (time->mFlags & kAudioTimeStampHostTimeValid)) {
		info.host_time_ns = spudaudio_apple_host_time_to_ns(time->mHostTime, &s->timebase);
		info.flags |= SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
	}

	if (out) {
		s->callback(s, frames_ptr, frames, &info, s->user_data);
		spudaudio_apple_copy_to_buffer_list(output, frames, channels, interleaved, s->planes);
	} else {
		spudaudio_apple_copy_from_buffer_list(input, frames, channels, interleaved, s->planes);
		s->callback(s, frames_ptr, frames, &info, s->user_data);
	}

	atomic_store(&s->in_callback, false);
	return noErr;
}

// --------------------------------------------------------------------------
// Streams
// --------------------------------------------------------------------------

static void ca_set_terminal(struct spudaudio_stream_t *s, SPUDRESULT r) {
	atomic_store(&s->error, (int)r);
	atomic_store(&s->state, r == SPUDRESULT_SAUD_DEVICE_LOST ? SPUDAUDIO_STREAM_STATE_DEVICE_LOST : SPUDAUDIO_STREAM_STATE_ERROR);
}

static bool ca_is_terminal(unsigned int state) {
	return state == SPUDAUDIO_STREAM_STATE_DEVICE_LOST || state == SPUDAUDIO_STREAM_STATE_ERROR;
}

// Runs on the stream's listener queue: the device vanished, or its nominal
// rate changed (this process or any other) so the granted format - or the
// converter built for the old rate - is stale.
// Either ends the stream; the device is stopped so the IOProc stops too.
static void ca_on_device_change(struct spudaudio_stream_t *s, UInt32 count, const AudioObjectPropertyAddress *addresses) {
	for (UInt32 i = 0; i < count && !ca_is_terminal(atomic_load(&s->state)); ++i) {
		if (addresses[i].mSelector == kAudioDevicePropertyDeviceIsAlive) {
			UInt32 alive = 0;
			if (ca_get(s->device, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, sizeof(alive), &alive) != noErr || !alive)
				ca_set_terminal(s, SPUDRESULT_SAUD_DEVICE_LOST);
		} else if (addresses[i].mSelector == kAudioDevicePropertyNominalSampleRate) {
			if (ca_nominal_rate(s->device) != s->device_format.sample_rate)
				ca_set_terminal(s, SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED);
		}
	}
	if (ca_is_terminal(atomic_load(&s->state)))
		AudioDeviceStop(s->device, s->proc);
}

static const AudioObjectPropertyAddress ca_watched[] = {
    {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
};

// Hog mode toggles: setting it takes ownership if free, releases it if
// ours (the value written is ignored by the HAL).
static void ca_release_hog(struct spudaudio_stream_t *s) {
	pid_t owner = -1;
	if (s->hogged && ca_get(s->device, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(owner), &owner) == noErr && owner == getpid()) {
		pid_t value = -1;
		ca_set(s->device, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(value), &value);
	}
	s->hogged = false;
}

static void ca_free_stream(struct spudaudio_stream_t *s) {
	if (s->listening) {
		for (size_t i = 0; i < sizeof(ca_watched) / sizeof(ca_watched[0]); ++i)
			AudioObjectRemovePropertyListenerBlock(s->device, &ca_watched[i], s->listener_queue, s->listener);
		// Removal doesn't wait for a listener already queued: running an
		// empty task on the same serial queue does.
		dispatch_sync_f(s->listener_queue, NULL, ca_drain);
	}
	if (s->proc)
		AudioDeviceDestroyIOProcID(s->device, s->proc);
	if (s->converter)
		AudioConverterDispose(s->converter);
	ca_release_hog(s);
	if (s->listener)
		Block_release(s->listener);
	if (s->listener_queue)
		dispatch_release(s->listener_queue);
	const SPUD_ALLOCATOR allocator = s->head.allocator;
	SPUD_DELETE_ARRAY(&allocator, void *, s->client_planes, s->client_planes_count);
	spud_free_array(&allocator, s->client, s->client_samples, s->client_sample_bytes, SPUD_MAX_ALIGN);
	SPUD_DELETE_ARRAY(&allocator, float *, s->planes, s->planes_count);
	SPUD_DELETE_ARRAY(&allocator, float, s->scratch, s->scratch_count);
	spud_string_free(&allocator, s->head.debug_name);
	SPUD_DELETE(&allocator, struct spudaudio_stream_t, s);
}

SPUDRESULT spudaudio_create_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    spudaudio_stream *out_stream) {
	if (!out_stream)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_stream  = NULL;
	SPUDRESULT r = spudaudio_validate_stream_desc(desc);
	if (SPUDFAIL(r))
		return r;
	if (!desc->callback)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	r = ca_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;

	struct spudaudio_stream_t *s = SPUD_NEW_ZEROED(&desc->device->head.allocator, struct spudaudio_stream_t);
	if (!s)
		return SPUDRESULT_OUT_OF_MEMORY;
	s->head.allocator = desc->device->head.allocator;
	SPUD_BIRTH(s);
	s->device         = desc->device->id;
	s->direction      = desc->device->direction;
	s->callback       = desc->callback;
	s->user_data      = desc->user_data;
	atomic_init(&s->state, SPUDAUDIO_STREAM_STATE_STOPPED);
	atomic_init(&s->error, SPUD_SUCCESS);
	atomic_init(&s->underflow_count, 0);
	atomic_init(&s->overflow_count, 0);
	atomic_init(&s->in_callback, false);
	mach_timebase_info(&s->timebase);

	struct ca_plan plan;
	r                    = ca_check_desc(desc, &s->config, &plan);
	s->converter         = plan.converter;
	s->device_format     = plan.device_format;
	s->device_max_frames = plan.device_max_frames;

	// Period: set this process's IO buffer size for the device and read
	// it back — the HAL can still refuse or adjust a value in range.
	if (SPUD_SUCCESS == r) {
		UInt32 frames   = plan.device_period;
		OSStatus status = ca_set(s->device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(frames), &frames);
		UInt32 granted  = 0;
		if (status == noErr)
			status = ca_get(s->device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(granted), &granted);
		if (status != noErr)
			r = ca_result(status);
		else if (granted != plan.device_period)
			r = SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}

	// Exclusive: take hog mode and confirm it's ours.
	if (SPUD_SUCCESS == r && (desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE)) {
		pid_t owner = -1;
		ca_get(s->device, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(owner), &owner);
		if (owner != getpid()) {
			pid_t value = getpid();
			ca_set(s->device, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(value), &value);
			ca_get(s->device, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(owner), &owner);
		}
		s->hogged = owner == getpid();
		if (!s->hogged)
			r = SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;
	}

	if (SPUD_SUCCESS == r && s->converter) {
		// The scratch buffer is the device side of the converter, and
		// `client` holds the one period the callback is given.
		// Every block here and below is made now, so the IOProc never
		// allocates. Each count is kept: it is what the block is freed with.
		const SPUD_ALLOCATOR *allocator = &s->head.allocator;
		const uint32_t channels         = s->config.format.channel_count;
		const uint32_t frames           = s->config.period_frames;
		s->client_sample_bytes          = spudaudio_sample_format_byte_size(s->config.format.sample_format);
		s->scratch_count                = (size_t)s->device_max_frames * s->device_format.channel_count;
		s->client_samples               = (size_t)frames * channels;
		s->client_planes_count          = channels;
		s->scratch                      = SPUD_NEW_ARRAY_ZEROED(allocator, float, s->scratch_count);
		// Bytes read as the stream's samples (int16_t, int32_t, float).
		s->client        = (uint8_t *)spud_allocate_array(allocator, s->client_samples, s->client_sample_bytes, SPUD_MAX_ALIGN, true);
		s->client_planes = SPUD_NEW_ARRAY_ZEROED(allocator, void *, channels);
		if (!s->scratch || !s->client || !s->client_planes)
			r = SPUDRESULT_OUT_OF_MEMORY;
		else
			for (uint32_t ch = 0; ch < channels; ++ch)
				s->client_planes[ch] = s->client + (size_t)ch * frames * s->client_sample_bytes;
	} else if (SPUD_SUCCESS == r) {
		const SPUD_ALLOCATOR *allocator = &s->head.allocator;
		const uint32_t channels         = s->config.format.channel_count;
		const uint32_t frames           = s->config.max_callback_frames;
		s->scratch_count                = (size_t)frames * channels;
		s->planes_count                 = channels;
		s->scratch                      = SPUD_NEW_ARRAY_ZEROED(allocator, float, s->scratch_count);
		s->planes                       = SPUD_NEW_ARRAY_ZEROED(allocator, float *, channels);
		if (!s->scratch || !s->planes)
			r = SPUDRESULT_OUT_OF_MEMORY;
		else
			for (uint32_t ch = 0; ch < channels; ++ch)
				s->planes[ch] = s->scratch + (size_t)ch * frames;
	}

	if (SPUD_SUCCESS == r)
		r = ca_result(AudioDeviceCreateIOProcID(s->device, ca_ioproc, s, &s->proc));

	if (SPUD_SUCCESS == r) {
		s->listener_queue = dispatch_queue_create("spudaudio.coreaudio.stream", DISPATCH_QUEUE_SERIAL);
		s->listener       = Block_copy(^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
            ca_on_device_change(s, count, addresses);
        });
		s->listening      = true;
		for (size_t i = 0; i < sizeof(ca_watched) / sizeof(ca_watched[0]) && SPUD_SUCCESS == r; ++i)
			r = ca_result(AudioObjectAddPropertyListenerBlock(s->device, &ca_watched[i], s->listener_queue, s->listener));
	}

	if (SPUDFAIL(r)) {
		ca_free_stream(s);
		return r;
	}
	*out_stream = s;
	return SPUD_SUCCESS;
}

static bool ca_on_callback_thread(struct spudaudio_stream_t *s) {
	return atomic_load(&s->in_callback) && pthread_equal(pthread_self(), s->callback_thread);
}

SPUDRESULT spudaudio_stream_start(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_start", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (ca_on_callback_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE;
	unsigned int state = atomic_load(&stream->state);
	if (state == SPUDAUDIO_STREAM_STATE_RUNNING)
		return SPUD_SUCCESS;
	if (ca_is_terminal(state))
		return (SPUDRESULT)atomic_load(&stream->error);

	stream->have_origin     = false;
	stream->position_frames = 0;
	if (stream->converter) {
		// Nothing carried over from the last run: not the converter's
		// history, not a part-used period.
		AudioConverterReset(stream->converter);
		stream->client_used           = stream->config.period_frames;
		stream->client_filled         = 0;
		stream->client_position       = 0;
		stream->pending_discontinuity = false;
	}
	// RUNNING before the device starts: the IOProc outputs silence
	// unless the stream is RUNNING.
	atomic_store(&stream->state, SPUDAUDIO_STREAM_STATE_RUNNING);
	OSStatus status = AudioDeviceStart(stream->device, stream->proc);
	if (status != noErr) {
		SPUDRESULT r = ca_result(status);
		if (r == SPUDRESULT_SAUD_DEVICE_LOST)
			ca_set_terminal(stream, r);
		else
			atomic_store(&stream->state, SPUDAUDIO_STREAM_STATE_STOPPED);
		return r;
	}
	return SPUD_SUCCESS;
}

// AudioDeviceStop is synchronous when called off the IO thread: once it
// returns, the IOProc isn't running and won't be called again.
SPUDRESULT spudaudio_stream_stop(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_stop", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (ca_on_callback_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE;
	AudioDeviceStop(stream->device, stream->proc);
	unsigned int running = SPUDAUDIO_STREAM_STATE_RUNNING;
	atomic_compare_exchange_strong(&stream->state, &running, SPUDAUDIO_STREAM_STATE_STOPPED);
	return SPUD_SUCCESS;
}

void spudaudio_destroy_stream(spudaudio_stream stream) {
	if (!stream)
		return;
	if (SPUD_STALE(stream, "spudaudio_destroy_stream", "stream"))
		return;
	spudaudio_stream_stop(stream);
	SPUD_KILL(stream);
	ca_free_stream(stream);
}

SPUDRESULT spudaudio_stream_get_config(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_config->base, SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_config", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	*out_config = stream->config;
	out_config->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_stream_get_status(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_STATUS *out_status) {
	if (!out_status)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_status", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	out_status->state           = (SPUDAUDIO_STREAM_STATE)atomic_load(&stream->state);
	out_status->error           = (SPUDRESULT)atomic_load(&stream->error);
	out_status->underflow_count = atomic_load(&stream->underflow_count);
	out_status->overflow_count  = atomic_load(&stream->overflow_count);
	return SPUD_SUCCESS;
}

// The HAL's own breakdown: device latency + safety offset + the IO buffer
// + the first stream's latency, all in frames at the nominal rate. For a
// converting stream that total is restated in the stream's frames; the
// converter's own delay is not in it.
SPUDRESULT spudaudio_stream_get_latency_frames(
    spudaudio_stream stream,
    uint32_t *out_latency_frames) {
	if (!out_latency_frames)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_latency_frames = 0;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_latency_frames", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	const AudioObjectPropertyScope scope = ca_scope(stream->direction);
	UInt32 device_latency = 0, safety = 0, buffer = 0, stream_latency = 0;
	OSStatus status = ca_get(stream->device, kAudioDevicePropertyLatency, scope, sizeof(device_latency), &device_latency);
	if (status == noErr)
		status = ca_get(stream->device, kAudioDevicePropertySafetyOffset, scope, sizeof(safety), &safety);
	if (status == noErr)
		status = ca_get(stream->device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(buffer), &buffer);
	if (status != noErr)
		return ca_result(status);
	const struct ca_property p   = ca_get_alloc(&stream->head.allocator, stream->device, kAudioDevicePropertyStreams, scope);
	const AudioStreamID *streams = (const AudioStreamID *)p.data;
	if (streams && p.size >= sizeof(AudioStreamID))
		ca_get(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, sizeof(stream_latency), &stream_latency);
	ca_property_free(&stream->head.allocator, &p);
	*out_latency_frames = device_latency + safety + buffer + stream_latency;
	if (stream->converter)
		*out_latency_frames = ca_scale_frames(*out_latency_frames, stream->device_format.sample_rate, stream->config.format.sample_rate, 0);
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_set_device_event_callback(
    spudaudio_instance instance,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data) {
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_set_device_event_callback", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (ca_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;

	// The new watch is built before the old one goes, so a failure leaves
	// the callback that was set still set.
	struct ca_watch *watch = NULL;
	if (callback) {
		SPUDRESULT r = ca_watch_create(&instance->head.allocator, callback, user_data, &watch);
		if (SPUDFAIL(r))
			return r;
	}
	ca_watch_destroy(instance->watch);
	instance->watch = watch;
	return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#else
// Not built without SPUDAUDIO_COMPILE_COREAUDIO (CMake leaves this file
// out); this keeps a stray build of it a valid, empty translation unit.
typedef int spudaudio_coreaudio_not_compiled;
#endif // SPUDAUDIO_COMPILE_COREAUDIO

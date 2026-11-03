#if SPUDAUDIO_COMPILE_REMOTEIO

/*
 * RemoteIO backend for iOS and iPadOS: the RemoteIO audio unit
 * (AudioToolbox's AudioComponent / AudioUnit C calls) on the process's
 * AVAudioSession, which the caller owns. One backend for both: the iPhone
 * and the iPad are one SDK, and nothing here tells them apart. This whole
 * file is SPUDAUDIO_COMPILE_REMOTEIO, which spudaudio.h only allows with
 * SPUDAUDIO_PLATFORM_IOS.
 *
 * Model:
 * - The session belongs to the caller. Nothing here sets its category,
 *   mode or options, activates it or asks for record permission; the
 *   session half (spudaudiosession.m) reads it as it is. The only things
 *   written are the two preferences spudaudio.h names: the sample rate
 *   (spudaudio_set_device_sample_rate) and the IO buffer duration (a
 *   stream's period).
 * - A SpudAudio device is a port of the session's current route in one
 *   direction, identified by its UID. iOS has no other list of devices;
 *   the route is chosen by the system and the person holding the device.
 * - A stream is a RemoteIO unit of its own: bus 0 is the output to the
 *   route, bus 1 the input from it. Its render callback runs on the unit's
 *   real-time thread, so the thread priority must be
 *   SPUDAUDIO_THREAD_PRIORITY_PLATFORM. The callback makes no allocation,
 *   takes no lock and sends no Objective-C message.
 * - The route's format is Float32 at the session's rate with the port's
 *   channels. With ALLOW_OS_CONVERSION another sample format or rate is
 *   the unit's own converter's, between the client format set on the unit
 *   and the route's; the channel count must still be the port's.
 * - The period is the session's IO buffer duration, a request the system
 *   may grant differently, which is then refused with what it granted as
 *   the suggestion.
 * - No exclusive mode.
 * - Device-change notification is the session's route change and media
 *   services reset, heard by the session half and compared here against
 *   the ports last seen. A stream hears the same notifications, and
 *   interruptions, on a queue of its own and ends itself; nothing is ever
 *   restarted behind the caller.
 */

#include "spudaudiosession.h"

#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "../../../spudallocator.h"
#include "../../../spudstructbase.h"

#if __cplusplus
extern "C" {
#endif

/* The two buses of a RemoteIO unit. */
#define RIO_OUTPUT_BUS 0
#define RIO_INPUT_BUS 1

/* How long spudaudio_set_device_sample_rate() waits for the session to
 * report a rate it was asked for, in milliseconds, and how often it looks:
 * the same bound CoreAudio's backend keeps for the HAL. */
#define RIO_RATE_WAIT_MS 2000
#define RIO_RATE_POLL_MS 10

struct spudaudio_device_t {
	spud_object_head head; // allocator: a copy of its instance's
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256]; // the port's UID
	struct spudaudio_device_t *next; // instance-owned list
};

/*
 * Device-change notification for one instance: exists while a callback is
 * set. The session only says that the route changed, so the watch keeps
 * the ports it last saw, per direction, and reports the difference. That
 * record is the watch's own and is only touched on its queue.
 */
struct rio_watch {
	// The instance's: the watch and its port lists are its memory.
	SPUD_ALLOCATOR allocator;
	SPUDAUDIO_DEVICE_EVENT_CALLBACK callback;
	void *user_data;
	spudaudio_session_observer *observer;
	// Queue-only state.
	bool have_baseline; // changes before the baseline is taken are not events
	SPUDRESULT baseline;
	spudaudio_session_port *ports[2];
	uint32_t port_count[2];
	uint32_t port_capacity[2]; // what ports[d] was allocated, and is freed, with
};

struct spudaudio_instance_t {
	spud_object_head head;
	struct spudaudio_device_t *devices; // every device ever enumerated
	spudaudio_device *enumerated[2];    // latest array per direction
	uint32_t enumerated_capacity[2];    // what each was allocated, and is freed, with
	struct rio_watch *watch;            // NULL while no device-event callback is set
};

struct spudaudio_stream_t {
	spud_object_head head; // allocator: a copy of its device's
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256]; // the port the stream is on
	SPUDAUDIO_STREAM_CONFIG config;
	SPUDAUDIO_STREAM_CALLBACK callback;
	void *user_data;
	AudioComponentInstance unit;
	mach_timebase_info_data_t timebase;
	uint32_t device_rate;     // the session's rate when the stream was created
	uint32_t device_channels; // the port's channels then

	// INPUT: the buffers AudioUnitRender writes into, made at creation.
	AudioBufferList *input_list; // one buffer, or one per channel when planar
	size_t input_list_bytes;     // what input_list was allocated, and is freed, with
	float *input_samples;        // max_callback_frames x channels
	size_t input_sample_count;   // floats in input_samples, which it is freed with
	// Planar: one pointer per channel, the callback's `frames`.
	void **planes;
	uint32_t planes_count;

	spudaudio_session_observer *observer; // route changes, interruptions, media services

	atomic_uint state; // SPUDAUDIO_STREAM_STATE
	atomic_int error;  // SPUDRESULT
	atomic_uint_least64_t underflow_count;
	atomic_uint_least64_t overflow_count;

	// Which thread is inside the callback right now, for the start/stop
	// re-entrancy guard. The unit's render thread isn't known in advance.
	atomic_bool in_callback;
	pthread_t callback_thread;

	// Render-only state.
	bool have_origin;
	double origin_sample_time;   // sample time of the first frame since start
	double expected_sample_time; // where the next buffer should start
	uint64_t position_frames;    // fallback when a timestamp has no sample time
	bool pending_discontinuity;  // a render was skipped; flag the next callback
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static SPUDRESULT rio_result(OSStatus status) {
	switch (status) {
	case noErr:
		return SPUD_SUCCESS;
	case AVAudioSessionErrorCodeCannotInterruptOthers:
	case AVAudioSessionErrorCodeCannotStartPlaying:
	case AVAudioSessionErrorCodeCannotStartRecording:
	case AVAudioSessionErrorCodeInsufficientPriority:
	case AVAudioSessionErrorCodeSiriIsRecording:
		return SPUDRESULT_SAUD_INTERRUPTED;
	case AVAudioSessionErrorCodeSessionNotActive:
		return SPUDRESULT_SAUD_SESSION_INACTIVE;
	case AVAudioSessionErrorCodeMediaServicesFailed:
		return SPUDRESULT_SAUD_DEVICE_LOST;
	case kAudioUnitErr_FormatNotSupported:
	case kAudioConverterErr_FormatNotSupported:
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	case kAudio_MemFullError:
		return SPUDRESULT_OUT_OF_MEMORY;
	default:
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

static uint32_t rio_frames(double seconds, uint32_t rate) {
	return (uint32_t)lround(seconds * (double)rate);
}

// frames at rate `from` as frames at rate `to`. round: -1 down, 0 nearest,
// 1 up.
static uint32_t rio_scale_frames(uint32_t frames, uint32_t from, uint32_t to, int round) {
	const uint64_t scaled = (uint64_t)frames * to;
	if (round > 0)
		return (uint32_t)((scaled + from - 1) / from);
	if (round == 0)
		return (uint32_t)((scaled + from / 2) / from);
	return (uint32_t)(scaled / from);
}

static void rio_sleep_ms(long ms) {
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

// The route's own format for a port: Float32 at the session's rate.
static void rio_device_format(const spudaudio_session_port *port, uint32_t rate, SPUDAUDIO_FORMAT *out) {
	memset(out, 0, sizeof(*out));
	out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
	out->sample_rate   = rate;
	out->channel_count = port->channel_count;
	out->interleaved   = true;
	memcpy(out->positions, port->positions, sizeof(out->positions));
}

// Whether a stream of `format` on a port of `device_format` goes through
// the unit's converter. Interleaving is never a reason: RemoteIO takes
// either as its client format without converting a sample.
static bool rio_format_needs_conversion(const SPUDAUDIO_FORMAT *format, const SPUDAUDIO_FORMAT *device_format) {
	return format->sample_format != SPUDAUDIO_SAMPLE_FORMAT_F32 || format->sample_rate != device_format->sample_rate;
}

// A SpudAudio format as linear PCM in native byte order. False for a sample
// format with no PCM description. As CoreAudio's backend describes them.
static bool rio_format_to_asbd(const SPUDAUDIO_FORMAT *format, AudioStreamBasicDescription *out) {
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

static SPUDRESULT rio_new_unit(AudioComponentInstance *out_unit) {
	*out_unit                       = NULL;
	AudioComponentDescription which = {0};
	which.componentType             = kAudioUnitType_Output;
	which.componentSubType          = kAudioUnitSubType_RemoteIO;
	which.componentManufacturer     = kAudioUnitManufacturer_Apple;
	AudioComponent component        = AudioComponentFindNext(NULL, &which);
	if (!component)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	return rio_result(AudioComponentInstanceNew(component, out_unit));
}

// The client side of the unit for a stream in `direction`: what the
// render callback hands over (OUTPUT, bus 0's input scope) or what the
// input callback renders (INPUT, bus 1's output scope).
static OSStatus rio_set_client_format(AudioComponentInstance unit, SPUDAUDIO_DIRECTION direction, const AudioStreamBasicDescription *asbd) {
	const bool out = direction == SPUDAUDIO_DIRECTION_OUTPUT;
	return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, out ? kAudioUnitScope_Input : kAudioUnitScope_Output,
	    out ? RIO_OUTPUT_BUS : RIO_INPUT_BUS, asbd, sizeof(*asbd));
}

// --------------------------------------------------------------------------
// Device-change notification
// --------------------------------------------------------------------------

static bool rio_port_in(const spudaudio_session_port *port, const spudaudio_session_port *list, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i)
		if (strcmp(list[i].persistent_id, port->persistent_id) == 0)
			return true;
	return false;
}

// The port holding GENERAL in a list: the only one, else none.
static const char *rio_general(const spudaudio_session_port *list, uint32_t count) {
	return count == 1 ? list[0].persistent_id : NULL;
}

static bool rio_same_id(const char *a, const char *b) {
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

// Runs on the watch's queue: the route changed, or the media services were
// reset. Reports, per direction, what left and then what arrived since the
// last record, then a GENERAL that moved; and keeps the new record. If the
// ports can't be read the old record stays, and the next change is
// compared against it.
static void rio_watch_route_changed(struct rio_watch *w) {
	static const SPUDAUDIO_DIRECTION directions[] = {SPUDAUDIO_DIRECTION_OUTPUT, SPUDAUDIO_DIRECTION_INPUT};
	for (size_t d = 0; d < sizeof(directions) / sizeof(directions[0]); ++d) {
		const SPUDAUDIO_DIRECTION dir = directions[d];
		spudaudio_session_port *now   = NULL;
		uint32_t now_count = 0, now_capacity = 0;
		if (SPUDFAIL(spudaudio_session_route_ports(&w->allocator, dir, &now, &now_count, &now_capacity)))
			continue;
		spudaudio_session_port *before = w->ports[dir];
		const uint32_t before_count    = w->port_count[dir];
		const uint32_t before_capacity = w->port_capacity[dir];
		w->ports[dir]                  = now;
		w->port_count[dir]             = now_count;
		w->port_capacity[dir]          = now_capacity;

		for (uint32_t i = 0; i < before_count; ++i)
			if (!rio_port_in(&before[i], now, now_count))
				w->callback(SPUDAUDIO_DEVICE_EVENT_REMOVED, before[i].persistent_id, dir, SPUDAUDIO_DEFAULT_ROLE_NONE, w->user_data);
		for (uint32_t i = 0; i < now_count; ++i)
			if (!rio_port_in(&now[i], before, before_count))
				w->callback(SPUDAUDIO_DEVICE_EVENT_ADDED, now[i].persistent_id, dir, SPUDAUDIO_DEFAULT_ROLE_NONE, w->user_data);
		const char *general = rio_general(now, now_count);
		if (!rio_same_id(rio_general(before, before_count), general))
			w->callback(SPUDAUDIO_DEVICE_EVENT_DEFAULT_CHANGED, general, dir, SPUDAUDIO_DEFAULT_ROLE_GENERAL, w->user_data);
		SPUD_DELETE_ARRAY(&w->allocator, spudaudio_session_port, before, before_capacity);
	}
}

// Runs on the watch's queue.
static void rio_watch_event(uint32_t event, void *context) {
	struct rio_watch *w = (struct rio_watch *)context;
	if (!w->have_baseline)
		return;
	if (event == SPUDAUDIO_SESSION_EVENT_ROUTE_CHANGE || event == SPUDAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET)
		rio_watch_route_changed(w);
}

// Runs on the watch's queue, once, after the observer is registered.
static void rio_watch_take_baseline(void *context) {
	struct rio_watch *w = (struct rio_watch *)context;
	w->baseline         = spudaudio_session_route_ports(&w->allocator, SPUDAUDIO_DIRECTION_OUTPUT, &w->ports[SPUDAUDIO_DIRECTION_OUTPUT],
	            &w->port_count[SPUDAUDIO_DIRECTION_OUTPUT], &w->port_capacity[SPUDAUDIO_DIRECTION_OUTPUT]);
	if (SPUD_SUCCESS == w->baseline)
		w->baseline = spudaudio_session_route_ports(&w->allocator, SPUDAUDIO_DIRECTION_INPUT, &w->ports[SPUDAUDIO_DIRECTION_INPUT],
		    &w->port_count[SPUDAUDIO_DIRECTION_INPUT], &w->port_capacity[SPUDAUDIO_DIRECTION_INPUT]);
	w->have_baseline = SPUD_SUCCESS == w->baseline;
}

// Returns once the callback is not running and will not run. NULL is a
// no-op.
static void rio_watch_destroy(struct rio_watch *w) {
	if (!w)
		return;
	spudaudio_session_unobserve(w->observer);
	const SPUD_ALLOCATOR allocator = w->allocator;
	for (int d = 0; d < 2; ++d)
		SPUD_DELETE_ARRAY(&allocator, spudaudio_session_port, w->ports[d], w->port_capacity[d]);
	SPUD_DELETE(&allocator, struct rio_watch, w);
}

static SPUDRESULT rio_watch_create(
    const SPUD_ALLOCATOR *allocator,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data,
    struct rio_watch **out_watch) {
	struct rio_watch *w = SPUD_NEW_ZEROED(allocator, struct rio_watch);
	if (!w)
		return SPUDRESULT_OUT_OF_MEMORY;
	w->allocator = *allocator;
	w->callback  = callback;
	w->user_data = user_data;
	// Observer first, baseline second, and the baseline on its queue: a
	// change in between is then queued behind the baseline and compared
	// against it, instead of falling in a gap before the observer existed.
	SPUDRESULT r = spudaudio_session_observe(allocator, "spudaudio.remoteio.devices", rio_watch_event, w, &w->observer);
	if (SPUD_SUCCESS == r) {
		spudaudio_session_run_on_queue(w->observer, rio_watch_take_baseline, w);
		r = w->baseline;
	}
	if (SPUDFAIL(r)) {
		rio_watch_destroy(w);
		return r;
	}
	*out_watch = w;
	return SPUD_SUCCESS;
}

// True on the instance's own device-event callback, where waiting for that
// callback to return would never end.
static bool rio_in_device_event_callback(spudaudio_instance instance) {
	return instance->watch && spudaudio_session_on_queue(instance->watch->observer);
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
	if (desc->native_api != SPUDAUDIO_NATIVE_API_REMOTEIO)
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
	if (rio_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;
	rio_watch_destroy(instance->watch);
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
	return SPUDAUDIO_NATIVE_API_REMOTEIO;
}

// Stable handles across enumerations: keyed by UID + direction.
static struct spudaudio_device_t *rio_find_or_add_device(
    spudaudio_instance instance,
    const char *uid,
    SPUDAUDIO_DIRECTION direction) {
	for (struct spudaudio_device_t *d = instance->devices; d; d = d->next)
		if (d->direction == direction && strcmp(d->persistent_id, uid) == 0)
			return d;
	struct spudaudio_device_t *d = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudaudio_device_t);
	if (!d)
		return NULL;
	d->head.allocator = instance->head.allocator;
	SPUD_BIRTH(d);
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
	spudaudio_session_port *ports   = NULL;
	uint32_t count = 0, capacity = 0;
	SPUDRESULT r = spudaudio_session_route_ports(allocator, direction, &ports, &count, &capacity);
	if (SPUDFAIL(r))
		return r;
	spudaudio_device *list = count ? SPUD_NEW_ARRAY_ZEROED(allocator, spudaudio_device, count) : NULL;
	if (count && !list) {
		SPUD_DELETE_ARRAY(allocator, spudaudio_session_port, ports, capacity);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	for (uint32_t i = 0; i < count; ++i) {
		struct spudaudio_device_t *d = rio_find_or_add_device(instance, ports[i].persistent_id, direction);
		if (!d) {
			r = SPUDRESULT_OUT_OF_MEMORY;
			break;
		}
		list[i] = d;
	}
	SPUD_DELETE_ARRAY(allocator, spudaudio_session_port, ports, capacity);
	if (SPUDFAIL(r)) {
		SPUD_DELETE_ARRAY(allocator, spudaudio_device, list, count);
		return r;
	}
	SPUD_DELETE_ARRAY(allocator, spudaudio_device, instance->enumerated[direction], instance->enumerated_capacity[direction]);
	instance->enumerated[direction]          = list;
	instance->enumerated_capacity[direction] = count;
	*out_devices                             = list;
	*out_device_count                        = count;
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

	spudaudio_session_port port;
	uint32_t route_ports = 0;
	if (!spudaudio_session_find_port(&device->head.allocator, device->direction, device->persistent_id, &port, &route_ports))
		return SPUDRESULT_SAUD_DEVICE_LOST;
	spudaudio_session_state session;
	spudaudio_session_read(&session);

	memcpy(out_properties->name, port.name, sizeof(out_properties->name));
	memcpy(out_properties->persistent_id, device->persistent_id, sizeof(out_properties->persistent_id));
	out_properties->direction          = device->direction;
	out_properties->default_roles      = route_ports == 1 ? SPUDAUDIO_DEFAULT_ROLE_GENERAL : SPUDAUDIO_DEFAULT_ROLE_NONE;
	out_properties->supports_exclusive = false;
	rio_device_format(&port, (uint32_t)lround(session.sample_rate), &out_properties->native_format);
	return SPUD_SUCCESS;
}

/*
 * Session-wide, so explicit. The session takes a preferred rate as a
 * request; this waits (bounded) for it to report the rate, and puts the
 * preference back where it doesn't.
 */
SPUDRESULT spudaudio_set_device_sample_rate(spudaudio_device device, uint32_t sample_rate) {
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_set_device_sample_rate", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (sample_rate == 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!spudaudio_session_find_port(&device->head.allocator, device->direction, device->persistent_id, NULL, NULL))
		return SPUDRESULT_SAUD_DEVICE_LOST;

	spudaudio_session_state session;
	spudaudio_session_read(&session);
	if (lround(session.sample_rate) == (long)sample_rate)
		return SPUD_SUCCESS;
	SPUDRESULT r = spudaudio_session_set_preferred_sample_rate(sample_rate);
	if (SPUDFAIL(r))
		return r;
	for (int waited = 0; waited < RIO_RATE_WAIT_MS; waited += RIO_RATE_POLL_MS) {
		spudaudio_session_state now;
		spudaudio_session_read(&now);
		if (lround(now.sample_rate) == (long)sample_rate)
			return SPUD_SUCCESS;
		rio_sleep_ms(RIO_RATE_POLL_MS);
	}
	// Granted otherwise: a failed call changes nothing.
	spudaudio_session_set_preferred_sample_rate(session.preferred_sample_rate);
	return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
}

// --------------------------------------------------------------------------
// Timing / probe
// --------------------------------------------------------------------------

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
	if (!spudaudio_session_find_port(&device->head.allocator, device->direction, device->persistent_id, NULL, NULL))
		return SPUDRESULT_SAUD_DEVICE_LOST;

	// The session reports no range: the IO buffer it has now is the one
	// period known to be granted.
	spudaudio_session_state session;
	spudaudio_session_read(&session);
	const uint32_t rate                 = (uint32_t)lround(session.sample_rate);
	const uint32_t current              = rio_frames(session.io_buffer_duration, rate);
	out_caps->period_mode               = SPUDAUDIO_PERIOD_MODE_EXACT;
	out_caps->min_period_frames         = current;
	out_caps->max_period_frames         = current;
	out_caps->default_period_frames     = current;
	out_caps->period_granularity_frames = 1;
	out_caps->variable_callback_frames  = true;
	// min/max_buffer_frames stay 0: no buffer separate from the period.

	// A converted stream's period is in its own frames.
	if ((flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION) && format->sample_rate != 0 && rate != 0 && format->sample_rate != rate) {
		out_caps->min_period_frames     = rio_scale_frames(current, rate, format->sample_rate, 1);
		out_caps->max_period_frames     = rio_scale_frames(current, rate, format->sample_rate, -1);
		out_caps->default_period_frames = rio_scale_frames(current, rate, format->sample_rate, 0);
		if (out_caps->max_period_frames < out_caps->min_period_frames)
			out_caps->max_period_frames = out_caps->min_period_frames;
	}
	return SPUD_SUCCESS;
}

static SPUDRESULT rio_check_thread(const SPUDAUDIO_THREAD_DESC *t) {
	// The unit's own real-time render thread runs the callback.
	return t->priority == SPUDAUDIO_THREAD_PRIORITY_PLATFORM ? SPUD_SUCCESS : SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED;
}

// What rio_check_desc worked out about the route side of a stream.
struct rio_plan {
	SPUDAUDIO_FORMAT device_format; // the port's own format
	bool converted;                 // the unit converts between the two
	AudioComponentInstance unit;    // made for the stream, client format set; NULL on failure
	double restore_io_duration;     // the session's preferred IO buffer duration before the plan set it
	bool io_duration_set;           // the plan changed it
};

/*
 * The one check behind probe and create. The port must be in the route,
 * alone in its direction; an INPUT stream needs record permission. Then
 * format, period and buffer, in that order, each against what the session
 * reports, with its own value as the suggestion; then exclusive mode,
 * which RemoteIO doesn't have. The client format is set on a RemoteIO unit
 * made here, so whether the unit takes it is the unit's answer.
 *
 * The period is set as the session's preferred IO buffer duration and read
 * back: the plan leaves it set, with the old value in `plan` for whoever
 * must put it back. On failure it is already put back, and `plan->unit`
 * disposed.
 */
static SPUDRESULT rio_plan_stream(const SPUDAUDIO_STREAM_DESC *desc, SPUDAUDIO_STREAM_CONFIG *cfg, struct rio_plan *plan) {
	const struct spudaudio_device_t *device = desc->device;
	spudaudio_session_port port;
	uint32_t route_ports = 0;
	if (!spudaudio_session_find_port(&device->head.allocator, device->direction, device->persistent_id, &port, &route_ports))
		return SPUDRESULT_SAUD_DEVICE_LOST;
	if (route_ports != 1)
		return SPUDRESULT_NOT_IMPLEMENTED_YET; // a multi-route port: its channels would need a channel map
	if (device->direction == SPUDAUDIO_DIRECTION_INPUT && !spudaudio_session_record_permission_granted())
		return SPUDRESULT_SAUD_ACCESS_DENIED;

	spudaudio_session_state session;
	spudaudio_session_read(&session);
	const uint32_t device_rate = (uint32_t)lround(session.sample_rate);
	rio_device_format(&port, device_rate, &plan->device_format);
	const SPUDAUDIO_FORMAT *device_format = &plan->device_format;

	// --- Format.
	cfg->format = desc->format;
	if (desc->format.channel_count != device_format->channel_count) {
		cfg->format.channel_count = device_format->channel_count;
		memcpy(cfg->format.positions, device_format->positions, sizeof(cfg->format.positions));
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	// A layout must be the port's own.
	if (spudaudio_format_has_layout(&desc->format) && memcmp(desc->format.positions, device_format->positions, desc->format.channel_count) != 0) {
		memcpy(cfg->format.positions, device_format->positions, sizeof(cfg->format.positions));
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	plan->converted = rio_format_needs_conversion(&desc->format, device_format);
	if (plan->converted && !(desc->flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION)) {
		if (desc->format.sample_format != SPUDAUDIO_SAMPLE_FORMAT_F32)
			cfg->format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
		else
			cfg->format.sample_rate = device_rate; // or spudaudio_set_device_sample_rate
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	SPUDRESULT r = rio_new_unit(&plan->unit);
	if (SPUDFAIL(r))
		return r;
	AudioStreamBasicDescription asbd;
	if (!rio_format_to_asbd(&desc->format, &asbd) || rio_set_client_format(plan->unit, device->direction, &asbd) != noErr) {
		// The unit won't take it: suggest what needs no conversion.
		cfg->format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
		cfg->format.sample_rate   = device_rate;
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}

	// --- Period: asked of the session in route frames, granted or not.
	const uint32_t rate          = desc->format.sample_rate;
	const uint32_t device_period = plan->converted ? rio_scale_frames(desc->period_frames, rate, device_rate, 0) : desc->period_frames;
	cfg->period_mode             = SPUDAUDIO_PERIOD_MODE_EXACT;
	plan->restore_io_duration    = session.preferred_io_duration;
	r                            = spudaudio_session_set_preferred_io_duration((double)device_period / (double)device_rate);
	if (SPUDFAIL(r))
		return r;
	plan->io_duration_set = true;
	spudaudio_session_state granted;
	spudaudio_session_read(&granted);
	const uint32_t granted_period = rio_frames(granted.io_buffer_duration, device_rate);
	if (granted_period != device_period || device_period == 0) {
		cfg->period_frames = plan->converted ? rio_scale_frames(granted_period, device_rate, rate, 0) : granted_period;
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	// The most frames one render may ask for, as the platform has set it.
	UInt32 max_frames = 0;
	UInt32 size       = sizeof(max_frames);
	r = rio_result(AudioUnitGetProperty(plan->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, &size));
	if (SPUDFAIL(r))
		return r;
	if (device_period > max_frames) {
		cfg->period_frames = plan->converted ? rio_scale_frames(max_frames, device_rate, rate, -1) : max_frames;
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	cfg->period_frames = desc->period_frames;

	// --- Buffer: none separate from the period.
	if (desc->buffer_frames != 0) {
		cfg->buffer_frames = 0;
		return SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
	}

	// --- Exclusive: RemoteIO has none.
	if (desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE)
		return SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;

	cfg->max_callback_frames = plan->converted ? rio_scale_frames(max_frames, device_rate, rate, -1) : max_frames;
	return SPUD_SUCCESS;
}

// Puts back what a plan changed in the session and disposes of its unit.
static void rio_plan_undo(struct rio_plan *plan) {
	if (plan->io_duration_set)
		spudaudio_session_set_preferred_io_duration(plan->restore_io_duration);
	plan->io_duration_set = false;
	if (plan->unit)
		AudioComponentInstanceDispose(plan->unit);
	plan->unit = NULL;
}

static SPUDRESULT rio_check_desc(const SPUDAUDIO_STREAM_DESC *desc, SPUDAUDIO_STREAM_CONFIG *cfg, struct rio_plan *plan) {
	memset(plan, 0, sizeof(*plan));
	const SPUDRESULT r = rio_plan_stream(desc, cfg, plan);
	if (SPUDFAIL(r))
		rio_plan_undo(plan);
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
	r = rio_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;
	struct rio_plan plan;
	r = rio_check_desc(desc, out_config, &plan);
	// A probe leaves the session as it found it.
	rio_plan_undo(&plan);
	return r;
}

// --------------------------------------------------------------------------
// Render
// --------------------------------------------------------------------------

// Silence in every buffer of `list`.
static void rio_silence(AudioBufferList *list) {
	if (!list)
		return;
	for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
		if (list->mBuffers[b].mData)
			memset(list->mBuffers[b].mData, 0, list->mBuffers[b].mDataByteSize);
}

// This render's flags and position, from its timestamp. A render that
// starts past where the last one ended skipped frames: the DISCONTINUITY,
// and the position jumps by the frames missed.
static void rio_time(struct spudaudio_stream_t *s, const AudioTimeStamp *time, uint32_t frames, SPUDAUDIO_CALLBACK_INFO *info) {
	if (s->pending_discontinuity) {
		info->flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
		s->pending_discontinuity = false;
	}
	if (time && (time->mFlags & kAudioTimeStampSampleTimeValid)) {
		if (!s->have_origin) {
			s->have_origin          = true;
			s->origin_sample_time   = time->mSampleTime;
			s->expected_sample_time = time->mSampleTime;
		}
		if (time->mSampleTime > s->expected_sample_time + 0.5 && !(info->flags & SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY)) {
			info->flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
			atomic_fetch_add(s->direction == SPUDAUDIO_DIRECTION_OUTPUT ? &s->underflow_count : &s->overflow_count, 1);
		}
		info->device_position_frames = (uint64_t)(time->mSampleTime - s->origin_sample_time);
		s->expected_sample_time      = time->mSampleTime + frames;
	} else {
		info->device_position_frames = s->position_frames;
	}
	s->position_frames = info->device_position_frames + frames;
	if (time && (time->mFlags & kAudioTimeStampHostTimeValid)) {
		info->host_time_ns = spudaudio_apple_host_time_to_ns(time->mHostTime, &s->timebase);
		info->flags |= SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
	}
}

// A render past the promised bound: the callback is not called, the
// frames are silence, and the next callback carries the DISCONTINUITY.
static void rio_skip(struct spudaudio_stream_t *s) {
	s->pending_discontinuity = true;
	atomic_fetch_add(s->direction == SPUDAUDIO_DIRECTION_OUTPUT ? &s->underflow_count : &s->overflow_count, 1);
}

// The callback's `frames` over `list`: its one buffer, or one plane per
// channel. NULL where the list isn't shaped as the client format says.
static void *rio_frames_of(struct spudaudio_stream_t *s, AudioBufferList *list) {
	const uint32_t channels = s->config.format.channel_count;
	if (s->config.format.interleaved)
		return list->mNumberBuffers == 1 ? list->mBuffers[0].mData : NULL;
	if (list->mNumberBuffers != channels)
		return NULL;
	for (uint32_t ch = 0; ch < channels; ++ch)
		s->planes[ch] = list->mBuffers[ch].mData;
	return (void *)s->planes;
}

/*
 * OUTPUT: runs on the unit's real-time render thread. No allocation, no
 * lock, no Objective-C: the callback fills the unit's own buffers in
 * place, through planes made at creation.
 */
static OSStatus rio_render(
    void *client,
    AudioUnitRenderActionFlags *action_flags,
    const AudioTimeStamp *time,
    UInt32 bus,
    UInt32 frames,
    AudioBufferList *io) {
	(void)bus;
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)client;
	s->callback_thread           = pthread_self();
	atomic_store(&s->in_callback, true);

	void *frames_ptr = io ? rio_frames_of(s, io) : NULL;
	if (atomic_load(&s->state) != SPUDAUDIO_STREAM_STATE_RUNNING || !frames_ptr || frames > s->config.max_callback_frames) {
		// Stopping, ended, or a render the stream can't take: silence.
		if (frames > s->config.max_callback_frames)
			rio_skip(s);
		rio_silence(io);
		if (action_flags)
			*action_flags |= kAudioUnitRenderAction_OutputIsSilence;
		atomic_store(&s->in_callback, false);
		return noErr;
	}
	SPUDAUDIO_CALLBACK_INFO info = {0};
	rio_time(s, time, frames, &info);
	s->callback(s, frames_ptr, frames, &info, s->user_data);
	atomic_store(&s->in_callback, false);
	return noErr;
}

/*
 * INPUT: runs on the unit's real-time thread when bus 1 has frames. They
 * are rendered into the buffers made at creation, then handed over.
 */
static OSStatus rio_input(
    void *client,
    AudioUnitRenderActionFlags *action_flags,
    const AudioTimeStamp *time,
    UInt32 bus,
    UInt32 frames,
    AudioBufferList *unused) {
	(void)bus;
	(void)unused;
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)client;
	s->callback_thread           = pthread_self();
	atomic_store(&s->in_callback, true);

	if (atomic_load(&s->state) != SPUDAUDIO_STREAM_STATE_RUNNING || frames > s->config.max_callback_frames) {
		if (frames > s->config.max_callback_frames)
			rio_skip(s);
		atomic_store(&s->in_callback, false);
		return noErr;
	}
	const uint32_t channels     = s->config.format.channel_count;
	const uint32_t sample_bytes = (uint32_t)sizeof(float);
	AudioBufferList *list       = s->input_list;
	if (s->config.format.interleaved) {
		list->mBuffers[0].mNumberChannels = channels;
		list->mBuffers[0].mDataByteSize   = frames * channels * sample_bytes;
		list->mBuffers[0].mData           = s->input_samples;
	} else {
		for (uint32_t ch = 0; ch < channels; ++ch) {
			list->mBuffers[ch].mNumberChannels = 1;
			list->mBuffers[ch].mDataByteSize   = frames * sample_bytes;
			list->mBuffers[ch].mData           = s->input_samples + (size_t)ch * s->config.max_callback_frames;
		}
	}
	AudioUnitRenderActionFlags flags = action_flags ? *action_flags : 0;
	if (AudioUnitRender(s->unit, &flags, time, RIO_INPUT_BUS, frames, list) != noErr) {
		// Nothing captured this time: the frames are lost.
		rio_skip(s);
		atomic_store(&s->in_callback, false);
		return noErr;
	}
	void *frames_ptr = rio_frames_of(s, list);
	SPUDAUDIO_CALLBACK_INFO info = {0};
	rio_time(s, time, frames, &info);
	s->callback(s, frames_ptr, frames, &info, s->user_data);
	atomic_store(&s->in_callback, false);
	return noErr;
}

// --------------------------------------------------------------------------
// Streams
// --------------------------------------------------------------------------

static bool rio_is_terminal(unsigned int state) {
	return state == SPUDAUDIO_STREAM_STATE_DEVICE_LOST || state == SPUDAUDIO_STREAM_STATE_ERROR;
}

// The first terminal state sticks.
static void rio_set_terminal(struct spudaudio_stream_t *s, SPUDRESULT r) {
	unsigned int state = atomic_load(&s->state);
	if (rio_is_terminal(state))
		return;
	atomic_store(&s->error, (int)r);
	atomic_store(&s->state, r == SPUDRESULT_SAUD_DEVICE_LOST ? SPUDAUDIO_STREAM_STATE_DEVICE_LOST : SPUDAUDIO_STREAM_STATE_ERROR);
}

// Runs on the stream's queue: the session told of something that may end
// the stream. An ended stream's unit is stopped here, so the render stops
// too; nothing is ever restarted.
static void rio_stream_event(uint32_t event, void *context) {
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)context;
	if (rio_is_terminal(atomic_load(&s->state)))
		return;
	switch (event) {
	case SPUDAUDIO_SESSION_EVENT_ROUTE_CHANGE: {
		spudaudio_session_port port;
		uint32_t route_ports = 0;
		if (!spudaudio_session_find_port(&s->head.allocator, s->direction, s->persistent_id, &port, &route_ports)) {
			rio_set_terminal(s, SPUDRESULT_SAUD_DEVICE_LOST);
			break;
		}
		if (route_ports != 1) {
			rio_set_terminal(s, SPUDRESULT_NOT_IMPLEMENTED_YET); // now a multi-route port
			break;
		}
		spudaudio_session_state session;
		spudaudio_session_read(&session);
		if ((uint32_t)lround(session.sample_rate) != s->device_rate || port.channel_count != s->device_channels)
			rio_set_terminal(s, SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED);
		break;
	}
	case SPUDAUDIO_SESSION_EVENT_INTERRUPTION_BEGAN:
		// The system has stopped the unit; a stopped stream isn't changed.
		if (atomic_load(&s->state) == SPUDAUDIO_STREAM_STATE_RUNNING)
			rio_set_terminal(s, SPUDRESULT_SAUD_INTERRUPTED);
		break;
	case SPUDAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST:
	case SPUDAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET:
		// Every audio unit of the process is gone with them.
		rio_set_terminal(s, SPUDRESULT_SAUD_DEVICE_LOST);
		break;
	default:
		break; // an interruption's end: the caller's to act on
	}
	if (rio_is_terminal(atomic_load(&s->state)))
		AudioOutputUnitStop(s->unit);
}

static void rio_free_stream(struct spudaudio_stream_t *s) {
	spudaudio_session_unobserve(s->observer);
	if (s->unit) {
		AudioUnitUninitialize(s->unit);
		AudioComponentInstanceDispose(s->unit);
	}
	const SPUD_ALLOCATOR allocator = s->head.allocator;
	SPUD_DELETE_ARRAY(&allocator, void *, s->planes, s->planes_count);
	SPUD_DELETE_ARRAY(&allocator, float, s->input_samples, s->input_sample_count);
	spud_free(&allocator, s->input_list, s->input_list_bytes, SPUD_MAX_ALIGN);
	spud_string_free(&allocator, s->head.debug_name);
	SPUD_DELETE(&allocator, struct spudaudio_stream_t, s);
}

// The unit's IO, callbacks and buffers for the stream's direction, then
// the unit initialized. Every block here is made now, so the render
// callback never allocates.
static SPUDRESULT rio_setup_unit(struct spudaudio_stream_t *s) {
	const SPUD_ALLOCATOR *allocator = &s->head.allocator;
	const uint32_t channels         = s->config.format.channel_count;
	const bool out                  = s->direction == SPUDAUDIO_DIRECTION_OUTPUT;
	const UInt32 on                 = 1;
	const UInt32 off                = 0;
	OSStatus status = AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, RIO_OUTPUT_BUS, out ? &on : &off, sizeof(on));
	if (status == noErr)
		status = AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, RIO_INPUT_BUS, out ? &off : &on, sizeof(on));
	if (status == noErr) {
		AURenderCallbackStruct callback = {out ? rio_render : rio_input, s};
		status = out ? AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, RIO_OUTPUT_BUS, &callback, sizeof(callback))
		             : AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, RIO_INPUT_BUS, &callback, sizeof(callback));
	}
	if (status == noErr && !out)
		// AudioUnitRender writes into the stream's own buffers.
		status = AudioUnitSetProperty(s->unit, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output, RIO_INPUT_BUS, &off, sizeof(off));
	if (status != noErr)
		return rio_result(status);

	s->planes_count = channels;
	s->planes       = SPUD_NEW_ARRAY_ZEROED(allocator, void *, channels);
	if (!s->planes)
		return SPUDRESULT_OUT_OF_MEMORY;
	if (!out) {
		const uint32_t buffers = s->config.format.interleaved ? 1 : channels;
		s->input_list_bytes    = offsetof(AudioBufferList, mBuffers) + (size_t)buffers * sizeof(AudioBuffer);
		s->input_list          = (AudioBufferList *)spud_allocate(allocator, s->input_list_bytes, SPUD_MAX_ALIGN);
		s->input_sample_count  = (size_t)s->config.max_callback_frames * channels;
		s->input_samples       = SPUD_NEW_ARRAY_ZEROED(allocator, float, s->input_sample_count);
		if (!s->input_list || !s->input_samples)
			return SPUDRESULT_OUT_OF_MEMORY;
		memset(s->input_list, 0, s->input_list_bytes);
		s->input_list->mNumberBuffers = buffers;
	}
	return rio_result(AudioUnitInitialize(s->unit));
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
	r = rio_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;

	struct spudaudio_stream_t *s = SPUD_NEW_ZEROED(&desc->device->head.allocator, struct spudaudio_stream_t);
	if (!s)
		return SPUDRESULT_OUT_OF_MEMORY;
	s->head.allocator = desc->device->head.allocator;
	SPUD_BIRTH(s);
	s->direction = desc->device->direction;
	memcpy(s->persistent_id, desc->device->persistent_id, sizeof(s->persistent_id));
	s->callback  = desc->callback;
	s->user_data = desc->user_data;
	atomic_init(&s->state, SPUDAUDIO_STREAM_STATE_STOPPED);
	atomic_init(&s->error, SPUD_SUCCESS);
	atomic_init(&s->underflow_count, 0);
	atomic_init(&s->overflow_count, 0);
	atomic_init(&s->in_callback, false);
	mach_timebase_info(&s->timebase);

	struct rio_plan plan;
	r = rio_check_desc(desc, &s->config, &plan);
	if (SPUD_SUCCESS == r) {
		s->unit                = plan.unit;
		plan.unit              = NULL; // the stream's now
		s->device_rate         = plan.device_format.sample_rate;
		s->device_channels     = plan.device_format.channel_count;
		r                      = rio_setup_unit(s);
	}
	if (SPUD_SUCCESS == r)
		r = spudaudio_session_observe(&s->head.allocator, "spudaudio.remoteio.stream", rio_stream_event, s, &s->observer);
	if (SPUDFAIL(r)) {
		// A failed create changes nothing: the session's preferred IO
		// buffer duration goes back too (the plan's own failure has put it
		// back already).
		rio_plan_undo(&plan);
		rio_free_stream(s);
		return r;
	}
	*out_stream = s;
	return SPUD_SUCCESS;
}

static bool rio_on_callback_thread(struct spudaudio_stream_t *s) {
	return atomic_load(&s->in_callback) && pthread_equal(pthread_self(), s->callback_thread);
}

SPUDRESULT spudaudio_stream_start(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_start", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (rio_on_callback_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE;
	unsigned int state = atomic_load(&stream->state);
	if (state == SPUDAUDIO_STREAM_STATE_RUNNING)
		return SPUD_SUCCESS;
	if (rio_is_terminal(state))
		return (SPUDRESULT)atomic_load(&stream->error);
	if (stream->direction == SPUDAUDIO_DIRECTION_INPUT && !spudaudio_session_record_permission_granted())
		return SPUDRESULT_SAUD_ACCESS_DENIED;

	stream->have_origin           = false;
	stream->position_frames       = 0;
	stream->pending_discontinuity = false;
	// RUNNING before the unit starts: the render outputs silence unless
	// the stream is RUNNING.
	atomic_store(&stream->state, SPUDAUDIO_STREAM_STATE_RUNNING);
	const OSStatus status = AudioOutputUnitStart(stream->unit);
	if (status != noErr) {
		const SPUDRESULT r = rio_result(status);
		if (r == SPUDRESULT_SAUD_DEVICE_LOST)
			rio_set_terminal(stream, r);
		else {
			unsigned int running = SPUDAUDIO_STREAM_STATE_RUNNING;
			atomic_compare_exchange_strong(&stream->state, &running, SPUDAUDIO_STREAM_STATE_STOPPED);
		}
		return r;
	}
	return SPUD_SUCCESS;
}

// AudioOutputUnitStop called off the render thread returns once the render
// isn't running and won't be called again.
SPUDRESULT spudaudio_stream_stop(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_stop", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (rio_on_callback_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE;
	AudioOutputUnitStop(stream->unit);
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
	rio_free_stream(stream);
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
	*out_config      = stream->config;
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

// The session's latency in the stream's direction plus its IO buffer, as it
// reports them now, at the stream's own rate: seconds restate exactly. A
// converted stream's converter delay is not in it.
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
	spudaudio_session_state session;
	spudaudio_session_read(&session);
	const double latency = stream->direction == SPUDAUDIO_DIRECTION_OUTPUT ? session.output_latency : session.input_latency;
	*out_latency_frames  = rio_frames(latency + session.io_buffer_duration, stream->config.format.sample_rate);
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
	if (rio_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;

	// The new watch is built before the old one goes, so a failure leaves
	// the callback that was set still set.
	struct rio_watch *watch = NULL;
	if (callback) {
		SPUDRESULT r = rio_watch_create(&instance->head.allocator, callback, user_data, &watch);
		if (SPUDFAIL(r))
			return r;
	}
	rio_watch_destroy(instance->watch);
	instance->watch = watch;
	return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#else
// Not built without SPUDAUDIO_COMPILE_REMOTEIO (CMake leaves this file
// out); this keeps a stray build of it a valid, empty translation unit.
typedef int spudaudio_remoteio_not_compiled;
#endif // SPUDAUDIO_COMPILE_REMOTEIO

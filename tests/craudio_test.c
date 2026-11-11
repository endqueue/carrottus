/*
 * craudio_test — Carrottus Audio without a sound.
 *
 * Every call, with nothing heard and no device changed:
 * craudio_device_set_sample_rate() is only given arguments it refuses or
 * the rate the device has, and the one stream started writes silence. That
 * covers each call's argument checks; the instance, enumeration and device
 * properties in both directions; timing caps; a probe of the default output
 * device's own format, following its suggestions, and the probe's refusals
 * of what the backend doesn't have; a stream created stopped, its
 * configuration, latency and status read, and destroyed; a stream started
 * writing silence, its callback's frames, flags and positions checked, its
 * status read from inside the callback and out, stopped and started again;
 * and setting and clearing the device-event callback. Every block goes
 * through a counting allocator, checked at the end. craudio_sine is the
 * test that plays something.
 *
 * With no output device, the parts that need one are skipped and the rest
 * still run. A build with no native audio API checks that the stub refuses.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <craudio.h>
#include <crperf.h>

#include "crtest_allocator.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define CHECK_RESULT(call, expected)                                                                                         \
	do {                                                                                                                     \
		const CR_RESULT r_ = (call);                                                                                        \
		if (r_ != (expected)) {                                                                                              \
			fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, #call, cr_result_str(r_), #expected); \
			++failures;                                                                                                      \
		}                                                                                                                    \
	} while (0)

#if CRAUDIO_COMPILE_COREAUDIO
static const CRAUDIO_NATIVE_API g_api        = CRAUDIO_NATIVE_API_COREAUDIO;
static const CRAUDIO_THREAD_PRIORITY g_thread = CRAUDIO_THREAD_PRIORITY_PLATFORM;
#elif CRAUDIO_COMPILE_WASAPI
static const CRAUDIO_NATIVE_API g_api        = CRAUDIO_NATIVE_API_WASAPI;
static const CRAUDIO_THREAD_PRIORITY g_thread = CRAUDIO_THREAD_PRIORITY_NORMAL;
#elif CRAUDIO_COMPILE_ALSA
static const CRAUDIO_NATIVE_API g_api        = CRAUDIO_NATIVE_API_ALSA;
static const CRAUDIO_THREAD_PRIORITY g_thread = CRAUDIO_THREAD_PRIORITY_NORMAL;
#elif CRAUDIO_COMPILE_REMOTEIO
static const CRAUDIO_NATIVE_API g_api        = CRAUDIO_NATIVE_API_REMOTEIO;
static const CRAUDIO_THREAD_PRIORITY g_thread = CRAUDIO_THREAD_PRIORITY_PLATFORM;
#else
static const CRAUDIO_NATIVE_API g_api        = CRAUDIO_NATIVE_API_NONE;
static const CRAUDIO_THREAD_PRIORITY g_thread = CRAUDIO_THREAD_PRIORITY_NORMAL;
#endif

/* Never called: no stream is started. */
static void silent_callback(
    craudio_stream stream,
    void *frames,
    uint32_t frame_count,
    const CRAUDIO_CALLBACK_INFO *info,
    void *user_data) {
	(void)stream;
	(void)frames;
	(void)frame_count;
	(void)info;
	*(int *)user_data = 1;
}

/* Carrottus Perf's wait on the monotonic clock: the same call on every platform. */
static void sleep_ms(long ms) {
	crperf_sleep_until_monotonic_ns(crperf_get_monotonic_time_ns() + (uint64_t)ms * 1000000u);
}

/* What a running stream's callbacks saw, written on the callback's thread
 * and read on main. */
typedef struct run_record {
	uint32_t channels;
	uint32_t sample_bytes;
	bool interleaved;
	uint32_t max_callback_frames;
	atomic_uint calls;
	atomic_uint too_many_frames;   /* frame_count past max_callback_frames */
	atomic_uint host_time_valid;   /* calls with HOST_TIME_VALID */
	atomic_uint went_back;         /* a position before the last one's end */
	atomic_uint status_failed;     /* craudio_stream_get_status from inside, failed */
	uint64_t next_position;        /* callback thread only */
} run_record;

/* Writes silence, and records what it was given. */
static void run_callback(
    craudio_stream stream,
    void *frames,
    uint32_t frame_count,
    const CRAUDIO_CALLBACK_INFO *info,
    void *user_data) {
	run_record *r = (run_record *)user_data;
	if (frame_count > r->max_callback_frames)
		atomic_fetch_add(&r->too_many_frames, 1);
	else if (r->interleaved)
		memset(frames, 0, (size_t)frame_count * r->channels * r->sample_bytes);
	else
		for (uint32_t c = 0; c < r->channels; ++c)
			memset(((void **)frames)[c], 0, (size_t)frame_count * r->sample_bytes);
	if (info->flags & CRAUDIO_CALLBACK_FLAG_HOST_TIME_VALID)
		atomic_fetch_add(&r->host_time_valid, 1);
	if (atomic_load(&r->calls) > 0 && info->device_position_frames < r->next_position)
		atomic_fetch_add(&r->went_back, 1);
	r->next_position = info->device_position_frames + frame_count;
	CRAUDIO_STREAM_STATUS status;
	if (craudio_stream_get_status(stream, &status) != CR_SUCCESS || status.state != CRAUDIO_STREAM_STATE_RUNNING)
		atomic_fetch_add(&r->status_failed, 1);
	atomic_fetch_add(&r->calls, 1);
}

static void device_event_callback(
    CRAUDIO_DEVICE_EVENT event,
    const char *persistent_id,
    CRAUDIO_DIRECTION direction,
    CRAUDIO_DEFAULT_ROLES roles,
    void *user_data) {
	(void)event;
	(void)persistent_id;
	(void)direction;
	(void)roles;
	(void)user_data;
}

static void test_sample_format_sizes(void) {
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_UNKNOWN) == 0);
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_S16) == 2);
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_S24_PACKED) == 3);
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_S24_32_LSB) == 4);
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_S32) == 4);
	CHECK(craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT_F32) == 4);
	CHECK(craudio_sample_format_get_byte_size((CRAUDIO_SAMPLE_FORMAT)99) == 0);
}

static void test_create_instance_checks(const CR_ALLOCATOR *allocator) {
	craudio_instance instance = NULL;
	CRAUDIO_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));

	CHECK_RESULT(craudio_instance_create(NULL, &instance), CR_RESULT_NULL_DESC);
	/* A zeroed desc is refused for its base, then one with only its base
	 * for its allocator. */
	CHECK_RESULT(craudio_instance_create(&desc, &instance), CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	desc.base.next = (const CR_STRUCT_BASE *)&desc;
	CHECK_RESULT(craudio_instance_create(&desc, &instance), CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;
	CHECK_RESULT(craudio_instance_create(&desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_instance_create(&desc, &instance), CR_RESULT_NULL_ALLOCATOR);
	CR_ALLOCATOR missing = *allocator;
	missing.free           = NULL;
	desc.allocator         = &missing;
	CHECK_RESULT(craudio_instance_create(&desc, &instance), CR_RESULT_INVALID_ALLOCATOR);
	desc.allocator  = allocator;
	desc.native_api = CRAUDIO_NATIVE_API_NONE;
	CHECK(craudio_instance_create(&desc, &instance) != CR_SUCCESS);
	CHECK(instance == NULL);
	desc.native_api = (CRAUDIO_NATIVE_API)99;
	CHECK(craudio_instance_create(&desc, &instance) != CR_SUCCESS);
	CHECK(instance == NULL);

	CHECK_RESULT(craudio_instance_destroy(NULL), CR_RESULT_AUDIO_INVALID_INSTANCE);
	CHECK(craudio_instance_get_native_api(NULL) == CRAUDIO_NATIVE_API_NONE);
}

/* The calls that take a device, a stream or an instance, given none. */
static void test_null_handles(void) {
	craudio_device *devices = NULL;
	uint32_t count            = 0;
	CHECK_RESULT(craudio_device_enumerate(NULL, CRAUDIO_DIRECTION_OUTPUT, &devices, &count), CR_RESULT_AUDIO_INVALID_INSTANCE);

	CRAUDIO_DEVICE_PROPERTIES props = {.base = {CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES, NULL}};
	CHECK_RESULT(craudio_device_get_properties(NULL, &props), CR_RESULT_AUDIO_INVALID_DEVICE);
	CHECK_RESULT(craudio_device_set_sample_rate(NULL, 48000), CR_RESULT_AUDIO_INVALID_DEVICE);

	CRAUDIO_FORMAT format       = {0};
	CRAUDIO_TIMING_CAPS caps     = {.base = {CR_STRUCT_TYPE_AUDIO_TIMING_CAPS, NULL}};
	CHECK_RESULT(craudio_device_get_timing_caps(NULL, &format, CRAUDIO_STREAM_FLAG_NONE, &caps), CR_RESULT_AUDIO_INVALID_DEVICE);

	CRAUDIO_STREAM_CONFIG config = {.base = {CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG, NULL}};
	CRAUDIO_STREAM_STATUS status;
	uint32_t latency = 0;
	CHECK_RESULT(craudio_stream_start(NULL), CR_RESULT_AUDIO_INVALID_STREAM);
	CHECK_RESULT(craudio_stream_stop(NULL), CR_RESULT_AUDIO_INVALID_STREAM);
	CHECK_RESULT(craudio_stream_get_config(NULL, &config), CR_RESULT_AUDIO_INVALID_STREAM);
	CHECK_RESULT(craudio_stream_get_latency_frames(NULL, &latency), CR_RESULT_AUDIO_INVALID_STREAM);
	CHECK_RESULT(craudio_stream_get_status(NULL, &status), CR_RESULT_AUDIO_INVALID_STREAM);
	craudio_stream_destroy(NULL);

	CHECK_RESULT(craudio_instance_set_device_event_callback(NULL, device_event_callback, NULL), CR_RESULT_AUDIO_INVALID_INSTANCE);
}

/* Each direction's devices and their properties. Returns the output device
 * holding the general default role, else the first, else NULL. */
static craudio_device test_enumerate(craudio_instance instance) {
	craudio_device *devices = NULL;
	uint32_t count            = 0;
	CHECK_RESULT(craudio_device_enumerate(instance, CRAUDIO_DIRECTION_OUTPUT, NULL, &count), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_device_enumerate(instance, CRAUDIO_DIRECTION_OUTPUT, &devices, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_device_enumerate(instance, (CRAUDIO_DIRECTION)99, &devices, &count), CR_RESULT_DESC_INVALID_PARAMETERS);

	craudio_device chosen = NULL;
	const CRAUDIO_DIRECTION directions[2] = {CRAUDIO_DIRECTION_OUTPUT, CRAUDIO_DIRECTION_INPUT};
	for (int d = 0; d < 2; ++d) {
		CHECK_RESULT(craudio_device_enumerate(instance, directions[d], &devices, &count), CR_SUCCESS);
		printf("craudio_test: %u %s device(s)\n", (unsigned)count, d == 0 ? "output" : "input");
		if (g_api == CRAUDIO_NATIVE_API_REMOTEIO && d == 1 && count == 0)
			printf("craudio_test: the session's route has no input (its category has none, or no input is available)\n");
		for (uint32_t i = 0; i < count; ++i) {
			CRAUDIO_DEVICE_PROPERTIES props;
			memset(&props, 0, sizeof(props));
			props.base.type = CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
			CHECK_RESULT(craudio_device_get_properties(devices[i], &props), CR_SUCCESS);
			CHECK(props.direction == directions[d]);
			CHECK(memchr(props.name, '\0', sizeof(props.name)) != NULL);
			CHECK(props.persistent_id[0] != '\0');
			printf("craudio_test:   %s\n", props.name);
			if (d == 0 && !chosen)
				chosen = devices[i];
			if (d == 0 && (props.default_roles & CRAUDIO_DEFAULT_ROLE_GENERAL))
				chosen = devices[i];
			/* A route of one port in a direction: that port is GENERAL. */
			if (g_api == CRAUDIO_NATIVE_API_REMOTEIO && count == 1)
				CHECK(props.default_roles == CRAUDIO_DEFAULT_ROLE_GENERAL);
		}
		/* The same endpoint keeps its handle across enumerations. */
		craudio_device first = count ? devices[0] : NULL;
		uint32_t again         = 0;
		CHECK_RESULT(craudio_device_enumerate(instance, directions[d], &devices, &again), CR_SUCCESS);
		if (first && again)
			CHECK(devices[0] == first);
	}

	if (chosen) {
		CRAUDIO_DEVICE_PROPERTIES props;
		memset(&props, 0, sizeof(props));
		CHECK_RESULT(craudio_device_get_properties(chosen, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK_RESULT(craudio_device_get_properties(chosen, &props), CR_RESULT_STRUCT_WRONG_TYPE);
		/* A rate of 0 is refused before anything is set. */
		CHECK_RESULT(craudio_device_set_sample_rate(chosen, 0), CR_RESULT_DESC_INVALID_PARAMETERS);
	}
	return chosen;
}

/* A stream of [desc] started, writing silence: its callback is called with
 * no more than max_callback_frames, positions that only go forward, the
 * host time where the platform gives one, and its status readable inside;
 * stop returns with no callback running, and the stream starts again. A
 * start the platform refuses is printed and checked against the codes the
 * header gives for it. */
static void test_run(const CRAUDIO_STREAM_DESC *stopped_desc, const CRAUDIO_STREAM_CONFIG *granted) {
	run_record record;
	memset(&record, 0, sizeof(record));
	record.channels            = granted->format.channel_count;
	record.sample_bytes        = craudio_sample_format_get_byte_size(granted->format.sample_format);
	record.interleaved         = granted->format.interleaved;
	record.max_callback_frames = granted->max_callback_frames;
	CRAUDIO_STREAM_DESC desc = *stopped_desc;
	desc.callback              = run_callback;
	desc.user_data             = &record;
	craudio_stream stream    = NULL;
	CHECK_RESULT(craudio_stream_create(&desc, &stream), CR_SUCCESS);
	if (!stream)
		return;
	CR_RESULT r = craudio_stream_start(stream);
	if (r != CR_SUCCESS) {
		printf("craudio_test: the stream didn't start: %s\n", cr_result_str(r));
		CHECK(r == CR_RESULT_AUDIO_INTERRUPTED || r == CR_RESULT_AUDIO_SESSION_INACTIVE || r == CR_RESULT_AUDIO_DEVICE_LOST);
		CRAUDIO_STREAM_STATUS status;
		CHECK_RESULT(craudio_stream_get_status(stream, &status), CR_SUCCESS);
		CHECK(status.state != CRAUDIO_STREAM_STATE_RUNNING);
		craudio_stream_destroy(stream);
		return;
	}
	CHECK_RESULT(craudio_stream_start(stream), CR_SUCCESS); /* already running: nothing */
	/* Twenty callbacks, or five seconds. */
	int first = -1, waited = 0;
	for (; waited < 5000 && atomic_load(&record.calls) < 20; waited += 10) {
		if (first < 0 && atomic_load(&record.calls) > 0)
			first = waited;
		sleep_ms(10);
	}
	printf("craudio_test: the first callback came within %d ms of the start, the twentieth within %d ms\n", first, waited);
	CRAUDIO_STREAM_STATUS status;
	CHECK_RESULT(craudio_stream_get_status(stream, &status), CR_SUCCESS);
	CHECK(status.state == CRAUDIO_STREAM_STATE_RUNNING);
	CHECK_RESULT(craudio_stream_stop(stream), CR_SUCCESS);
	const unsigned calls = atomic_load(&record.calls);
	printf("craudio_test: a silent stream ran: %u callback(s), %u with a host time, %llu underflow(s)\n", calls,
	    atomic_load(&record.host_time_valid), (unsigned long long)status.underflow_count);
	CHECK(calls >= 20);
	CHECK(atomic_load(&record.too_many_frames) == 0);
	CHECK(atomic_load(&record.went_back) == 0);
	CHECK(atomic_load(&record.status_failed) == 0);
	/* Stop has returned: no callback runs after it. */
	sleep_ms(50);
	CHECK(atomic_load(&record.calls) == calls);
	CHECK_RESULT(craudio_stream_get_status(stream, &status), CR_SUCCESS);
	CHECK(status.state == CRAUDIO_STREAM_STATE_STOPPED);
	/* And again. */
	atomic_store(&record.calls, 0);
	CHECK_RESULT(craudio_stream_start(stream), CR_SUCCESS);
	for (int waited = 0; waited < 5000 && atomic_load(&record.calls) < 5; waited += 10)
		sleep_ms(10);
	CHECK(atomic_load(&record.calls) >= 5);
	craudio_stream_destroy(stream); /* stops it first */
}

/* A probe of [device]'s own format (or float stereo where it reports none),
 * following its suggestions, then a stream created stopped. */
static void test_stream(craudio_device device) {
	CRAUDIO_DEVICE_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.base.type = CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
	CHECK_RESULT(craudio_device_get_properties(device, &props), CR_SUCCESS);

	CRAUDIO_STREAM_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_DESC;
	desc.device    = device;
	if (props.native_format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN) {
		desc.format = props.native_format;
	} else {
		desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
		desc.format.sample_rate   = 48000;
		desc.format.channel_count = 2;
		desc.format.interleaved   = true;
	}
	desc.flags           = CRAUDIO_STREAM_FLAG_NONE;
	desc.thread.priority = g_thread;

	CRAUDIO_TIMING_CAPS caps;
	memset(&caps, 0, sizeof(caps));
	caps.base.type = CR_STRUCT_TYPE_AUDIO_TIMING_CAPS;
	CHECK_RESULT(craudio_device_get_timing_caps(device, NULL, desc.flags, &caps), CR_RESULT_NULL_DESC);
	CHECK_RESULT(craudio_device_get_timing_caps(device, &desc.format, desc.flags, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CR_RESULT r = craudio_device_get_timing_caps(device, &desc.format, desc.flags, &caps);
	if (g_api == CRAUDIO_NATIVE_API_REMOTEIO && r == CR_SUCCESS) {
		/* The period is the session's IO buffer in force, the only one:
		 * min, max and default, and the frames vary. */
		CHECK(caps.period_mode == CRAUDIO_PERIOD_MODE_FIXED);
		CHECK(caps.min_period_frames == caps.default_period_frames);
		CHECK(caps.max_period_frames == caps.default_period_frames);
		CHECK(caps.variable_callback_frames);
		CHECK(caps.max_buffer_frames == 0);
		CHECK(!props.supports_exclusive);
		CHECK(props.native_format.sample_format == CRAUDIO_SAMPLE_FORMAT_F32);
	}
	if (r == CR_SUCCESS) {
		CHECK(caps.min_period_frames <= caps.max_period_frames);
		CHECK(caps.min_buffer_frames <= caps.max_buffer_frames);
		CHECK(caps.period_granularity_frames >= 1);
		desc.period_frames = caps.default_period_frames ? caps.default_period_frames : caps.min_period_frames;
		if (caps.max_buffer_frames) {
			desc.buffer_frames = desc.period_frames * 2;
			if (desc.buffer_frames < caps.min_buffer_frames)
				desc.buffer_frames = caps.min_buffer_frames;
			if (desc.buffer_frames > caps.max_buffer_frames)
				desc.buffer_frames = caps.max_buffer_frames;
		}
	} else {
		printf("craudio_test: timing caps: %s\n", cr_result_str(r));
		desc.period_frames = desc.format.sample_rate / 100;
		desc.buffer_frames = g_api == CRAUDIO_NATIVE_API_ALSA ? desc.period_frames * 2 : 0;
	}

	/* The probe's checks. */
	CRAUDIO_STREAM_CONFIG config;
	memset(&config, 0, sizeof(config));
	config.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
	CHECK_RESULT(craudio_stream_probe(&desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_stream_probe(NULL, &config), CR_RESULT_NULL_DESC);
	CRAUDIO_STREAM_DESC bad = desc;
	bad.device                = NULL;
	CHECK_RESULT(craudio_stream_probe(&bad, &config), CR_RESULT_AUDIO_INVALID_DEVICE);
	bad                 = desc;
	bad.base.type       = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	CHECK_RESULT(craudio_stream_probe(&bad, &config), CR_RESULT_STRUCT_WRONG_TYPE);
	bad                 = desc;
	bad.thread.priority = CRAUDIO_THREAD_PRIORITY_UNSPECIFIED;
	CHECK(craudio_stream_probe(&bad, &config) != CR_SUCCESS);
	bad                 = desc;
	bad.period_frames   = 0;
	CHECK_RESULT(craudio_stream_probe(&bad, &config), CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE);
	CRAUDIO_STREAM_CONFIG wrong;
	memset(&wrong, 0, sizeof(wrong));
	CHECK_RESULT(craudio_stream_probe(&desc, &wrong), CR_RESULT_STRUCT_WRONG_TYPE);
	if (g_api == CRAUDIO_NATIVE_API_COREAUDIO || g_api == CRAUDIO_NATIVE_API_REMOTEIO) {
		/* The platform's thread is the only mechanism. */
		bad                 = desc;
		bad.thread.priority = CRAUDIO_THREAD_PRIORITY_NORMAL;
		CHECK_RESULT(craudio_stream_probe(&bad, &config), CR_RESULT_AUDIO_THREAD_PRIORITY_UNSUPPORTED);
	}

	/* Follow the suggestions until a configuration is accepted. */
	int accepted = 0;
	for (int round = 0; round < 8 && !accepted; ++round) {
		memset(&config, 0, sizeof(config));
		config.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		r                = craudio_stream_probe(&desc, &config);
		if (r == CR_SUCCESS)
			accepted = 1;
		else if (r == CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED && config.format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN)
			desc.format = config.format;
		else if (r == CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE && config.period_frames)
			desc.period_frames = config.period_frames;
		else if (r == CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE)
			desc.buffer_frames = config.buffer_frames;
		else
			break;
	}
	if (!accepted) {
		printf("craudio_test: no configuration accepted (%s); stream not created\n", cr_result_str(r));
		CHECK(accepted);
		return;
	}
	CHECK(config.period_frames == desc.period_frames);
	CHECK(config.max_callback_frames >= config.period_frames);

	if (g_api == CRAUDIO_NATIVE_API_REMOTEIO) {
		CRAUDIO_STREAM_CONFIG suggested;
		memset(&suggested, 0, sizeof(suggested));
		suggested.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		/* What RemoteIO hasn't: exclusive mode, a buffer apart from the
		 * period. */
		bad       = desc;
		bad.flags = CRAUDIO_STREAM_FLAG_EXCLUSIVE;
		CHECK_RESULT(craudio_stream_probe(&bad, &suggested), CR_RESULT_AUDIO_EXCLUSIVE_UNAVAILABLE);
		bad               = desc;
		bad.buffer_frames = desc.period_frames * 2;
		CHECK_RESULT(craudio_stream_probe(&bad, &suggested), CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE);
		CHECK(suggested.buffer_frames == 0);
		/* Another channel count is refused, with the port's suggested. */
		bad                      = desc;
		bad.format.channel_count = desc.format.channel_count + 1;
		memset(bad.format.positions, 0, sizeof(bad.format.positions));
		CHECK_RESULT(craudio_stream_probe(&bad, &suggested), CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED);
		CHECK(suggested.format.channel_count == desc.format.channel_count);
		/* S16 without ALLOW_OS_CONVERSION is refused, F32 suggested; with
		 * it, the unit's converter takes it. */
		bad                      = desc;
		bad.format.sample_format = CRAUDIO_SAMPLE_FORMAT_S16;
		CHECK_RESULT(craudio_stream_probe(&bad, &suggested), CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED);
		CHECK(suggested.format.sample_format == CRAUDIO_SAMPLE_FORMAT_F32);
		bad.flags = CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION;
		r         = craudio_stream_probe(&bad, &suggested);
		printf("craudio_test: S16 through the unit's converter: %s\n", cr_result_str(r));
		CHECK(r == CR_SUCCESS || r == CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED);
		/* Another period is refused, the session's IO buffer in force
		 * suggested, and the session isn't changed: the caps read the same
		 * after. */
		CRAUDIO_TIMING_CAPS after;
		memset(&after, 0, sizeof(after));
		after.base.type   = CR_STRUCT_TYPE_AUDIO_TIMING_CAPS;
		bad               = desc;
		bad.period_frames = desc.period_frames * 2;
		CHECK_RESULT(craudio_stream_probe(&bad, &suggested), CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE);
		CHECK(suggested.period_frames == desc.period_frames);
		CHECK(suggested.period_mode == CRAUDIO_PERIOD_MODE_FIXED);
		CHECK_RESULT(craudio_device_get_timing_caps(device, &desc.format, desc.flags, &after), CR_SUCCESS);
		CHECK(after.default_period_frames == desc.period_frames);
	}
	if (g_api == CRAUDIO_NATIVE_API_COREAUDIO) {
		/* The rate the device has is granted without a change. */
		CHECK_RESULT(craudio_device_set_sample_rate(device, config.format.sample_rate), CR_SUCCESS);
	} else {
		/* WASAPI and ALSA have no device-wide rate; RemoteIO's is the
		 * session's, which is the caller's. */
		CHECK_RESULT(craudio_device_set_sample_rate(device, config.format.sample_rate), CR_RESULT_AUDIO_SAMPLE_RATE_NOT_SETTABLE);
	}

	/* Create checks: the callback is required to create, not to probe. */
	int called              = 0;
	craudio_stream stream = NULL;
	CHECK_RESULT(craudio_stream_create(&desc, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_stream_create(&desc, &stream), CR_RESULT_DESC_INVALID_PARAMETERS);
	CHECK(stream == NULL);
	desc.callback  = silent_callback;
	desc.user_data = &called;
	CHECK_RESULT(craudio_stream_create(&desc, &stream), CR_SUCCESS);
	if (!stream)
		return;

	CRAUDIO_STREAM_CONFIG granted;
	memset(&granted, 0, sizeof(granted));
	granted.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
	CHECK_RESULT(craudio_stream_get_config(stream, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_stream_get_config(stream, &granted), CR_SUCCESS);
	/* Creation grants what the probe said it would. */
	CHECK(granted.period_frames == config.period_frames);
	CHECK(granted.buffer_frames == config.buffer_frames);
	CHECK(granted.max_callback_frames >= granted.period_frames);
	CHECK(granted.format.sample_rate == config.format.sample_rate);
	CHECK(granted.format.channel_count == config.format.channel_count);

	uint32_t latency = UINT32_MAX;
	CHECK_RESULT(craudio_stream_get_latency_frames(stream, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	r = craudio_stream_get_latency_frames(stream, &latency);
	CHECK(r == CR_SUCCESS);
	if (g_api == CRAUDIO_NATIVE_API_REMOTEIO)
		CHECK(latency >= granted.period_frames); /* the session's latency + its IO buffer */

	CRAUDIO_STREAM_STATUS status;
	memset(&status, 0xFF, sizeof(status));
	CHECK_RESULT(craudio_stream_get_status(stream, NULL), CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK_RESULT(craudio_stream_get_status(stream, &status), CR_SUCCESS);
	CHECK(status.state == CRAUDIO_STREAM_STATE_STOPPED);
	CHECK(status.error == CR_SUCCESS);

	/* Stopping a stream that never started is allowed. */
	CHECK_RESULT(craudio_stream_stop(stream), CR_SUCCESS);
	craudio_stream_destroy(stream);
	CHECK(called == 0);

	test_run(&desc, &granted);
	printf("craudio_test: stream of %u Hz, %u channel(s), period %u, buffer %u, created and destroyed stopped\n",
	    (unsigned)granted.format.sample_rate, (unsigned)granted.format.channel_count, (unsigned)granted.period_frames,
	    (unsigned)granted.buffer_frames);
}

int main(void) {
	test_sample_format_sizes();

	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	test_create_instance_checks(&allocator);
	test_null_handles();

	CRAUDIO_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type              = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	desc.allocator              = &allocator;
	desc.native_api             = g_api;
	craudio_instance instance = NULL;
	const CR_RESULT r          = craudio_instance_create(&desc, &instance);
	if (g_api == CRAUDIO_NATIVE_API_NONE) {
		/* A build with no native audio API: the stub refuses. */
		CHECK(r != CR_SUCCESS);
		CHECK(instance == NULL);
	} else {
		CHECK_RESULT(r, CR_SUCCESS);
	}
	if (instance) {
		CHECK(craudio_instance_get_native_api(instance) == g_api);
		craudio_device device = test_enumerate(instance);
		if (device)
			test_stream(device);
		else
			printf("craudio_test: no output device; streams not tested\n");

		CHECK_RESULT(craudio_instance_set_device_event_callback(instance, device_event_callback, NULL), CR_SUCCESS);
		/* Setting it again replaces it; NULL clears it. */
		CHECK_RESULT(craudio_instance_set_device_event_callback(instance, device_event_callback, &tracker), CR_SUCCESS);
		CHECK_RESULT(craudio_instance_set_device_event_callback(instance, NULL, NULL), CR_SUCCESS);
		CHECK_RESULT(craudio_instance_set_device_event_callback(instance, device_event_callback, NULL), CR_SUCCESS);
		/* Destroying the instance clears it. */
		CHECK_RESULT(craudio_instance_destroy(instance), CR_SUCCESS);
	}
	CHECK(crtest_allocator_clean(&tracker));

	if (failures) {
		fprintf(stderr, "craudio_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("craudio_test: all passed\n");
	return 0;
}

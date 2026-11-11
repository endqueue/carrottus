/*
 * craudio_sine — plays a sine tone through Carrottus Audio on a real output
 * device and checks what the stream delivered.
 *
 * The first test of Carrottus Audio against hardware: it exercises enumeration,
 * device properties, timing caps, the probe/suggestion loop, stream
 * creation, the callback, status polling, stop/restart and teardown, in the
 * order a caller would use them. Everything this program picks (device,
 * format, period, thread mechanism) is its own choice as Carrottus Audio's
 * caller, printed so a failure says what was asked for.
 *
 *   craudio_sine --list
 *   craudio_sine [--device N] [--seconds S] [--freq HZ] [--rate HZ]
 *                  [--period FRAMES] [--os-conversion]
 *
 * Exits 0 when every check passes, 1 when one fails, 2 on bad arguments.
 * It needs an output device and someone listening, so it isn't a ctest.
 */

#include <craudio.h>
#include <crperf.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(uint32_t ms) {
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
#endif

#define TWO_PI 6.28318530717958647692
#define AMPLITUDE 0.2 // about -14 dBFS: clearly audible, not startling

// --------------------------------------------------------------------------
// Callback state. Written only by the audio thread while a run is going,
// and read by main only after craudio_stream_stop() returns, which
// guarantees no callback is running or will run — so no atomics needed.
// --------------------------------------------------------------------------

typedef struct run_stats {
	uint64_t calls;
	uint64_t frames;
	uint32_t min_frame_count;
	uint32_t max_frame_count;
	uint64_t over_max_callback_frames; // frame_count > max_callback_frames
	uint64_t discontinuities;
	uint64_t position_jumps; // position mismatch with no DISCONTINUITY flag
	bool first_position_seen;
	uint64_t first_position;
	uint64_t next_position; // expected position of the next callback
	uint64_t host_time_valid_calls;
	// First and last callback carrying a valid host time, for measuring the
	// device's real rate against the host clock.
	bool host_time_seen;
	uint64_t first_ht_ns, first_ht_pos;
	uint64_t last_ht_ns, last_ht_pos;
} run_stats;

typedef struct tone {
	CRAUDIO_STREAM_CONFIG config;
	uint32_t sample_bytes;
	double phase;
	double phase_step;
	run_stats stats;
} tone;

static void write_sample(
    void *dst,
    CRAUDIO_SAMPLE_FORMAT fmt,
    double s) {
	switch (fmt) {
	case CRAUDIO_SAMPLE_FORMAT_F32:
		*(float *)dst = (float)s;
		break;
	case CRAUDIO_SAMPLE_FORMAT_S16:
		*(int16_t *)dst = (int16_t)lrint(s * 32767.0);
		break;
	case CRAUDIO_SAMPLE_FORMAT_S32:
		*(int32_t *)dst = (int32_t)lrint(s * 2147483647.0);
		break;
	case CRAUDIO_SAMPLE_FORMAT_S24_32_MSB:
		*(int32_t *)dst = (int32_t)((uint32_t)lrint(s * 8388607.0) << 8);
		break;
	case CRAUDIO_SAMPLE_FORMAT_S24_32_LSB:
		*(int32_t *)dst = (int32_t)lrint(s * 8388607.0);
		break;
	case CRAUDIO_SAMPLE_FORMAT_S24_PACKED: {
		// Native-endian; every target Carrottus Audio builds for is little-endian.
		int32_t v  = (int32_t)lrint(s * 8388607.0);
		uint8_t *b = (uint8_t *)dst;
		b[0]       = (uint8_t)(v & 0xff);
		b[1]       = (uint8_t)((v >> 8) & 0xff);
		b[2]       = (uint8_t)((v >> 16) & 0xff);
		break;
	}
	default:
		break;
	}
}

// Silent channels: NA carries nothing by definition, and a full-scale tone
// in an LFE channel is a poor idea.
static bool channel_is_silent(CRAUDIO_CHANNEL_POSITION p) {
	return p == CRAUDIO_CHANNEL_POSITION_NA || p == CRAUDIO_CHANNEL_POSITION_LFE || p == CRAUDIO_CHANNEL_POSITION_LFE2 ||
	       p == CRAUDIO_CHANNEL_POSITION_LLFE || p == CRAUDIO_CHANNEL_POSITION_RLFE;
}

static void tone_callback(
    craudio_stream stream,
    void *frames,
    uint32_t frame_count,
    const CRAUDIO_CALLBACK_INFO *info,
    void *user_data) {
	(void)stream;
	tone *t                   = (tone *)user_data;
	run_stats *st             = &t->stats;
	const CRAUDIO_FORMAT *f = &t->config.format;
	const uint32_t channels   = f->channel_count;
	const uint32_t bytes      = t->sample_bytes;

	// Bookkeeping first, so it's recorded even if writing goes wrong.
	if (st->calls == 0 || frame_count < st->min_frame_count)
		st->min_frame_count = frame_count;
	if (frame_count > st->max_frame_count)
		st->max_frame_count = frame_count;
	if (frame_count > t->config.max_callback_frames)
		st->over_max_callback_frames++;
	if (info->flags & CRAUDIO_CALLBACK_FLAG_DISCONTINUITY)
		st->discontinuities++;
	if (!st->first_position_seen) {
		st->first_position_seen = true;
		st->first_position      = info->device_position_frames;
	} else if (info->device_position_frames != st->next_position && !(info->flags & CRAUDIO_CALLBACK_FLAG_DISCONTINUITY)) {
		st->position_jumps++;
	}
	st->next_position = info->device_position_frames + frame_count;
	if (info->flags & CRAUDIO_CALLBACK_FLAG_HOST_TIME_VALID) {
		st->host_time_valid_calls++;
		if (!st->host_time_seen) {
			st->host_time_seen = true;
			st->first_ht_ns    = info->host_time_ns;
			st->first_ht_pos   = info->device_position_frames;
		}
		st->last_ht_ns  = info->host_time_ns;
		st->last_ht_pos = info->device_position_frames;
	}
	st->calls++;
	st->frames += frame_count;

	// Clamp so an over-long callback is recorded above, not a buffer overrun.
	uint32_t n = frame_count;
	if (n > t->config.max_callback_frames)
		n = t->config.max_callback_frames;

	for (uint32_t i = 0; i < n; i++) {
		const double s = AMPLITUDE * sin(t->phase);
		t->phase += t->phase_step;
		if (t->phase >= TWO_PI)
			t->phase -= TWO_PI;
		for (uint32_t c = 0; c < channels; c++) {
			const double v = channel_is_silent(f->positions[c]) ? 0.0 : s;
			void *dst      = f->interleaved ? (uint8_t *)frames + ((size_t)i * channels + c) * bytes : (uint8_t *)((void **)frames)[c] + (size_t)i * bytes;
			write_sample(dst, f->sample_format, v);
		}
	}
}

// --------------------------------------------------------------------------
// Printing
// --------------------------------------------------------------------------

static const char *sample_format_name(CRAUDIO_SAMPLE_FORMAT f) {
	switch (f) {
	case CRAUDIO_SAMPLE_FORMAT_S16:
		return "S16";
	case CRAUDIO_SAMPLE_FORMAT_S24_PACKED:
		return "S24_PACKED";
	case CRAUDIO_SAMPLE_FORMAT_S24_32_MSB:
		return "S24_32_MSB";
	case CRAUDIO_SAMPLE_FORMAT_S24_32_LSB:
		return "S24_32_LSB";
	case CRAUDIO_SAMPLE_FORMAT_S32:
		return "S32";
	case CRAUDIO_SAMPLE_FORMAT_F32:
		return "F32";
	default:
		return "UNKNOWN";
	}
}

static const char *period_mode_name(CRAUDIO_PERIOD_MODE m) {
	switch (m) {
	case CRAUDIO_PERIOD_MODE_FIXED:
		return "FIXED";
	case CRAUDIO_PERIOD_MODE_EXACT:
		return "EXACT";
	case CRAUDIO_PERIOD_MODE_NEGOTIATED:
		return "NEGOTIATED";
	default:
		return "UNKNOWN";
	}
}

static const char *state_name(CRAUDIO_STREAM_STATE s) {
	switch (s) {
	case CRAUDIO_STREAM_STATE_STOPPED:
		return "STOPPED";
	case CRAUDIO_STREAM_STATE_RUNNING:
		return "RUNNING";
	case CRAUDIO_STREAM_STATE_DEVICE_LOST:
		return "DEVICE_LOST";
	case CRAUDIO_STREAM_STATE_ERROR:
		return "ERROR";
	default:
		return "?";
	}
}

static void print_format(const CRAUDIO_FORMAT *f) {
	printf("%s %u Hz %u ch %s", sample_format_name(f->sample_format), f->sample_rate, f->channel_count, f->interleaved ? "interleaved" : "planar");
	if (f->channel_count && f->positions[0] != CRAUDIO_CHANNEL_POSITION_UNSPECIFIED) {
		printf(" [");
		for (uint32_t c = 0; c < f->channel_count && c < CRAUDIO_MAX_CHANNELS; c++)
			printf("%s%u", c ? " " : "", (unsigned)f->positions[c]);
		printf("]");
	} else {
		printf(" (no layout)");
	}
}

static void print_roles(CRAUDIO_DEFAULT_ROLES r) {
	if (!r)
		return;
	printf("  default:");
	if (r & CRAUDIO_DEFAULT_ROLE_GENERAL)
		printf(" general");
	if (r & CRAUDIO_DEFAULT_ROLE_MULTIMEDIA)
		printf(" multimedia");
	if (r & CRAUDIO_DEFAULT_ROLE_COMMUNICATIONS)
		printf(" communications");
	if (r & CRAUDIO_DEFAULT_ROLE_SYSTEM_SOUNDS)
		printf(" system-sounds");
}

static int failures = 0;

static void check(
    bool ok,
    const char *what) {
	printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
	if (!ok)
		failures++;
}

static bool call_ok(
    CR_RESULT r,
    const char *what) {
	if (r == CR_SUCCESS)
		return true;
	printf("  [FAIL] %s: %s (%d)\n", what, cr_result_str(r), (int)r);
	failures++;
	return false;
}

// --------------------------------------------------------------------------
// Runs
// --------------------------------------------------------------------------

// Plays one run of `ms` milliseconds, polling status every 100 ms, and
// checks what the callback saw. Returns false if the stream couldn't run.
static bool play_run(
    craudio_stream stream,
    tone *t,
    uint32_t ms,
    const char *label) {
	memset(&t->stats, 0, sizeof(t->stats));
	printf("\n%s: %u ms\n", label, ms);

	const uint64_t t0 = crperf_get_monotonic_time_ns();
	if (!call_ok(craudio_stream_start(stream), "craudio_stream_start"))
		return false;

	CRAUDIO_STREAM_STATUS before = {0}, status = {0};
	craudio_stream_get_status(stream, &before);
	bool always_running = true;
	for (uint32_t elapsed = 0; elapsed < ms; elapsed += 100) {
		sleep_ms(100);
		if (craudio_stream_get_status(stream, &status) != CR_SUCCESS || status.state != CRAUDIO_STREAM_STATE_RUNNING) {
			always_running = false;
			printf("  stream left RUNNING: %s, error %s\n", state_name(status.state), cr_result_str(status.error));
			break;
		}
	}
	call_ok(craudio_stream_stop(stream), "craudio_stream_stop");
	const uint64_t wall_ns = crperf_get_monotonic_time_ns() - t0;

	CRAUDIO_STREAM_STATUS after = {0};
	craudio_stream_get_status(stream, &after);

	// Safe to read t->stats now: stop returned, so no callback can run.
	const run_stats *st   = &t->stats;
	const double rate     = (double)t->config.format.sample_rate;
	const double wall_s   = (double)wall_ns / 1e9;
	const double expected = rate * wall_s;

	printf(
	    "  callbacks %llu, frames %llu (%.2f s of audio in %.2f s wall)\n", (unsigned long long)st->calls, (unsigned long long)st->frames,
	    (double)st->frames / rate, wall_s);
	printf("  frame_count min %u max %u (max_callback_frames %u)\n", st->min_frame_count, st->max_frame_count, t->config.max_callback_frames);
	printf(
	    "  first position %llu, discontinuities %llu, underflows %llu, host time valid on %llu callbacks\n", (unsigned long long)st->first_position,
	    (unsigned long long)st->discontinuities, (unsigned long long)(after.underflow_count - before.underflow_count),
	    (unsigned long long)st->host_time_valid_calls);

	check(always_running, "stream stayed RUNNING while playing");
	check(after.state == CRAUDIO_STREAM_STATE_STOPPED, "stream is STOPPED after stop");
	check(st->calls > 0, "callback ran");
	check(st->over_max_callback_frames == 0, "frame_count never exceeded max_callback_frames");
	check(st->min_frame_count > 0, "no zero-length callbacks");
	check(st->first_position_seen && st->first_position == 0, "device position starts at 0 on start");
	check(st->position_jumps == 0, "device position continuous (jumps only with DISCONTINUITY)");

	// Wall time includes start/stop latency and any prefill, so allow
	// slack of 25% plus a few periods either way.
	const double slack = expected * 0.25 + 4.0 * (double)t->config.max_callback_frames;
	check(fabs((double)st->frames - expected) <= slack, "frames delivered match wall time (within 25%)");

	if (st->host_time_seen && st->last_ht_ns > st->first_ht_ns + 200000000ull) {
		const double measured = (double)(st->last_ht_pos - st->first_ht_pos) / ((double)(st->last_ht_ns - st->first_ht_ns) / 1e9);
		printf("  device rate measured against host clock: %.1f Hz (nominal %.0f)\n", measured, rate);
		check(fabs(measured - rate) <= rate * 0.01, "host time agrees with sample rate (within 1%)");
	} else {
		printf("  (too few host timestamps to measure the device rate)\n");
	}
	return true;
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------

static void usage(void) {
	fprintf(
	    stderr, "usage: craudio_sine --list\n"
	            "       craudio_sine [--device N] [--seconds S] [--freq HZ] [--rate HZ]\n"
	            "                      [--period FRAMES] [--os-conversion]\n");
}

int main(
    int argc,
    char **argv) {
	bool list_only = false, os_conversion = false;
	int device_index = -1;
	double seconds = 3.0, freq = 440.0;
	uint32_t want_rate = 0, want_period = 0;

	for (int i = 1; i < argc; i++) {
		const char *a        = argv[i];
		const bool has_value = i + 1 < argc;
		if (!strcmp(a, "--list"))
			list_only = true;
		else if (!strcmp(a, "--os-conversion"))
			os_conversion = true;
		else if (!strcmp(a, "--device") && has_value)
			device_index = atoi(argv[++i]);
		else if (!strcmp(a, "--seconds") && has_value)
			seconds = atof(argv[++i]);
		else if (!strcmp(a, "--freq") && has_value)
			freq = atof(argv[++i]);
		else if (!strcmp(a, "--rate") && has_value)
			want_rate = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(a, "--period") && has_value)
			want_period = (uint32_t)atoi(argv[++i]);
		else {
			usage();
			return 2;
		}
	}
	if (seconds <= 0.0 || seconds > 600.0 || freq <= 0.0) {
		usage();
		return 2;
	}

	// The one native API this build compiled in. A build with several would
	// make this a command-line choice.
#if CRAUDIO_COMPILE_COREAUDIO
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_COREAUDIO;
	const char *api_name           = "CoreAudio";
#elif CRAUDIO_COMPILE_WASAPI
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_WASAPI;
	const char *api_name           = "WASAPI";
#elif CRAUDIO_COMPILE_ALSA
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_ALSA;
	const char *api_name           = "ALSA";
#else
	// No backend compiled in: the stub should refuse, which is still checked.
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_NONE;
	const char *api_name           = "none (stub)";
#endif

	printf("Carrottus Audio %s\n", api_name);
	// The system heap: this test listens for a tone, it doesn't count blocks.
	const CR_ALLOCATOR allocator        = cr_allocator_system();
	CRAUDIO_INSTANCE_DESC instance_desc = {0};
	instance_desc.base.type               = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	instance_desc.allocator               = &allocator;
	instance_desc.native_api              = api;
	craudio_instance instance           = NULL;
	if (!call_ok(craudio_instance_create(&instance_desc, &instance), "craudio_instance_create"))
		return 1;
	check(craudio_instance_get_native_api(instance) == api, "instance reports its native API");

	craudio_device *devices = NULL;
	uint32_t device_count     = 0;
	if (!call_ok(craudio_device_enumerate(instance, CRAUDIO_DIRECTION_OUTPUT, &devices, &device_count), "craudio_device_enumerate")) {
		craudio_instance_destroy(instance);
		return 1;
	}

	printf("\n%u output device(s):\n", device_count);
	int general_default = -1;
	CRAUDIO_DEVICE_PROPERTIES props = {.base = {CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES, NULL}};
	for (uint32_t i = 0; i < device_count; i++) {
		if (craudio_device_get_properties(devices[i], &props) != CR_SUCCESS) {
			printf("  %u: (properties failed)\n", i);
			continue;
		}
		printf("  %u: %s", i, props.name);
		print_roles(props.default_roles);
		printf("\n     native: ");
		print_format(&props.native_format);
		printf("%s\n     id: %s\n", props.supports_exclusive ? ", exclusive capable" : "", props.persistent_id);
		if (general_default < 0 && (props.default_roles & CRAUDIO_DEFAULT_ROLE_GENERAL))
			general_default = (int)i;
	}
	if (list_only || device_count == 0) {
		craudio_instance_destroy(instance);
		if (device_count == 0) {
			printf("no output devices\n");
			return 1;
		}
		return failures ? 1 : 0;
	}

	// This program's choice, not Carrottus Audio's: the general default unless
	// told otherwise, else the first device.
	if (device_index < 0)
		device_index = general_default >= 0 ? general_default : 0;
	if ((uint32_t)device_index >= device_count) {
		fprintf(stderr, "device %d out of range\n", device_index);
		craudio_instance_destroy(instance);
		return 2;
	}
	craudio_device device = devices[device_index];
	call_ok(craudio_device_get_properties(device, &props), "craudio_device_get_properties");
	printf("\nusing device %d: %s\n", device_index, props.name);

	// Starting format: the device's own, where it has one; ALSA reports none,
	// so ask for float stereo and let the probe suggest otherwise.
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
	if (want_rate)
		desc.format.sample_rate = want_rate;
	desc.flags = os_conversion ? CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION : CRAUDIO_STREAM_FLAG_NONE;
#if CRAUDIO_COMPILE_COREAUDIO
	desc.thread.priority = CRAUDIO_THREAD_PRIORITY_PLATFORM;
#else
	desc.thread.priority = CRAUDIO_THREAD_PRIORITY_NORMAL;
#endif
	desc.callback = tone_callback;

	// Period and buffer from the timing caps where the backend has them,
	// else 10 ms and whatever the probe suggests.
	CRAUDIO_TIMING_CAPS caps = {.base = {CR_STRUCT_TYPE_AUDIO_TIMING_CAPS, NULL}};
	CR_RESULT r = craudio_device_get_timing_caps(device, &desc.format, desc.flags, &caps);
	if (r == CR_SUCCESS) {
		printf(
		    "timing caps: %s, period %u..%u step %u default %u, buffer %u..%u%s\n", period_mode_name(caps.period_mode), caps.min_period_frames,
		    caps.max_period_frames, caps.period_granularity_frames, caps.default_period_frames, caps.min_buffer_frames, caps.max_buffer_frames,
		    caps.variable_callback_frames ? ", variable callbacks" : "");
		desc.period_frames = caps.default_period_frames ? caps.default_period_frames : caps.min_period_frames;
		if (caps.max_buffer_frames) {
			desc.buffer_frames = desc.period_frames * 2;
			if (desc.buffer_frames < caps.min_buffer_frames)
				desc.buffer_frames = caps.min_buffer_frames;
			if (desc.buffer_frames > caps.max_buffer_frames)
				desc.buffer_frames = caps.max_buffer_frames;
		}
	} else {
		printf("timing caps: %s - using 10 ms and the probe's suggestions\n", cr_result_str(r));
		desc.period_frames = desc.format.sample_rate / 100;
		desc.buffer_frames = api == CRAUDIO_NATIVE_API_ALSA ? desc.period_frames * 2 : 0;
	}
	if (want_period)
		desc.period_frames = want_period;

	// Probe, taking the suggestion for whichever field was rejected, until
	// it's accepted. A few rounds is plenty; more means the backend's
	// suggestions don't converge, which is itself a bug worth seeing.
	CRAUDIO_STREAM_CONFIG config = {.base = {CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG, NULL}};
	bool accepted = false;
	for (int round = 1; round <= 8 && !accepted; round++) {
		printf("probe %d: ", round);
		print_format(&desc.format);
		printf(", period %u, buffer %u -> ", desc.period_frames, desc.buffer_frames);
		memset(&config, 0, sizeof(config));
		config.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		r = craudio_stream_probe(&desc, &config);
		printf("%s\n", cr_result_str(r));
		if (r == CR_SUCCESS) {
			accepted = true;
		} else if (r == CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED && config.format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN) {
			desc.format = config.format;
		} else if (r == CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE && config.period_frames) {
			desc.period_frames = config.period_frames;
		} else if (r == CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE) {
			desc.buffer_frames = config.buffer_frames;
		} else {
			break; // no suggestion to follow
		}
	}
	check(accepted, "probe accepted a configuration");
	if (!accepted) {
		craudio_instance_destroy(instance);
		return 1;
	}

	tone t;
	memset(&t, 0, sizeof(t));
	desc.user_data          = &t;
	craudio_stream stream = NULL;
	if (!call_ok(craudio_stream_create(&desc, &stream), "craudio_stream_create")) {
		craudio_instance_destroy(instance);
		return 1;
	}
	t.config.base = (CR_STRUCT_BASE_OUT){CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG, NULL};
	call_ok(craudio_stream_get_config(stream, &t.config), "craudio_stream_get_config");
	t.sample_bytes = craudio_sample_format_get_byte_size(t.config.format.sample_format);
	t.phase_step   = TWO_PI * freq / (double)t.config.format.sample_rate;

	printf("\nstream: ");
	print_format(&t.config.format);
	printf(
	    "\n  period %u (%s), buffer %u, max_callback_frames %u\n", t.config.period_frames, period_mode_name(t.config.period_mode), t.config.buffer_frames,
	    t.config.max_callback_frames);
	check(
	    memcmp(&t.config.format, &config.format, sizeof(config.format)) == 0 && t.config.period_frames == config.period_frames &&
	        t.config.buffer_frames == config.buffer_frames,
	    "stream granted exactly what the probe accepted");
	check(t.sample_bytes != 0, "granted sample format has a byte size");
	check(t.config.max_callback_frames >= t.config.period_frames, "max_callback_frames >= period");

	uint32_t latency = 0;
	if (call_ok(craudio_stream_get_latency_frames(stream, &latency), "craudio_stream_get_latency_frames"))
		printf("  latency %u frames (%.1f ms)\n", latency, 1000.0 * latency / t.config.format.sample_rate);

	CRAUDIO_STREAM_STATUS status;
	call_ok(craudio_stream_get_status(stream, &status), "craudio_stream_get_status");
	check(status.state == CRAUDIO_STREAM_STATE_STOPPED, "new stream is STOPPED");

	printf("\nplaying %.0f Hz - you should hear a steady tone, then a short second one\n", freq);
	if (play_run(stream, &t, (uint32_t)(seconds * 1000.0), "run 1")) {
		// Stopping and starting again must work, with the position reset.
		sleep_ms(300);
		play_run(stream, &t, 1000, "run 2 (restart)");
	}

	craudio_stream_destroy(stream);
	call_ok(craudio_instance_destroy(instance), "craudio_instance_destroy");

	printf("\n%s: %d check(s) failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}

#if CRAUDIO_COMPILE_ALSA

#include "../../craudiointernal.h"

#include <alsa/asoundlib.h>
#include <libudev.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "../../../crallocator.h"
#include "../../../crstructbase.h"

#if __cplusplus
extern "C" {
#endif

/*
 * ALSA backend. PipeWire will be a second backend in this same instance
 * model; until then CRAUDIO_NATIVE_API_PIPEWIRE reports
 * CR_RESULT_NOT_IMPLEMENTED_YET.
 *
 * ALSA has no exclusive/shared flag: exclusivity is which PCM name the
 * caller picks (hw:... is exclusive, dmix/pipewire/default are shared),
 * and the enumerated names already expose that choice. A request with
 * CRAUDIO_STREAM_FLAG_EXCLUSIVE is therefore refused rather than
 * silently ignored.
 */

struct craudio_device_t {
	cr_object_head head; // allocator: a copy of its instance's
	CRAUDIO_DIRECTION direction;
	char persistent_id[256]; // the ALSA PCM name, e.g. "hw:CARD=PCH,DEV=0"
	char name[128];          // DESC hint, lines joined; else the PCM name
	struct craudio_device_t *next; // instance-owned list
};

/*
 * A created stream: an open PCM with hw_params/sw_params installed and
 * prepared, not started. Status fields are written by the audio thread
 * (once it exists) and read from any thread, hence C11 atomics.
 */
struct craudio_stream_t {
	cr_object_head head; // allocator: a copy of its device's
	snd_pcm_t *pcm;
	// The PCM's channel map before this stream applied its layout (NULL if
	// it had none / no layout requested) — restored on destroy, since some
	// drivers keep the map as a persistent control. ALSA's memory
	// (snd_pcm_get_chmap), freed with free().
	snd_pcm_chmap_t *original_chmap;
	CRAUDIO_DIRECTION direction;
	CRAUDIO_STREAM_CONFIG config;
	CRAUDIO_STREAM_CALLBACK callback;
	void *user_data;
	uint32_t sample_bytes;
	uint32_t frame_bytes;
	uint8_t *scratch; // one period; interleaved, or one plane per channel
	void **planes;    // planar: channel_count pointers into scratch
	uint32_t scratch_frames; // frames in scratch (frame_bytes each), which it is freed with
	uint32_t planes_count;   // pointers in planes

	CRAUDIO_THREAD_PRIORITY priority;
	int32_t sched_fifo_priority;

	// Audio thread. Only start/stop/destroy touch these (the caller
	// serializes those), except startup_result, written by the thread
	// before it posts `startup`.
	pthread_t thread;
	bool has_thread;
	// Set by the audio thread itself before its first callback (the
	// creator's copy of `thread` can land after the thread has already
	// called back), cleared after join.
	pthread_t audio_thread_self;
	atomic_bool audio_thread_known;
	int stop_fd; // eventfd; written by stop, polled by the thread
	// The thread's poll set: the stop eventfd, then the PCM's own
	// descriptors. Made by start on the caller's thread and freed after the
	// join, so the audio thread never calls the allocator. NULL while no
	// thread runs, and when the PCM has no descriptors.
	struct pollfd *fds;
	uint32_t pcm_fd_count; // fds holds pcm_fd_count + 1
	sem_t startup;
	bool startup_initialized;
	CR_RESULT startup_result;

	// Audio-thread-only state.
	uint64_t frames_transferred; // stream index of the next frame
	bool pending_discontinuity;  // flag the next callback

	atomic_uint state; // CRAUDIO_STREAM_STATE
	atomic_int error;  // CR_RESULT
	atomic_uint_least64_t underflow_count;
	atomic_uint_least64_t overflow_count;
};

// One endpoint as the device-change watch last saw it.
struct alsa_endpoint {
	CRAUDIO_DIRECTION direction;
	char persistent_id[256]; // the ALSA PCM name
};

/*
 * Device-change notification for one instance: exists while a callback is
 * set. ALSA itself reports nothing when a card comes or goes, so the watch
 * listens to udev's "sound" events - the "udev" source, sent once udev has
 * finished with the device - on a thread of its own, there being no
 * platform thread to borrow. An event only says that something about a
 * sound device changed: the watch then lists the PCM name hints again and
 * reports the difference from the ones it last saw. That record is the
 * watch's own and, once the thread runs, only the thread's - never the
 * instance's device list, which belongs to the thread that enumerates.
 *
 * No DEFAULT_CHANGED: ALSA's default is the PCM named "default", which
 * doesn't move.
 */
struct alsa_watch {
	// The instance's: the watch and its endpoint lists are its memory.
	CR_ALLOCATOR allocator;
	CRAUDIO_DEVICE_EVENT_CALLBACK callback;
	void *user_data;
	struct udev *udev;
	struct udev_monitor *monitor;
	int stop_fd; // eventfd; written by destroy, polled by the thread
	pthread_t thread;
	bool thread_started;
	struct alsa_endpoint *endpoints;
	uint32_t endpoint_count;
	uint32_t endpoint_capacity; // what `endpoints` was allocated, and is freed, with
};

struct craudio_instance_t {
	cr_object_head head;
	struct craudio_device_t *devices; // every PCM ever enumerated
	craudio_device *enumerated[2];    // latest array per direction
	uint32_t enumerated_capacity[2];    // what each was allocated, and is freed, with
	struct alsa_watch *watch;           // NULL while no device-event callback is set
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static CR_RESULT alsa_result(int err) {
	switch (-err) {
	case 0:
		return CR_SUCCESS;
	case EBUSY:
		return CR_RESULT_AUDIO_EXCLUSIVE_UNAVAILABLE;
	case ENODEV:
	case ENOENT:
		return CR_RESULT_AUDIO_DEVICE_LOST;
	case ENOMEM:
		return CR_RESULT_OUT_OF_MEMORY;
	default:
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
}

/*
 * The un-suffixed SND_PCM_FORMAT_* names are native-endian; packed 24-bit
 * only has explicit-endian names, so it's picked by byte order.
 *
 * S24_32_MSB -> SND_PCM_FORMAT_S32: ALSA has no name for 24 bits high in
 * 32, but those bytes *are* an S32 sample whose low byte is zero, so the
 * device receives exactly the intended signal. Read back, an ALSA S32
 * device reports as S32 — Carrottus Audio can't tell it only uses 24 bits.
 */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ALSA_FORMAT_S24_PACKED SND_PCM_FORMAT_S24_3LE
#else
#define ALSA_FORMAT_S24_PACKED SND_PCM_FORMAT_S24_3BE
#endif

static snd_pcm_format_t alsa_format(CRAUDIO_SAMPLE_FORMAT f) {
	switch (f) {
	case CRAUDIO_SAMPLE_FORMAT_S16:
		return SND_PCM_FORMAT_S16;
	case CRAUDIO_SAMPLE_FORMAT_S24_PACKED:
		return ALSA_FORMAT_S24_PACKED;
	case CRAUDIO_SAMPLE_FORMAT_S24_32_MSB:
		return SND_PCM_FORMAT_S32;
	case CRAUDIO_SAMPLE_FORMAT_S24_32_LSB:
		return SND_PCM_FORMAT_S24;
	case CRAUDIO_SAMPLE_FORMAT_S32:
		return SND_PCM_FORMAT_S32;
	case CRAUDIO_SAMPLE_FORMAT_F32:
		return SND_PCM_FORMAT_FLOAT;
	default:
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static CRAUDIO_SAMPLE_FORMAT alsa_format_back(snd_pcm_format_t f) {
	if (f == SND_PCM_FORMAT_S16)
		return CRAUDIO_SAMPLE_FORMAT_S16;
	if (f == ALSA_FORMAT_S24_PACKED)
		return CRAUDIO_SAMPLE_FORMAT_S24_PACKED;
	if (f == SND_PCM_FORMAT_S24)
		return CRAUDIO_SAMPLE_FORMAT_S24_32_LSB;
	if (f == SND_PCM_FORMAT_S32)
		return CRAUDIO_SAMPLE_FORMAT_S32;
	if (f == SND_PCM_FORMAT_FLOAT)
		return CRAUDIO_SAMPLE_FORMAT_F32;
	return CRAUDIO_SAMPLE_FORMAT_UNKNOWN;
}

/*
 * Channel positions <-> snd_pcm_chmap_t. Carrottus Audio's labels are ALSA's
 * full set plus LFE2, so every ALSA label maps both ways; LFE2 has no ALSA
 * label and makes a layout unsupported. AUX is SND_CHMAP_UNKNOWN (read
 * back as AUX0, AUX1, ...) and NA is SND_CHMAP_NA.
 */
static const unsigned int alsa_chmap_of_position[CRAUDIO_CHANNEL_POSITION_LAST_SPEAKER + 1] = {
    [CRAUDIO_CHANNEL_POSITION_MONO] = SND_CHMAP_MONO,
    [CRAUDIO_CHANNEL_POSITION_FL]   = SND_CHMAP_FL,
    [CRAUDIO_CHANNEL_POSITION_FR]   = SND_CHMAP_FR,
    [CRAUDIO_CHANNEL_POSITION_FC]   = SND_CHMAP_FC,
    [CRAUDIO_CHANNEL_POSITION_LFE]  = SND_CHMAP_LFE,
    [CRAUDIO_CHANNEL_POSITION_RL]   = SND_CHMAP_RL,
    [CRAUDIO_CHANNEL_POSITION_RR]   = SND_CHMAP_RR,
    [CRAUDIO_CHANNEL_POSITION_FLC]  = SND_CHMAP_FLC,
    [CRAUDIO_CHANNEL_POSITION_FRC]  = SND_CHMAP_FRC,
    [CRAUDIO_CHANNEL_POSITION_RC]   = SND_CHMAP_RC,
    [CRAUDIO_CHANNEL_POSITION_SL]   = SND_CHMAP_SL,
    [CRAUDIO_CHANNEL_POSITION_SR]   = SND_CHMAP_SR,
    [CRAUDIO_CHANNEL_POSITION_TC]   = SND_CHMAP_TC,
    [CRAUDIO_CHANNEL_POSITION_TFL]  = SND_CHMAP_TFL,
    [CRAUDIO_CHANNEL_POSITION_TFC]  = SND_CHMAP_TFC,
    [CRAUDIO_CHANNEL_POSITION_TFR]  = SND_CHMAP_TFR,
    [CRAUDIO_CHANNEL_POSITION_TRL]  = SND_CHMAP_TRL,
    [CRAUDIO_CHANNEL_POSITION_TRC]  = SND_CHMAP_TRC,
    [CRAUDIO_CHANNEL_POSITION_TRR]  = SND_CHMAP_TRR,
    [CRAUDIO_CHANNEL_POSITION_RLC]  = SND_CHMAP_RLC,
    [CRAUDIO_CHANNEL_POSITION_RRC]  = SND_CHMAP_RRC,
    [CRAUDIO_CHANNEL_POSITION_FLW]  = SND_CHMAP_FLW,
    [CRAUDIO_CHANNEL_POSITION_FRW]  = SND_CHMAP_FRW,
    // LFE2: no ALSA label — left SND_CHMAP_UNKNOWN (0), i.e. unrepresentable.
    [CRAUDIO_CHANNEL_POSITION_FLH]  = SND_CHMAP_FLH,
    [CRAUDIO_CHANNEL_POSITION_FCH]  = SND_CHMAP_FCH,
    [CRAUDIO_CHANNEL_POSITION_FRH]  = SND_CHMAP_FRH,
    [CRAUDIO_CHANNEL_POSITION_TFLC] = SND_CHMAP_TFLC,
    [CRAUDIO_CHANNEL_POSITION_TFRC] = SND_CHMAP_TFRC,
    [CRAUDIO_CHANNEL_POSITION_TSL]  = SND_CHMAP_TSL,
    [CRAUDIO_CHANNEL_POSITION_TSR]  = SND_CHMAP_TSR,
    [CRAUDIO_CHANNEL_POSITION_LLFE] = SND_CHMAP_LLFE,
    [CRAUDIO_CHANNEL_POSITION_RLFE] = SND_CHMAP_RLFE,
    [CRAUDIO_CHANNEL_POSITION_BC]   = SND_CHMAP_BC,
    [CRAUDIO_CHANNEL_POSITION_BLC]  = SND_CHMAP_BLC,
    [CRAUDIO_CHANNEL_POSITION_BRC]  = SND_CHMAP_BRC,
};

// false when ALSA has no label for `p` (LFE2).
static bool alsa_chmap_pos(CRAUDIO_CHANNEL_POSITION p, unsigned int *out) {
	if (p == CRAUDIO_CHANNEL_POSITION_NA)
		*out = SND_CHMAP_NA;
	else if (p >= CRAUDIO_CHANNEL_POSITION_AUX0)
		*out = SND_CHMAP_UNKNOWN;
	else if (p <= CRAUDIO_CHANNEL_POSITION_LAST_SPEAKER && alsa_chmap_of_position[p] != SND_CHMAP_UNKNOWN)
		*out = alsa_chmap_of_position[p];
	else
		return false;
	return true;
}

static void alsa_chmap_to_positions(const snd_pcm_chmap_t *map, CRAUDIO_FORMAT *out) {
	memset(out->positions, 0, sizeof(out->positions));
	uint32_t aux = 0;
	for (unsigned int i = 0; i < map->channels && i < CRAUDIO_MAX_CHANNELS; ++i) {
		unsigned int pos               = map->pos[i] & SND_CHMAP_POSITION_MASK;
		CRAUDIO_CHANNEL_POSITION got = CRAUDIO_CHANNEL_POSITION_UNSPECIFIED;
		// Driver-specific numbering (SND_CHMAP_DRIVER_SPEC) isn't an ALSA
		// label, so it stays unlabeled like UNKNOWN.
		const bool labeled = !(map->pos[i] & SND_CHMAP_DRIVER_SPEC);
		if (labeled && pos == SND_CHMAP_NA)
			got = CRAUDIO_CHANNEL_POSITION_NA;
		else if (labeled && pos != SND_CHMAP_UNKNOWN)
			for (int p = CRAUDIO_CHANNEL_POSITION_MONO; p <= CRAUDIO_CHANNEL_POSITION_LAST_SPEAKER && !got; ++p)
				if (alsa_chmap_of_position[p] == pos)
					got = (CRAUDIO_CHANNEL_POSITION)p;
		out->positions[i] = got ? got : (CRAUDIO_CHANNEL_POSITION)(CRAUDIO_CHANNEL_POSITION_AUX0 + aux++);
	}
}

static bool alsa_chmap_pos_equal(unsigned int have, unsigned int want) {
	if (have & SND_CHMAP_DRIVER_SPEC)
		return want == SND_CHMAP_UNKNOWN;
	return (have & SND_CHMAP_POSITION_MASK) == want;
}

static bool alsa_chmap_equal_in_order(const snd_pcm_chmap_t *map, const unsigned int *want, unsigned int n) {
	if (map->channels != n)
		return false;
	for (unsigned int i = 0; i < n; ++i)
		if (!alsa_chmap_pos_equal(map->pos[i], want[i]))
			return false;
	return true;
}

// FIXED maps must match in order; VAR/PAIRED maps can be reordered, so a
// same-set match counts here and snd_pcm_set_chmap at install decides
// whether this particular order is reachable.
static bool alsa_chmap_query_matches(const snd_pcm_chmap_query_t *q, const unsigned int *want, unsigned int n) {
	if (q->map.channels != n)
		return false;
	if (q->type == SND_CHMAP_TYPE_FIXED || q->type == SND_CHMAP_TYPE_NONE)
		return alsa_chmap_equal_in_order(&q->map, want, n);
	bool used[CRAUDIO_MAX_CHANNELS] = {false};
	for (unsigned int i = 0; i < n; ++i) {
		bool found = false;
		for (unsigned int j = 0; j < n && !found; ++j)
			if (!used[j] && alsa_chmap_pos_equal(q->map.pos[j], want[i]))
				used[j] = found = true;
		if (!found)
			return false;
	}
	return true;
}

// --------------------------------------------------------------------------
// Device-change notification
// --------------------------------------------------------------------------

// Every endpoint there is now, in a block of the watch's allocator with room
// for `*out_capacity`: the same hints, the same test and the same identity
// (PCM name + direction) as craudio_device_enumerate, for both
// directions. A PCM with no IOID does both and is two endpoints.
static CR_RESULT alsa_list_endpoints(
    const CR_ALLOCATOR *allocator,
    struct alsa_endpoint **out_endpoints,
    uint32_t *out_count,
    uint32_t *out_capacity) {
	static const struct {
		CRAUDIO_DIRECTION direction;
		const char *ioid;
	} directions[] = {
	    {CRAUDIO_DIRECTION_OUTPUT, "Output"},
	    {CRAUDIO_DIRECTION_INPUT, "Input"},
	};
	*out_endpoints = NULL;
	*out_count     = 0;
	*out_capacity  = 0;

	void **hints = NULL;
	int err      = snd_device_name_hint(-1, "pcm", &hints);
	if (err < 0)
		return alsa_result(err);
	uint32_t capacity = 0;
	for (void **h = hints; *h; ++h)
		capacity += 2;
	if (capacity == 0) {
		snd_device_name_free_hint(hints);
		return CR_SUCCESS;
	}
	struct alsa_endpoint *list = CR_NEW_ARRAY_ZEROED(allocator, struct alsa_endpoint, capacity);
	if (!list) {
		snd_device_name_free_hint(hints);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	uint32_t written = 0;
	for (void **h = hints; *h; ++h) {
		char *name = snd_device_name_get_hint(*h, "NAME");
		char *ioid = snd_device_name_get_hint(*h, "IOID");
		for (size_t d = 0; name && d < sizeof(directions) / sizeof(directions[0]); ++d) {
			if (ioid && strcmp(ioid, directions[d].ioid) != 0)
				continue;
			list[written].direction = directions[d].direction;
			strncpy(list[written].persistent_id, name, sizeof(list[written].persistent_id) - 1);
			++written;
		}
		// ALSA's strings, which it says to free with free().
		free(name);
		free(ioid);
	}
	snd_device_name_free_hint(hints);
	*out_endpoints = list;
	*out_count     = written;
	*out_capacity  = capacity;
	return CR_SUCCESS;
}

static bool alsa_endpoint_in(const struct alsa_endpoint *endpoint, const struct alsa_endpoint *list, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i)
		if (list[i].direction == endpoint->direction && strcmp(list[i].persistent_id, endpoint->persistent_id) == 0)
			return true;
	return false;
}

// Runs on the watch's thread: udev reported a sound device. Reports what
// left and then what arrived since the last record, and keeps the new one.
// If the endpoints can't be listed the old record stays, and the next change
// is compared against it.
static void alsa_watch_devices_changed(struct alsa_watch *w) {
	struct alsa_endpoint *now = NULL;
	uint32_t now_count        = 0;
	uint32_t now_capacity     = 0;
	if (CR_FAILED(alsa_list_endpoints(&w->allocator, &now, &now_count, &now_capacity)))
		return;
	struct alsa_endpoint *before   = w->endpoints;
	uint32_t before_count          = w->endpoint_count;
	const uint32_t before_capacity = w->endpoint_capacity;
	w->endpoints                   = now;
	w->endpoint_count              = now_count;
	w->endpoint_capacity           = now_capacity;

	for (uint32_t i = 0; i < before_count; ++i)
		if (!alsa_endpoint_in(&before[i], now, now_count))
			w->callback(CRAUDIO_DEVICE_EVENT_REMOVED, before[i].persistent_id, before[i].direction, CRAUDIO_DEFAULT_ROLE_NONE, w->user_data);
	for (uint32_t i = 0; i < now_count; ++i)
		if (!alsa_endpoint_in(&now[i], before, before_count))
			w->callback(CRAUDIO_DEVICE_EVENT_ADDED, now[i].persistent_id, now[i].direction, CRAUDIO_DEFAULT_ROLE_NONE, w->user_data);
	CR_DELETE_ARRAY(&w->allocator, struct alsa_endpoint, before, before_capacity);
}

/*
 * The watch's thread: blocks until udev has something or destroy wants it
 * gone. One plugged card is several udev events (the card, its control
 * device, each PCM device), so everything waiting is taken before the
 * endpoints are listed once; events that come later list them again, and a
 * listing that finds nothing new reports nothing.
 */
static void *alsa_watch_thread(void *arg) {
	struct alsa_watch *w = (struct alsa_watch *)arg;
	struct pollfd fds[2] = {
	    {.fd = w->stop_fd, .events = POLLIN},
	    {.fd = udev_monitor_get_fd(w->monitor), .events = POLLIN},
	};
	for (;;) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[0].revents)
			break;
		if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
			break; // the monitor is gone: nothing more will come
		if (!(fds[1].revents & POLLIN))
			continue;
		// The monitor's socket is non-blocking: NULL once it is empty.
		bool changed = false;
		struct udev_device *device;
		while ((device = udev_monitor_receive_device(w->monitor))) {
			changed = true;
			udev_device_unref(device);
		}
		if (changed)
			alsa_watch_devices_changed(w);
	}
	return NULL;
}

// Returns once the thread has ended, so the callback is not running and
// will not run again. NULL is a no-op.
static void alsa_watch_destroy(struct alsa_watch *w) {
	if (!w)
		return;
	if (w->thread_started) {
		const uint64_t one = 1;
		ssize_t written    = write(w->stop_fd, &one, sizeof(one));
		(void)written; // an eventfd write only fails at counter overflow
		pthread_join(w->thread, NULL);
	}
	if (w->monitor)
		udev_monitor_unref(w->monitor);
	if (w->udev)
		udev_unref(w->udev);
	if (w->stop_fd >= 0)
		close(w->stop_fd);
	const CR_ALLOCATOR allocator = w->allocator;
	CR_DELETE_ARRAY(&allocator, struct alsa_endpoint, w->endpoints, w->endpoint_capacity);
	CR_DELETE(&allocator, struct alsa_watch, w);
}

static CR_RESULT alsa_watch_create(
    const CR_ALLOCATOR *allocator,
    CRAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data,
    struct alsa_watch **out_watch) {
	struct alsa_watch *w = CR_NEW_ZEROED(allocator, struct alsa_watch);
	if (!w)
		return CR_RESULT_OUT_OF_MEMORY;
	w->allocator = *allocator;
	w->callback  = callback;
	w->user_data = user_data;
	w->stop_fd   = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	CR_RESULT r = w->stop_fd >= 0 ? CR_SUCCESS : CR_RESULT_API_SPECIFIC_FAILURE;
	if (CR_SUCCESS == r) {
		w->udev = udev_new();
		if (!w->udev)
			r = CR_RESULT_OUT_OF_MEMORY;
	}
	if (CR_SUCCESS == r) {
		w->monitor = udev_monitor_new_from_netlink(w->udev, "udev");
		if (!w->monitor)
			r = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	// Receiving first, baseline second, thread last: a change in between
	// waits in the monitor's socket and is compared against the baseline,
	// instead of falling in a gap before anything was listening.
	if (CR_SUCCESS == r && udev_monitor_filter_add_match_subsystem_devtype(w->monitor, "sound", NULL) < 0)
		r = CR_RESULT_API_SPECIFIC_FAILURE;
	if (CR_SUCCESS == r && udev_monitor_enable_receiving(w->monitor) < 0)
		r = CR_RESULT_API_SPECIFIC_FAILURE;
	if (CR_SUCCESS == r)
		r = alsa_list_endpoints(&w->allocator, &w->endpoints, &w->endpoint_count, &w->endpoint_capacity);
	if (CR_SUCCESS == r) {
		if (pthread_create(&w->thread, NULL, alsa_watch_thread, w) == 0)
			w->thread_started = true;
		else
			r = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	if (CR_FAILED(r)) {
		alsa_watch_destroy(w);
		return r;
	}
	*out_watch = w;
	return CR_SUCCESS;
}

// True on the instance's own device-event callback, where waiting for that
// callback to return would never end. The callback only ever runs on the
// watch's thread.
static bool alsa_in_device_event_callback(craudio_instance instance) {
	return instance->watch && pthread_equal(pthread_self(), instance->watch->thread);
}

// --------------------------------------------------------------------------
// Instance / devices
// --------------------------------------------------------------------------

CR_RESULT craudio_instance_create(
    const CRAUDIO_INSTANCE_DESC *desc,
    craudio_instance *out_instance) {
	const CR_RESULT desc_result = craudio_validate_instance_desc(desc, out_instance);
	if (desc_result != CR_SUCCESS)
		return desc_result;
	if (desc->native_api == CRAUDIO_NATIVE_API_PIPEWIRE)
		return CR_RESULT_NOT_IMPLEMENTED_YET;
	if (desc->native_api != CRAUDIO_NATIVE_API_ALSA)
		return CR_RESULT_INVALID_API;

	struct craudio_instance_t *inst = CR_NEW_ZEROED(desc->allocator, struct craudio_instance_t);
	if (!inst)
		return CR_RESULT_OUT_OF_MEMORY;
	inst->head.allocator = *desc->allocator;
	CR_BIRTH(inst);
	*out_instance        = inst;
	return CR_SUCCESS;
}

CR_RESULT craudio_instance_destroy(craudio_instance instance) {
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_instance_destroy", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (alsa_in_device_event_callback(instance))
		return CR_RESULT_GENERAL_FAILURE;
	alsa_watch_destroy(instance->watch);
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	struct craudio_device_t *dev = instance->devices;
	while (dev) {
		struct craudio_device_t *next = dev->next;
		CR_KILL(dev);
		cr_string_free(&allocator, dev->head.debug_name);
		CR_DELETE(&allocator, struct craudio_device_t, dev);
		dev = next;
	}
	CR_DELETE_ARRAY(&allocator, craudio_device, instance->enumerated[0], instance->enumerated_capacity[0]);
	CR_DELETE_ARRAY(&allocator, craudio_device, instance->enumerated[1], instance->enumerated_capacity[1]);
	cr_string_free(&allocator, instance->head.debug_name);
	CR_DELETE(&allocator, struct craudio_instance_t, instance);
	return CR_SUCCESS;
}

CRAUDIO_NATIVE_API craudio_instance_get_native_api(craudio_instance instance) {
	if (!instance)
		return CRAUDIO_NATIVE_API_NONE;
	if (CR_STALE(instance, "craudio_instance_get_native_api", "instance"))
		return CRAUDIO_NATIVE_API_NONE;
	return CRAUDIO_NATIVE_API_ALSA;
}

// Returns the instance's existing handle for this PCM name, or adds one,
// so handles stay stable across enumerations.
static struct craudio_device_t *alsa_find_or_add_device(
    craudio_instance instance,
    const char *name,
    const char *description,
    CRAUDIO_DIRECTION direction) {
	for (struct craudio_device_t *d = instance->devices; d; d = d->next)
		if (d->direction == direction && strcmp(d->persistent_id, name) == 0)
			return d;

	struct craudio_device_t *d = CR_NEW_ZEROED(&instance->head.allocator, struct craudio_device_t);
	if (!d)
		return NULL;
	d->head.allocator = instance->head.allocator;
	CR_BIRTH(d);
	d->direction      = direction;
	strncpy(d->persistent_id, name, sizeof(d->persistent_id) - 1);
	// DESC is multi-line ("card, device\nwhat it is"); join it into one.
	strncpy(d->name, description ? description : name, sizeof(d->name) - 1);
	for (char *c = d->name; *c; ++c)
		if (*c == '\n')
			*c = ' ';
	d->next           = instance->devices;
	instance->devices = d;
	return d;
}

CR_RESULT craudio_device_enumerate(
    craudio_instance instance,
    CRAUDIO_DIRECTION direction,
    craudio_device **out_devices,
    uint32_t *out_device_count) {
	if (!out_devices || !out_device_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_devices      = NULL;
	*out_device_count = 0;
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_device_enumerate", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (direction != CRAUDIO_DIRECTION_OUTPUT && direction != CRAUDIO_DIRECTION_INPUT)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	void **hints = NULL;
	int err      = snd_device_name_hint(-1, "pcm", &hints);
	if (err < 0)
		return alsa_result(err);

	// IOID is "Output", "Input", or absent for a PCM that does both.
	const char *wanted_ioid = direction == CRAUDIO_DIRECTION_OUTPUT ? "Output" : "Input";

	uint32_t capacity = 0;
	for (void **h = hints; *h; ++h)
		++capacity;
	craudio_device *list = capacity ? CR_NEW_ARRAY_ZEROED(&instance->head.allocator, craudio_device, capacity) : NULL;
	if (capacity && !list) {
		snd_device_name_free_hint(hints);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	uint32_t written = 0;
	CR_RESULT r     = CR_SUCCESS;
	for (void **h = hints; *h; ++h) {
		char *name = snd_device_name_get_hint(*h, "NAME");
		char *desc = snd_device_name_get_hint(*h, "DESC");
		char *ioid = snd_device_name_get_hint(*h, "IOID");
		if (name && (!ioid || strcmp(ioid, wanted_ioid) == 0)) {
			struct craudio_device_t *d = alsa_find_or_add_device(instance, name, desc, direction);
			if (d)
				list[written++] = d;
			else
				r = CR_RESULT_OUT_OF_MEMORY;
		}
		// ALSA's strings, which it says to free with free().
		free(name);
		free(desc);
		free(ioid);
		if (CR_FAILED(r))
			break;
	}
	snd_device_name_free_hint(hints);
	if (CR_FAILED(r)) {
		CR_DELETE_ARRAY(&instance->head.allocator, craudio_device, list, capacity);
		return r;
	}

	CR_DELETE_ARRAY(&instance->head.allocator, craudio_device, instance->enumerated[direction], instance->enumerated_capacity[direction]);
	instance->enumerated[direction]          = list;
	instance->enumerated_capacity[direction] = capacity;
	*out_devices                             = list;
	*out_device_count                        = written;
	return CR_SUCCESS;
}

CR_RESULT craudio_device_get_properties(
    craudio_device device,
    CRAUDIO_DEVICE_PROPERTIES *out_properties) {
	if (!out_properties)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_properties->base, CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	memset(out_properties, 0, sizeof(*out_properties));
	out_properties->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_get_properties", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	memcpy(out_properties->name, device->name, sizeof(out_properties->name));
	memcpy(out_properties->persistent_id, device->persistent_id, sizeof(out_properties->persistent_id));
	out_properties->direction     = device->direction;
	// ALSA's only notion of a default is the PCM named "default".
	out_properties->default_roles = strcmp(device->persistent_id, "default") == 0 ? CRAUDIO_DEFAULT_ROLE_GENERAL : CRAUDIO_DEFAULT_ROLE_NONE;
	// native_format stays zeroed: an ALSA PCM has no single native format,
	// only the hw_params space the probe explores. No exclusive flag
	// either — see the file comment.
	out_properties->supports_exclusive = false;
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// Timing / probe
// --------------------------------------------------------------------------

CR_RESULT craudio_device_get_timing_caps(
    craudio_device device,
    const CRAUDIO_FORMAT *format,
    CRAUDIO_STREAM_FLAGS flags,
    CRAUDIO_TIMING_CAPS *out_caps) {
	(void)flags;
	if (!out_caps)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_caps->base, CR_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	memset(out_caps, 0, sizeof(*out_caps));
	out_caps->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (!format)
		return CR_RESULT_NULL_DESC;
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_get_timing_caps", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

// With out_original NULL (probe), a successfully applied map is undone
// before returning. Otherwise (create) it stays applied and the previous
// map is handed back for the stream to restore on destroy.
// `map` is Carrottus Audio's, from `allocator`; the original ALSA hands back is
// ALSA's (snd_pcm_get_chmap), freed with free().
static CR_RESULT alsa_apply_chmap(
    const CR_ALLOCATOR *allocator,
    snd_pcm_t *pcm,
    const unsigned int *want,
    const CRAUDIO_FORMAT *format,
    CRAUDIO_STREAM_CONFIG *out_config,
    snd_pcm_chmap_t **out_original) {
	const unsigned int n = format->channel_count;
	const size_t size    = sizeof(snd_pcm_chmap_t) + n * sizeof(unsigned int);
	snd_pcm_chmap_t *map = (snd_pcm_chmap_t *)cr_allocate(allocator, size, CR_ALIGNOF(snd_pcm_chmap_t));
	if (!map)
		return CR_RESULT_OUT_OF_MEMORY;
	map->channels = n;
	memcpy(map->pos, want, n * sizeof(unsigned int));

	snd_pcm_chmap_t *original = snd_pcm_get_chmap(pcm);
	CR_RESULT r              = CR_SUCCESS;
	if (snd_pcm_set_chmap(pcm, map) < 0) {
		// Refused — but a fixed map that already *is* this layout is fine.
		if (!original || !alsa_chmap_equal_in_order(original, want, n)) {
			if (original)
				alsa_chmap_to_positions(original, &out_config->format);
			else
				memset(out_config->format.positions, 0, sizeof(out_config->format.positions));
			r = CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
		}
	} else if (out_original) {
		*out_original = original; // kept applied; caller restores later
		original      = NULL;
	} else if (original) {
		snd_pcm_set_chmap(pcm, original);
	}
	free(original);
	cr_free(allocator, map, size, CR_ALIGNOF(snd_pcm_chmap_t));
	return r;
}

/*
 * ALSA's hw_params is a constraint space the driver narrows as each
 * parameter is fixed — period/buffer limits depend on format, rate and
 * channels, and drivers can add arbitrary rules (power-of-two periods,
 * byte alignment, ...) that no min/max/step describes. So rather than
 * modelling them, the probe sets each requested value *exactly* in the
 * same order a stream would, and on the first refusal asks ALSA's own
 * solver for its nearest acceptable value (set_*_near on a scratch copy of
 * the space as it stood) — that becomes the suggestion. Finally
 * snd_pcm_hw_params installs the configuration on the real device, which
 * is the step that catches anything the refinement didn't.
 */
static CR_RESULT alsa_configure(
    snd_pcm_t *pcm,
    const CRAUDIO_STREAM_DESC *desc,
    CRAUDIO_STREAM_CONFIG *out_config,
    snd_pcm_chmap_t **out_original_chmap) {
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_t *scratch;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_alloca(&scratch);

	int err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0)
		return alsa_result(err);

	// Resampling only happens inside ALSA plugins (plughw:, default, ...);
	// it's off unless the caller opted into OS conversion. Sample-format
	// conversion is likewise a plugin property — the device name the
	// caller chose decides whether it exists at all.
	snd_pcm_hw_params_set_rate_resample(pcm, hw, (desc->flags & CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION) ? 1 : 0);

	// --- Format: access (interleaved/planar), sample format, channels, rate.
	out_config->format = desc->format;

	snd_pcm_access_t access = desc->format.interleaved ? SND_PCM_ACCESS_RW_INTERLEAVED : SND_PCM_ACCESS_RW_NONINTERLEAVED;
	if (snd_pcm_hw_params_set_access(pcm, hw, access) < 0) {
		snd_pcm_access_t other = desc->format.interleaved ? SND_PCM_ACCESS_RW_NONINTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED;
		if (snd_pcm_hw_params_test_access(pcm, hw, other) == 0)
			out_config->format.interleaved = !desc->format.interleaved;
		else
			memset(&out_config->format, 0, sizeof(out_config->format));
		return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
	}

	snd_pcm_format_t fmt = alsa_format(desc->format.sample_format);
	if (fmt == SND_PCM_FORMAT_UNKNOWN || snd_pcm_hw_params_set_format(pcm, hw, fmt) < 0) {
		// ALSA has no "nearest" sample format; set_format_first is its own
		// pick from what the device supports. UNKNOWN if Carrottus Audio has no
		// equivalent (e.g. U8).
		snd_pcm_format_t first = SND_PCM_FORMAT_UNKNOWN;
		snd_pcm_hw_params_copy(scratch, hw);
		if (snd_pcm_hw_params_set_format_first(pcm, scratch, &first) >= 0)
			out_config->format.sample_format = alsa_format_back(first);
		else
			out_config->format.sample_format = CRAUDIO_SAMPLE_FORMAT_UNKNOWN;
		return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
	}

	if (snd_pcm_hw_params_set_channels(pcm, hw, desc->format.channel_count) < 0) {
		unsigned int near = desc->format.channel_count;
		snd_pcm_hw_params_copy(scratch, hw);
		out_config->format.channel_count = snd_pcm_hw_params_set_channels_near(pcm, scratch, &near) >= 0 ? near : 0;
		return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
	}

	if (snd_pcm_hw_params_set_rate(pcm, hw, desc->format.sample_rate, 0) < 0) {
		unsigned int near = desc->format.sample_rate;
		int dir           = 0;
		snd_pcm_hw_params_copy(scratch, hw);
		out_config->format.sample_rate = snd_pcm_hw_params_set_rate_near(pcm, scratch, &near, &dir) >= 0 ? near : 0;
		return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
	}

	// --- Channel positions, first pass: the maps the PCM advertises. A
	// layout must match one; a PCM that advertises none can't confirm any
	// layout, so only "no layout" is accepted there.
	unsigned int want[CRAUDIO_MAX_CHANNELS];
	const bool has_layout = craudio_format_has_layout(&desc->format);
	if (has_layout) {
		for (uint32_t i = 0; i < desc->format.channel_count; ++i) {
			if (!alsa_chmap_pos(desc->format.positions[i], &want[i])) {
				// A label ALSA can't express: suggest the same format with
				// no layout rather than relabeling.
				memset(out_config->format.positions, 0, sizeof(out_config->format.positions));
				return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
			}
		}
		snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(pcm);
		bool matched                 = false;
		const snd_pcm_chmap_t *first = NULL;
		for (snd_pcm_chmap_query_t **q = maps; q && *q && !matched; ++q) {
			matched = alsa_chmap_query_matches(*q, want, desc->format.channel_count);
			if (!first && (*q)->map.channels == desc->format.channel_count)
				first = &(*q)->map;
		}
		if (!matched) {
			if (first)
				alsa_chmap_to_positions(first, &out_config->format);
			else
				memset(out_config->format.positions, 0, sizeof(out_config->format.positions));
		}
		if (maps)
			snd_pcm_free_chmaps(maps);
		if (!matched)
			return CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED;
	}

	// --- Period.
	out_config->period_mode = CRAUDIO_PERIOD_MODE_EXACT;
	if (snd_pcm_hw_params_set_period_size(pcm, hw, desc->period_frames, 0) < 0) {
		snd_pcm_uframes_t near = desc->period_frames;
		int dir                = 0;
		snd_pcm_hw_params_copy(scratch, hw);
		out_config->period_frames = snd_pcm_hw_params_set_period_size_near(pcm, scratch, &near, &dir) >= 0 ? (uint32_t)near : 0;
		return CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE;
	}
	out_config->period_frames = desc->period_frames;

	// --- Buffer. Required on ALSA (it has a buffer separate from the
	// period), so 0 is refused like any other unacceptable size, and the
	// solver's nearest — its smallest legal buffer, for 0 — is suggested.
	if (desc->buffer_frames == 0 || snd_pcm_hw_params_set_buffer_size(pcm, hw, desc->buffer_frames) < 0) {
		snd_pcm_uframes_t near = desc->buffer_frames;
		snd_pcm_hw_params_copy(scratch, hw);
		out_config->buffer_frames = snd_pcm_hw_params_set_buffer_size_near(pcm, scratch, &near) >= 0 ? (uint32_t)near : 0;
		return CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE;
	}
	out_config->buffer_frames = desc->buffer_frames;

	// --- Install on the real device: the authoritative step.
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0)
		return alsa_result(err);

	snd_pcm_uframes_t granted_period = 0, granted_buffer = 0;
	int dir                          = 0;
	snd_pcm_hw_params_get_period_size(hw, &granted_period, &dir);
	snd_pcm_hw_params_get_buffer_size(hw, &granted_buffer);
	out_config->period_frames = (uint32_t)granted_period;
	out_config->buffer_frames = (uint32_t)granted_buffer;
	// Carrottus Audio's thread hands the callback one period per transfer.
	out_config->max_callback_frames = (uint32_t)granted_period;

	if (granted_period != desc->period_frames)
		return CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE;
	if (granted_buffer != desc->buffer_frames)
		return CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE;

	// --- Channel positions, second pass: actually apply the map, which is
	// the only way to know a VAR/PAIRED map reaches this exact order.
	// Some drivers keep the map as a persistent control (HDMI's "Playback
	// Channel Map"), so the original is restored before closing — a probe
	// must not leave the device changed.
	return has_layout ? alsa_apply_chmap(&desc->device->head.allocator, pcm, want, &desc->format, out_config, out_original_chmap) : CR_SUCCESS;
}

// Carrottus Audio's own thread runs the callback, so NORMAL and SCHED_FIFO
// apply; MMCSS and PLATFORM are other backends' mechanisms.
static CR_RESULT alsa_check_thread(const CRAUDIO_THREAD_DESC *t) {
	switch (t->priority) {
	case CRAUDIO_THREAD_PRIORITY_NORMAL:
		return CR_SUCCESS;
	case CRAUDIO_THREAD_PRIORITY_SCHED_FIFO:
		if (t->sched_fifo_priority < sched_get_priority_min(SCHED_FIFO) || t->sched_fifo_priority > sched_get_priority_max(SCHED_FIFO))
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		return CR_SUCCESS;
	default:
		return CR_RESULT_AUDIO_THREAD_PRIORITY_UNSUPPORTED;
	}
}

/*
 * The one path that turns a desc into an open, hw-configured PCM, used by
 * both probe and create so creation can never accept something the probe
 * would reject (or vice versa). NONBLOCK so a hw: device another process
 * holds reports -EBUSY instead of blocking until it's released; the
 * stream's audio thread will wait on the PCM's poll descriptors rather
 * than in blocking reads/writes, so it stays non-blocking.
 */
static CR_RESULT alsa_open(
    const CRAUDIO_STREAM_DESC *desc,
    CRAUDIO_STREAM_CONFIG *out_config,
    snd_pcm_chmap_t **out_original_chmap,
    snd_pcm_t **out_pcm) {
	*out_pcm = NULL;
	if (desc->flags & CRAUDIO_STREAM_FLAG_EXCLUSIVE)
		return CR_RESULT_AUDIO_EXCLUSIVE_UNAVAILABLE;

	snd_pcm_t *pcm          = NULL;
	snd_pcm_stream_t stream = desc->device->direction == CRAUDIO_DIRECTION_OUTPUT ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
	int err                 = snd_pcm_open(&pcm, desc->device->persistent_id, stream, SND_PCM_NONBLOCK);
	if (err < 0)
		return alsa_result(err);

	// Conversion only exists in plugins. These PCM types never convert, so
	// the flag is refused rather than left a no-op.
	if (desc->flags & CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION) {
		const snd_pcm_type_t type = snd_pcm_type(pcm);
		if (type == SND_PCM_TYPE_HW || type == SND_PCM_TYPE_DMIX || type == SND_PCM_TYPE_DSNOOP || type == SND_PCM_TYPE_DSHARE) {
			snd_pcm_close(pcm);
			return CR_RESULT_AUDIO_OS_CONVERSION_UNAVAILABLE;
		}
	}

	CR_RESULT r = alsa_configure(pcm, desc, out_config, out_original_chmap);
	if (CR_FAILED(r)) {
		snd_pcm_close(pcm);
		return r;
	}
	*out_pcm = pcm;
	return CR_SUCCESS;
}

CR_RESULT craudio_stream_probe(
    const CRAUDIO_STREAM_DESC *desc,
    CRAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_config->base, CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	memset(out_config, 0, sizeof(*out_config));
	out_config->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	CR_RESULT r = craudio_validate_stream_desc(desc);
	if (CR_FAILED(r))
		return r;
	r = alsa_check_thread(&desc->thread);
	if (CR_FAILED(r))
		return r;

	snd_pcm_t *pcm = NULL;
	r              = alsa_open(desc, out_config, NULL, &pcm);
	if (pcm)
		snd_pcm_close(pcm);
	return r;
}

/*
 * sw_params for a created stream:
 * - start_threshold = boundary: the PCM never starts itself; it starts
 *   only when craudio_stream_start says so (after the first period is
 *   written, for output), so start is always the caller's decision.
 * - avail_min = one period: the PCM's poll descriptors wake the audio
 *   thread once per period, matching the callback cadence.
 * - monotonic timestamps: snd_pcm_status' htstamp becomes CLOCK_MONOTONIC,
 *   the clock crperf_get_monotonic_time_ns and
 *   CRAUDIO_CALLBACK_INFO::host_time_ns use. ALSA's default is
 *   gettimeofday, which would not line up.
 * stop_threshold stays at ALSA's default (buffer size), so an underrun or
 * overrun stops the PCM with -EPIPE — reported as a DISCONTINUITY, not
 * hidden.
 */
static CR_RESULT alsa_set_sw_params(snd_pcm_t *pcm, uint32_t period_frames) {
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	int err = snd_pcm_sw_params_current(pcm, sw);
	if (err < 0)
		return alsa_result(err);

	snd_pcm_uframes_t boundary = 0;
	if ((err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(pcm, sw, period_frames)) < 0 ||
	    (err = snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE)) < 0 ||
	    (err = snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0)
		return alsa_result(err);
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// Streams
// --------------------------------------------------------------------------

static void alsa_set_terminal(struct craudio_stream_t *s, CR_RESULT r) {
	atomic_store(&s->error, (int)r);
	atomic_store(&s->state, r == CR_RESULT_AUDIO_DEVICE_LOST ? CRAUDIO_STREAM_STATE_DEVICE_LOST : CRAUDIO_STREAM_STATE_ERROR);
}

static void alsa_free_stream(struct craudio_stream_t *s) {
	if (s->pcm) {
		if (s->original_chmap)
			snd_pcm_set_chmap(s->pcm, s->original_chmap);
		snd_pcm_close(s->pcm);
	}
	if (s->stop_fd >= 0)
		close(s->stop_fd);
	if (s->startup_initialized)
		sem_destroy(&s->startup);
	free(s->original_chmap); // ALSA's
	const CR_ALLOCATOR allocator = s->head.allocator;
	CR_DELETE_ARRAY(&allocator, void *, s->planes, s->planes_count);
	// Bytes read as the stream's samples.
	cr_free_array(&allocator, s->scratch, s->scratch_frames, s->frame_bytes, CR_MAX_ALIGN);
	cr_string_free(&allocator, s->head.debug_name);
	CR_DELETE(&allocator, struct craudio_stream_t, s);
}

CR_RESULT craudio_stream_create(
    const CRAUDIO_STREAM_DESC *desc,
    craudio_stream *out_stream) {
	if (!out_stream)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_stream  = NULL;
	CR_RESULT r = craudio_validate_stream_desc(desc);
	if (CR_FAILED(r))
		return r;
	if (!desc->callback)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	r = alsa_check_thread(&desc->thread);
	if (CR_FAILED(r))
		return r;

	struct craudio_stream_t *s = CR_NEW_ZEROED(&desc->device->head.allocator, struct craudio_stream_t);
	if (!s)
		return CR_RESULT_OUT_OF_MEMORY;
	s->head.allocator      = desc->device->head.allocator;
	CR_BIRTH(s);
	s->stop_fd             = -1;
	s->direction           = desc->device->direction;
	s->callback            = desc->callback;
	s->user_data           = desc->user_data;
	s->priority            = desc->thread.priority;
	s->sched_fifo_priority = desc->thread.sched_fifo_priority;
	atomic_init(&s->state, CRAUDIO_STREAM_STATE_STOPPED);
	atomic_init(&s->error, CR_SUCCESS);
	atomic_init(&s->underflow_count, 0);
	atomic_init(&s->overflow_count, 0);
	atomic_init(&s->audio_thread_known, false);

	r = alsa_open(desc, &s->config, &s->original_chmap, &s->pcm);
	if (CR_SUCCESS == r)
		r = alsa_set_sw_params(s->pcm, s->config.period_frames);
	if (CR_SUCCESS == r) {
		// hw_params already left the PCM PREPARED; this makes that
		// explicit and is a no-op if so.
		int err = snd_pcm_prepare(s->pcm);
		if (err < 0)
			r = alsa_result(err);
	}
	if (CR_SUCCESS == r) {
		const CR_ALLOCATOR *allocator = &s->head.allocator;
		const uint32_t channels         = s->config.format.channel_count;
		const uint32_t period           = s->config.period_frames;
		s->sample_bytes                 = craudio_sample_format_get_byte_size(s->config.format.sample_format);
		s->frame_bytes                  = s->sample_bytes * channels;
		s->scratch_frames               = period;
		s->planes_count                 = channels;
		s->scratch                      = (uint8_t *)cr_allocate_array(allocator, period, s->frame_bytes, CR_MAX_ALIGN, true);
		s->planes                       = CR_NEW_ARRAY_ZEROED(allocator, void *, channels);
		s->stop_fd                      = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (!s->scratch || !s->planes)
			r = CR_RESULT_OUT_OF_MEMORY;
		else if (s->stop_fd < 0 || sem_init(&s->startup, 0, 0) != 0)
			r = CR_RESULT_API_SPECIFIC_FAILURE;
		else
			s->startup_initialized = true;
		if (CR_SUCCESS == r)
			for (uint32_t ch = 0; ch < channels; ++ch)
				s->planes[ch] = s->scratch + (size_t)ch * period * s->sample_bytes;
	}
	if (CR_FAILED(r)) {
		alsa_free_stream(s);
		return r;
	}

	*out_stream = s;
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// Audio thread
// --------------------------------------------------------------------------

static CR_RESULT alsa_apply_priority(const struct craudio_stream_t *s) {
	if (s->priority != CRAUDIO_THREAD_PRIORITY_SCHED_FIFO)
		return CR_SUCCESS;
	struct sched_param param = {0};
	param.sched_priority     = s->sched_fifo_priority;
	int err                  = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
	if (err == 0)
		return CR_SUCCESS;
	return err == EPERM ? CR_RESULT_AUDIO_THREAD_PRIORITY_DENIED : CR_RESULT_API_SPECIFIC_FAILURE;
}

static uint64_t alsa_ns(const snd_htimestamp_t *t) {
	return (uint64_t)t->tv_sec * 1000000000ULL + (uint64_t)t->tv_nsec;
}

/*
 * Host time of the next frame to transfer, from snd_pcm_status (htstamp is
 * CLOCK_MONOTONIC — set in alsa_set_sw_params). Output: the next frame
 * written plays after the `delay` frames already queued. Input: the next
 * frame read is the oldest unread one, captured `delay` frames ago.
 */
static void alsa_fill_host_time(const struct craudio_stream_t *s, CRAUDIO_CALLBACK_INFO *info) {
	snd_pcm_status_t *status;
	snd_pcm_status_alloca(&status);
	if (snd_pcm_status(s->pcm, status) < 0 || snd_pcm_status_get_state(status) != SND_PCM_STATE_RUNNING)
		return;
	snd_htimestamp_t stamp;
	snd_pcm_status_get_htstamp(status, &stamp);
	const uint64_t now = alsa_ns(&stamp);
	if (now == 0)
		return;
	snd_pcm_sframes_t delay = snd_pcm_status_get_delay(status);
	uint64_t offset         = delay > 0 ? (uint64_t)delay * 1000000000ULL / s->config.format.sample_rate : 0;
	if (s->direction == CRAUDIO_DIRECTION_OUTPUT)
		info->host_time_ns = now + offset;
	else if (offset <= now)
		info->host_time_ns = now - offset;
	else
		return;
	info->flags |= CRAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
}

// Moves `frames` between scratch and the PCM, finishing short transfers.
static int alsa_io(struct craudio_stream_t *s, uint32_t frames) {
	const bool output = s->direction == CRAUDIO_DIRECTION_OUTPUT;
	uint32_t done     = 0;
	while (done < frames) {
		snd_pcm_sframes_t n;
		if (s->config.format.interleaved) {
			uint8_t *at = s->scratch + (size_t)done * s->frame_bytes;
			n           = output ? snd_pcm_writei(s->pcm, at, frames - done) : snd_pcm_readi(s->pcm, at, frames - done);
		} else {
			void *at[CRAUDIO_MAX_CHANNELS];
			for (uint32_t ch = 0; ch < s->config.format.channel_count; ++ch)
				at[ch] = (uint8_t *)s->planes[ch] + (size_t)done * s->sample_bytes;
			n = output ? snd_pcm_writen(s->pcm, at, frames - done) : snd_pcm_readn(s->pcm, at, frames - done);
		}
		if (n == -EAGAIN) {
			snd_pcm_wait(s->pcm, 100);
			continue;
		}
		if (n < 0)
			return (int)n;
		done += (uint32_t)n;
	}
	return 0;
}

// One period through the callback: output calls it then writes, input
// reads then calls it. The callback always gets exactly one period.
static int alsa_transfer(struct craudio_stream_t *s, CRAUDIO_CALLBACK_INFO *info) {
	const uint32_t period       = s->config.period_frames;
	void *frames                = s->config.format.interleaved ? (void *)s->scratch : (void *)s->planes;
	info->device_position_frames = s->frames_transferred;
	if (s->pending_discontinuity) {
		info->flags |= CRAUDIO_CALLBACK_FLAG_DISCONTINUITY;
		s->pending_discontinuity = false;
	}
	int err;
	if (s->direction == CRAUDIO_DIRECTION_OUTPUT) {
		s->callback(s, frames, period, info, s->user_data);
		err = alsa_io(s, period);
	} else {
		err = alsa_io(s, period);
		if (err == 0)
			s->callback(s, frames, period, info, s->user_data);
	}
	if (err == 0)
		s->frames_transferred += period;
	return err;
}

// Output: prefill the whole buffer through the callback, then start. The
// start threshold is the boundary (alsa_set_sw_params), so the PCM only
// ever starts here. Prefill runs before the device clock does, so those
// callbacks carry no host time.
static CR_RESULT alsa_start_device(struct craudio_stream_t *s) {
	if (s->direction == CRAUDIO_DIRECTION_OUTPUT) {
		uint32_t periods = s->config.buffer_frames / s->config.period_frames;
		for (uint32_t i = 0; i < (periods ? periods : 1); ++i) {
			CRAUDIO_CALLBACK_INFO info = {0};
			int err                      = alsa_transfer(s, &info);
			if (err < 0)
				return alsa_result(err);
		}
	}
	int err = snd_pcm_start(s->pcm);
	return err < 0 ? alsa_result(err) : CR_SUCCESS;
}

/*
 * -EPIPE is an xrun: the PCM stopped itself (stop_threshold). WASAPI keeps
 * a stream going through a glitch, so ALSA is brought to the same
 * behaviour: count it, advance the stream position by the frames lost
 * (estimated from when the xrun happened — snd_pcm_status' trigger
 * timestamp — to now), re-prepare and restart, and flag the next callback.
 * -ESTRPIPE is a system suspend: resume if the driver can, otherwise the
 * same re-prepare. Anything else is a real failure.
 */
static CR_RESULT alsa_recover(struct craudio_stream_t *s, int err) {
	if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(s->pcm)) == -EAGAIN)
			poll(NULL, 0, 100);
		if (err == 0) {
			s->pending_discontinuity = true;
			return CR_SUCCESS;
		}
	} else if (err != -EPIPE) {
		return alsa_result(err);
	}

	snd_pcm_status_t *status;
	snd_pcm_status_alloca(&status);
	if (snd_pcm_status(s->pcm, status) == 0 && snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
		snd_htimestamp_t now, at;
		snd_pcm_status_get_htstamp(status, &now);
		snd_pcm_status_get_trigger_htstamp(status, &at);
		if (alsa_ns(&at) && alsa_ns(&now) > alsa_ns(&at))
			s->frames_transferred += (alsa_ns(&now) - alsa_ns(&at)) * s->config.format.sample_rate / 1000000000ULL;
	}
	if (s->direction == CRAUDIO_DIRECTION_OUTPUT)
		atomic_fetch_add(&s->underflow_count, 1);
	else
		atomic_fetch_add(&s->overflow_count, 1);

	err = snd_pcm_prepare(s->pcm);
	if (err < 0)
		return alsa_result(err);
	s->pending_discontinuity = true;
	return alsa_start_device(s);
}

// Transfers every whole period available, recovering from xruns.
static CR_RESULT alsa_service(struct craudio_stream_t *s) {
	for (;;) {
		snd_pcm_sframes_t avail = snd_pcm_avail_update(s->pcm);
		if (avail < 0) {
			CR_RESULT r = alsa_recover(s, (int)avail);
			if (CR_FAILED(r))
				return r;
			continue;
		}
		if ((snd_pcm_uframes_t)avail < s->config.period_frames)
			return CR_SUCCESS;
		CRAUDIO_CALLBACK_INFO info = {0};
		alsa_fill_host_time(s, &info);
		int err = alsa_transfer(s, &info);
		if (err < 0) {
			CR_RESULT r = alsa_recover(s, err);
			if (CR_FAILED(r))
				return r;
		}
	}
}

static void *alsa_audio_thread(void *arg) {
	struct craudio_stream_t *s = (struct craudio_stream_t *)arg;
	s->audio_thread_self         = pthread_self();
	atomic_store(&s->audio_thread_known, true);

	// --- Startup, reported back to craudio_stream_start. The poll set is
	// the stop eventfd plus the PCM's own descriptors, in the array start
	// made for them (NULL when the PCM has none).
	struct pollfd *fds = s->fds;
	const int pcm_fds  = (int)s->pcm_fd_count;
	CR_RESULT r       = alsa_apply_priority(s);
	if (CR_SUCCESS == r) {
		if (!fds)
			r = CR_RESULT_API_SPECIFIC_FAILURE;
		else if (snd_pcm_poll_descriptors(s->pcm, fds + 1, (unsigned int)pcm_fds) != pcm_fds)
			r = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	// RUNNING before the prefill: it calls the callback, and RUNNING is the
	// state while the callback is called.
	atomic_store(&s->state, CRAUDIO_STREAM_STATE_RUNNING);
	if (CR_SUCCESS == r) {
		fds[0].fd     = s->stop_fd;
		fds[0].events = POLLIN;
		r             = alsa_start_device(s);
	}
	if (CR_FAILED(r)) {
		if (r == CR_RESULT_AUDIO_DEVICE_LOST)
			alsa_set_terminal(s, r);
		else
			atomic_store(&s->state, CRAUDIO_STREAM_STATE_STOPPED);
		snd_pcm_drop(s->pcm);
		snd_pcm_prepare(s->pcm);
		s->startup_result = r;
		sem_post(&s->startup);
		return NULL;
	}
	s->startup_result = CR_SUCCESS;
	sem_post(&s->startup);

	// --- Run. snd_pcm_poll_descriptors_revents translates plugin-specific
	// wakeups (dmix timers, ...) into real PCM readiness. The timeout keeps
	// servicing a device that went quiet, which is how a vanished one
	// surfaces (-ENODEV).
	for (;;) {
		int ready = poll(fds, (nfds_t)pcm_fds + 1, 2000);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			alsa_set_terminal(s, CR_RESULT_API_SPECIFIC_FAILURE);
			break;
		}
		if (fds[0].revents & POLLIN)
			break; // stop requested
		unsigned short revents = 0;
		if (ready > 0)
			snd_pcm_poll_descriptors_revents(s->pcm, fds + 1, (unsigned int)pcm_fds, &revents);
		if (ready == 0 || revents & (POLLIN | POLLOUT | POLLERR)) {
			r = alsa_service(s);
			if (CR_FAILED(r)) {
				alsa_set_terminal(s, r);
				break;
			}
		}
	}

	// --- Stop: discard what's queued and leave the PCM PREPARED, ready for
	// a later start.
	snd_pcm_drop(s->pcm);
	snd_pcm_prepare(s->pcm);
	unsigned int running = CRAUDIO_STREAM_STATE_RUNNING;
	atomic_compare_exchange_strong(&s->state, &running, CRAUDIO_STREAM_STATE_STOPPED);
	return NULL;
}

static bool alsa_on_audio_thread(struct craudio_stream_t *s) {
	return atomic_load(&s->audio_thread_known) && pthread_equal(pthread_self(), s->audio_thread_self);
}

// Frees the poll set start made. NULL is nothing to free.
static void alsa_free_poll_set(struct craudio_stream_t *s) {
	CR_DELETE_ARRAY(&s->head.allocator, struct pollfd, s->fds, (size_t)s->pcm_fd_count + 1);
	s->fds          = NULL;
	s->pcm_fd_count = 0;
}

static void alsa_join_thread(struct craudio_stream_t *s) {
	pthread_join(s->thread, NULL);
	s->has_thread = false;
	atomic_store(&s->audio_thread_known, false);
	alsa_free_poll_set(s);
}

CR_RESULT craudio_stream_start(craudio_stream stream) {
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_start", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (alsa_on_audio_thread(stream))
		return CR_RESULT_GENERAL_FAILURE; // would deadlock
	unsigned int state = atomic_load(&stream->state);
	if (state == CRAUDIO_STREAM_STATE_RUNNING)
		return CR_SUCCESS;
	if (state == CRAUDIO_STREAM_STATE_DEVICE_LOST || state == CRAUDIO_STREAM_STATE_ERROR)
		return (CR_RESULT)atomic_load(&stream->error);

	uint64_t drain;
	while (read(stream->stop_fd, &drain, sizeof(drain)) > 0) {
	} // clear a stop left over from the previous run
	stream->frames_transferred    = 0;
	stream->pending_discontinuity = false;

	// The thread's poll set is made here, on the caller's thread, so the
	// audio thread never calls the allocator. A PCM with no descriptors
	// gets none, and the thread reports it.
	const int pcm_fds = snd_pcm_poll_descriptors_count(stream->pcm);
	if (pcm_fds > 0) {
		stream->fds = CR_NEW_ARRAY_ZEROED(&stream->head.allocator, struct pollfd, (size_t)pcm_fds + 1);
		if (!stream->fds)
			return CR_RESULT_OUT_OF_MEMORY;
		stream->pcm_fd_count = (uint32_t)pcm_fds;
	}
	if (pthread_create(&stream->thread, NULL, alsa_audio_thread, stream) != 0) {
		alsa_free_poll_set(stream);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	stream->has_thread = true;

	while (sem_wait(&stream->startup) != 0 && errno == EINTR) {
	}
	if (CR_FAILED(stream->startup_result)) {
		alsa_join_thread(stream);
		return stream->startup_result;
	}
	return CR_SUCCESS;
}

CR_RESULT craudio_stream_stop(craudio_stream stream) {
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_stop", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (alsa_on_audio_thread(stream))
		return CR_RESULT_GENERAL_FAILURE; // would deadlock
	if (!stream->has_thread)
		return CR_SUCCESS; // never started, or already stopped
	const uint64_t one = 1;
	ssize_t written    = write(stream->stop_fd, &one, sizeof(one));
	(void)written; // an eventfd write only fails at counter overflow
	alsa_join_thread(stream);
	return CR_SUCCESS;
}

void craudio_stream_destroy(craudio_stream stream) {
	if (!stream)
		return;
	if (CR_STALE(stream, "craudio_stream_destroy", "stream"))
		return;
	craudio_stream_stop(stream);
	CR_KILL(stream);
	alsa_free_stream(stream);
}

CR_RESULT craudio_stream_get_config(
    craudio_stream stream,
    CRAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_config->base, CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_config", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	*out_config = stream->config; // fixed at creation
	out_config->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	return CR_SUCCESS;
}

CR_RESULT craudio_stream_get_status(
    craudio_stream stream,
    CRAUDIO_STREAM_STATUS *out_status) {
	if (!out_status)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_status", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	out_status->state           = (CRAUDIO_STREAM_STATE)atomic_load(&stream->state);
	out_status->error           = (CR_RESULT)atomic_load(&stream->error);
	out_status->underflow_count = atomic_load(&stream->underflow_count);
	out_status->overflow_count  = atomic_load(&stream->overflow_count);
	return CR_SUCCESS;
}

// snd_pcm_delay: output, frames until a newly written frame is heard;
// input, frames between capture and a read. alsa-lib's PCM calls are
// internally locked (its default thread-safe mode), so this is safe
// alongside the audio thread.
CR_RESULT craudio_stream_get_latency_frames(
    craudio_stream stream,
    uint32_t *out_latency_frames) {
	if (!out_latency_frames)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_latency_frames = 0;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_latency_frames", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	snd_pcm_sframes_t delay = 0;
	int err                 = snd_pcm_delay(stream->pcm, &delay);
	if (err < 0)
		return alsa_result(err);
	*out_latency_frames = delay > 0 ? (uint32_t)delay : 0;
	return CR_SUCCESS;
}

// ALSA has no device-wide rate: each opened PCM sets its own in
// hw_params.
CR_RESULT craudio_device_set_sample_rate(craudio_device device, uint32_t sample_rate) {
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_set_sample_rate", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (sample_rate == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_RESULT_AUDIO_SAMPLE_RATE_NOT_SETTABLE;
}

CR_RESULT craudio_instance_set_device_event_callback(
    craudio_instance instance,
    CRAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data) {
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_instance_set_device_event_callback", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (alsa_in_device_event_callback(instance))
		return CR_RESULT_GENERAL_FAILURE;

	// The new watch is built before the old one goes, so a failure leaves
	// the callback that was set still set.
	struct alsa_watch *watch = NULL;
	if (callback) {
		CR_RESULT r = alsa_watch_create(&instance->head.allocator, callback, user_data, &watch);
		if (CR_FAILED(r))
			return r;
	}
	alsa_watch_destroy(instance->watch);
	instance->watch = watch;
	return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRAUDIO_COMPILE_ALSA

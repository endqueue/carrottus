#ifndef CARROTTUS_CRINPUTSHARED_H
#define CARROTTUS_CRINPUTSHARED_H

/*
 * Carrottus Input: what the front end (src/crinput.c) and the backends share.
 *
 * The front end is the public API: every argument check, the table of
 * gamepads and their ids, and what each call reports. A backend finds
 * devices and reads them, through the functions listed at the end of this
 * file, and writes what it reads into the front end's records. So a rule
 * about a call - what it checks, in what order, what it returns - is
 * written once, in the front end, and is the same on every platform.
 *
 * Nothing outside src/ includes this.
 */

#include "crinput.h"
#include "../crallocator.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 * One connected gamepad: everything the public calls report about it, as of
 * the last poll. A backend fills it during its poll and nowhere else, so
 * that nothing a caller reads changes between two polls.
 */
typedef struct crinput_gamepad_record {
	crinput_gamepad_id id;
	/* The backend's own object for the device. Not null. It is also what
	 * tells one device from another: see crinput_shared_find(). */
	void *native;
	/* Cleared by the front end before a poll and set by the backend for
	 * each device still there; what is left clear has gone. */
	bool seen;
	/* As last set with crinput_gamepad_set_motion_enabled(). */
	bool motion_enabled;

	CRINPUT_GAMEPAD_PROPERTIES properties;
	CRINPUT_GAMEPAD_STATE state;
	CRINPUT_GAMEPAD_BATTERY battery;
	CRINPUT_GAMEPAD_TOUCH_STATE touch[CRINPUT_GAMEPAD_TOUCH_SURFACE_MAX];

	/* The samples of the last poll, oldest first. Room for the instance's
	 * motion_sample_capacity, allocated by the front end the first time
	 * the sensors are switched on; null until then. */
	CRINPUT_GAMEPAD_MOTION_SAMPLE *motion_samples;
	uint32_t motion_sample_count;
	uint32_t motion_dropped_count;
} crinput_gamepad_record;

struct crinput_instance_t {
	/* No debug name: the instance desc has none. head.allocator is the
	 * caller's allocator, copied. Everything of the instance's, the
	 * backend's included, is allocated and freed through it. */
	cr_object_head head;
	/* The caller's desc; its `allocator` points at `head.allocator`. */
	CRINPUT_INSTANCE_DESC desc;

	/* The connected gamepads, in the order they were first seen. */
	crinput_gamepad_record *records;
	uint32_t record_count;
	/* Of `records` and of `ids`. */
	uint32_t record_capacity;
	/* The ids of `records`, as crinput_instance_get_gamepads() hands
	 * them out. Rewritten by each poll that succeeds. */
	crinput_gamepad_id *ids;
	uint32_t id_count;
	/* The next id to give out. Starts at 1 and only goes up. */
	crinput_gamepad_id next_id;

	/* The backend's own state for the instance, or null. */
	void *backend;
};

/* ---- For a backend's poll ---- */

/* Makes room for `extra` gamepads beyond those there are. A backend calls
 * it once, with the number of devices it is about to report, before it
 * changes anything, so that a poll that runs out of memory has changed
 * nothing. Returns CR_RESULT_OUT_OF_MEMORY or CR_SUCCESS. */
CR_RESULT crinput_shared_reserve(
    crinput_instance instance,
    uint32_t extra);

/* A record for a new device: zeroed, with a new id and `native` in it, and
 * marked seen. There must be room (crinput_shared_reserve). Its id is
 * not handed out by crinput_instance_get_gamepads() until the poll
 * succeeds. */
crinput_gamepad_record *crinput_shared_add(
    crinput_instance instance,
    void *native);

/* Copies a null-terminated UTF-8 string into `size` bytes, terminator
 * included, cutting it at a whole character if it doesn't fit. A null
 * `source` gives an empty string. `size` is at least 1. */
void crinput_shared_copy_utf8(
    char *destination,
    uint32_t size,
    const char *source);

/* ---- What a backend provides ---- */

/*
 * A backend is part of the front end's translation unit: src/crinput.c
 * includes the file of each native API built in (with CRINPUT_FRONT_END
 * defined; a backend file compiled on its own is an #error), and CMake
 * compiles none of them separately. Each backend defines the verbs below
 * as crinput_<api>_<verb> (crinput_gameinput_poll, ...), every one with
 * CRINPUT_BACKEND_FUNCTION, and the front end calls them from one switch
 * per verb on the instance's desc.native_api. So any number of APIs can be
 * built into one build, and the caller's desc chooses between them.
 *
 * CRINPUT_BACKEND_FUNCTION makes each of them static and forces it
 * inline into its case of the switch: a call through the switch costs a
 * compare and a branch, no call. Each is called from exactly one case, so
 * inlining copies nothing. Where it can't be inlined the build fails:
 * clang and GCC refuse an always_inline function they can't inline, and
 * the MSVC build makes C4714 an error. MSVC inlines nothing at /Ob0 (its
 * Debug default), so there the cases are plain direct calls.
 *
 * Arguments have been checked by the front end: instance and record are
 * real, the record's gamepad has the feature the call is about, and every
 * number is in range.
 *
 * CR_RESULT create(crinput_instance instance)
 *     Starts the platform's API for a new instance whose desc is in place.
 *     Returns CR_RESULT_INPUT_UNSUPPORTED for a read_in_background the
 *     API doesn't offer. On failure the instance is freed by the front end
 *     and destroy is not called.
 *
 * void destroy(crinput_instance instance)
 *     Ends it. Every gamepad has already been released.
 *
 * CR_RESULT poll(crinput_instance instance)
 *     Reads the platform: for each device still there, finds its record or
 *     adds one (filling its properties), writes its state, battery, touch
 *     and motion samples, and marks it seen. The front end then releases
 *     what was not seen and rewrites the id list. On failure the front end
 *     changes nothing more, so a backend allocates what it needs before it
 *     writes to any record.
 *
 * void release_gamepad(crinput_instance instance, crinput_gamepad_record *record)
 *     The device has gone, or the instance is going: stops its motors and
 *     its sensors and frees `record->native`.
 *
 * CR_RESULT set_rumble(crinput_instance instance, crinput_gamepad_record *record,
 *                       const CRINPUT_GAMEPAD_RUMBLE *rumble)
 *
 * CR_RESULT set_motion_enabled(crinput_instance instance, crinput_gamepad_record *record,
 *                               bool enabled)
 *     `enabled` differs from record->motion_enabled, which the front end
 *     sets afterwards, and record->motion_samples is allocated.
 *
 * CR_RESULT set_light_color(crinput_instance instance, crinput_gamepad_record *record,
 *                            float red, float green, float blue)
 *
 * CR_RESULT set_player_indicator(crinput_instance instance, crinput_gamepad_record *record,
 *                                 uint32_t player)
 *
 * void get_button_label(crinput_instance instance, crinput_gamepad_record *record,
 *                       CRINPUT_GAMEPAD_BUTTONS button, char *out_label, uint32_t label_size)
 *     `out_label` holds an empty string on entry; left so where the
 *     platform has no label. `button` is exactly one bit.
 *
 * void get_axis_label(crinput_instance instance, crinput_gamepad_record *record,
 *                     CRINPUT_GAMEPAD_AXIS axis, char *out_label, uint32_t label_size)
 *     As get_button_label.
 *
 * A new native API adds its file, its #include and a case in each switch
 * in src/crinput.c, and its name to the list in crinput_has_api.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define CRINPUT_BACKEND_FUNCTION static __forceinline
#else
#define CRINPUT_BACKEND_FUNCTION static inline __attribute__((always_inline))
#endif

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CARROTTUS_CRINPUTSHARED_H

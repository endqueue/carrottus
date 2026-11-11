#include "input/crinputshared.h"
#include "crallocator.h"
#include "crstructbase.h"

#include <stdlib.h>
#include <string.h>

/*
 * Carrottus Input's front end: the public API, the same on every platform.
 *
 * Every argument check is here, in one order, and so is the table of
 * gamepads: which are connected, their ids, and what each call reports
 * about them. A backend (src/input/backends/) only finds devices and reads
 * them into the records. See src/input/crinputshared.h.
 */

/* Every button bit crinput.h names. */
#define CRINPUT_ALL_BUTTONS ((CRINPUT_GAMEPAD_BUTTONS)((CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_1 << 1) - 1))

/* ---- The backends ---- */

/* Each native API built in is compiled here, in this translation unit, so
 * that its functions are inlined into the switches below. See
 * src/input/crinputshared.h. */
#define CRINPUT_FRONT_END 1
#if CRINPUT_COMPILE_GAMECONTROLLER
#include "input/backends/apple/crinputgamecontroller.m"
#endif
#if CRINPUT_COMPILE_GAMEINPUT
#include "input/backends/windows/crinputgameinput.c"
#endif
#if CRINPUT_COMPILE_EVDEV
#include "input/backends/linux/crinputevdev.c"
#endif

/* ---- Dispatch ---- */

/* Whether this build can bind an instance to `api`. */
static bool crinput_has_api(CRINPUT_NATIVE_API api) {
	switch (api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return true;
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return true;
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return true;
#endif
	default:
		return false;
	}
}

/* One per verb of crinputshared.h, each a switch on the instance's API.
 * Only an instance of an API built in exists, so `default` is never
 * reached; it returns what an unknown API would get. */

static CR_RESULT crinput_backend_create(crinput_instance instance) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_create(instance);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_create(instance);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_create(instance);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static void crinput_backend_destroy(crinput_instance instance) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		crinput_gamecontroller_destroy(instance);
		return;
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		crinput_gameinput_destroy(instance);
		return;
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		crinput_evdev_destroy(instance);
		return;
#endif
	default:
		return;
	}
}

static CR_RESULT crinput_backend_poll(crinput_instance instance) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_poll(instance);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_poll(instance);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_poll(instance);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static void crinput_backend_release_gamepad(
    crinput_instance instance,
    crinput_gamepad_record *record) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		crinput_gamecontroller_release_gamepad(instance, record);
		return;
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		crinput_gameinput_release_gamepad(instance, record);
		return;
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		crinput_evdev_release_gamepad(instance, record);
		return;
#endif
	default:
		return;
	}
}

static CR_RESULT crinput_backend_set_rumble(
    crinput_instance instance,
    crinput_gamepad_record *record,
    const CRINPUT_GAMEPAD_RUMBLE *rumble) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_set_rumble(instance, record, rumble);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_set_rumble(instance, record, rumble);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_set_rumble(instance, record, rumble);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static CR_RESULT crinput_backend_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_record *record,
    bool enabled) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_set_motion_enabled(instance, record, enabled);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_set_motion_enabled(instance, record, enabled);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_set_motion_enabled(instance, record, enabled);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static CR_RESULT crinput_backend_set_light_color(
    crinput_instance instance,
    crinput_gamepad_record *record,
    float red,
    float green,
    float blue) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_set_light_color(instance, record, red, green, blue);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_set_light_color(instance, record, red, green, blue);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_set_light_color(instance, record, red, green, blue);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static CR_RESULT crinput_backend_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_record *record,
    uint32_t player) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		return crinput_gamecontroller_set_player_indicator(instance, record, player);
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		return crinput_gameinput_set_player_indicator(instance, record, player);
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		return crinput_evdev_set_player_indicator(instance, record, player);
#endif
	default:
		return CR_RESULT_INVALID_API;
	}
}

static void crinput_backend_get_button_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		crinput_gamecontroller_get_button_label(instance, record, button, out_label, label_size);
		return;
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		crinput_gameinput_get_button_label(instance, record, button, out_label, label_size);
		return;
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		crinput_evdev_get_button_label(instance, record, button, out_label, label_size);
		return;
#endif
	default:
		return;
	}
}

static void crinput_backend_get_axis_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size) {
	switch (instance->desc.native_api) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	case CRINPUT_NATIVE_API_GAMECONTROLLER:
		crinput_gamecontroller_get_axis_label(instance, record, axis, out_label, label_size);
		return;
#endif
#if CRINPUT_COMPILE_GAMEINPUT
	case CRINPUT_NATIVE_API_GAMEINPUT:
		crinput_gameinput_get_axis_label(instance, record, axis, out_label, label_size);
		return;
#endif
#if CRINPUT_COMPILE_EVDEV
	case CRINPUT_NATIVE_API_EVDEV:
		crinput_evdev_get_axis_label(instance, record, axis, out_label, label_size);
		return;
#endif
	default:
		return;
	}
}

/* ---- Shared with the backends ---- */

CR_RESULT crinput_shared_reserve(
    crinput_instance instance,
    uint32_t extra) {
	const uint64_t needed = (uint64_t)instance->record_count + extra;
	if (needed <= instance->record_capacity)
		return CR_SUCCESS;
	if (needed > UINT32_MAX)
		return CR_RESULT_OUT_OF_MEMORY;

	/* Both arrays or neither, so that both stay `record_capacity` long,
	 * which is the count each is freed with. */
	const CR_ALLOCATOR *allocator = &instance->head.allocator;
	crinput_gamepad_id *ids       = CR_NEW_ARRAY(allocator, crinput_gamepad_id, (size_t)needed);
	if (!ids)
		return CR_RESULT_OUT_OF_MEMORY;
	crinput_gamepad_record *records = CR_NEW_ARRAY(allocator, crinput_gamepad_record, (size_t)needed);
	if (!records) {
		CR_DELETE_ARRAY(allocator, crinput_gamepad_id, ids, (size_t)needed);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	if (instance->id_count)
		memcpy(ids, instance->ids, (size_t)instance->id_count * sizeof(crinput_gamepad_id));
	if (instance->record_count)
		memcpy(records, instance->records, (size_t)instance->record_count * sizeof(crinput_gamepad_record));
	CR_DELETE_ARRAY(allocator, crinput_gamepad_id, instance->ids, instance->record_capacity);
	CR_DELETE_ARRAY(allocator, crinput_gamepad_record, instance->records, instance->record_capacity);
	instance->ids             = ids;
	instance->records         = records;
	instance->record_capacity = (uint32_t)needed;
	return CR_SUCCESS;
}

crinput_gamepad_record *crinput_shared_add(
    crinput_instance instance,
    void *native) {
	crinput_gamepad_record *record = &instance->records[instance->record_count++];
	memset(record, 0, sizeof(*record));
	record->id     = instance->next_id++;
	record->native = native;
	record->seen   = true;
	return record;
}

void crinput_shared_copy_utf8(
    char *destination,
    uint32_t size,
    const char *source) {
	destination[0] = '\0';
	if (!source)
		return;

	const size_t length = strlen(source);
	size_t count        = length;
	if (count > (size_t)size - 1) {
		count = (size_t)size - 1;
		/* Back to the start of the character the cut falls inside: the
		 * bytes after a character's first all begin with the bits 10. */
		while (count > 0 && ((unsigned char)source[count] & 0xC0) == 0x80)
			--count;
	}
	memcpy(destination, source, count);
	destination[count] = '\0';
}

/* The record of a gamepad that is connected as of the last poll, or NULL.
 * One a failed poll added is not found: its id is not in the list yet. */
static crinput_gamepad_record *crinput_find_gamepad(
    crinput_instance instance,
    crinput_gamepad_id gamepad) {
	if (gamepad == CRINPUT_GAMEPAD_ID_NONE)
		return NULL;

	for (uint32_t i = 0; i < instance->id_count; ++i) {
		if (instance->ids[i] != gamepad)
			continue;

		/* The list is in the records' order, but a failed poll may have
		 * added records since it was written. */
		for (uint32_t j = 0; j < instance->record_count; ++j) {
			if (instance->records[j].id == gamepad)
				return &instance->records[j];
		}
		return NULL;
	}
	return NULL;
}

/* Whether a level, or a colour's part, is a number from 0 to 1. */
static bool crinput_is_unit(float value) { return value >= 0.0f && value <= 1.0f; }

/* ---- Instance ---- */

CR_RESULT crinput_instance_create(
    const CRINPUT_INSTANCE_DESC *desc,
    crinput_instance *out_instance) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_INPUT_INSTANCE_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_instance)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_instance = NULL;
	const CR_RESULT allocator_result = cr_allocator_check(desc->allocator);
	if (allocator_result != CR_SUCCESS)
		return allocator_result;
	if (!crinput_has_api(desc->native_api))
		return CR_RESULT_INVALID_API;

	crinput_instance instance = CR_NEW_ZEROED(desc->allocator, struct crinput_instance_t);
	if (!instance)
		return CR_RESULT_OUT_OF_MEMORY;
	instance->head.allocator = *desc->allocator;
	CR_BIRTH(instance);
	instance->desc      = *desc;
	/* The caller's chain and allocator struct needn't outlive this call;
	 * nothing is chained yet. */
	instance->desc.base.next = NULL;
	instance->desc.allocator = &instance->head.allocator;
	instance->next_id        = 1;

	const CR_RESULT result = crinput_backend_create(instance);
	if (result != CR_SUCCESS) {
		const CR_ALLOCATOR allocator = instance->head.allocator;
		CR_DELETE(&allocator, struct crinput_instance_t, instance);
		return result;
	}

	*out_instance = instance;
	return CR_SUCCESS;
}

void crinput_instance_destroy(crinput_instance instance) {
	if (!instance)
		return;
	if (CR_STALE(instance, "crinput_instance_destroy", "instance"))
		return;

	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	for (uint32_t i = 0; i < instance->record_count; ++i) {
		crinput_backend_release_gamepad(instance, &instance->records[i]);
		CR_DELETE_ARRAY(&allocator, CRINPUT_GAMEPAD_MOTION_SAMPLE, instance->records[i].motion_samples, instance->desc.motion_sample_capacity);
	}
	crinput_backend_destroy(instance);

	CR_DELETE_ARRAY(&allocator, crinput_gamepad_record, instance->records, instance->record_capacity);
	CR_DELETE_ARRAY(&allocator, crinput_gamepad_id, instance->ids, instance->record_capacity);
	CR_DELETE(&allocator, struct crinput_instance_t, instance);
}

CRINPUT_NATIVE_API crinput_instance_get_native_api(crinput_instance instance) {
	if (!instance)
		return CRINPUT_NATIVE_API_NONE;
	if (CR_STALE(instance, "crinput_instance_get_native_api", "instance"))
		return CRINPUT_NATIVE_API_NONE;

	return instance->desc.native_api;
}

CR_RESULT crinput_instance_poll(crinput_instance instance) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_instance_poll", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	for (uint32_t i = 0; i < instance->record_count; ++i)
		instance->records[i].seen = false;

	const CR_RESULT result = crinput_backend_poll(instance);
	if (result != CR_SUCCESS)
		return result;

	/* What was not seen has gone. The rest move down, keeping their
	 * order. */
	uint32_t kept = 0;
	for (uint32_t i = 0; i < instance->record_count; ++i) {
		crinput_gamepad_record *record = &instance->records[i];
		if (!record->seen) {
			crinput_backend_release_gamepad(instance, record);
			CR_DELETE_ARRAY(&instance->head.allocator, CRINPUT_GAMEPAD_MOTION_SAMPLE, record->motion_samples, instance->desc.motion_sample_capacity);
			continue;
		}

		if (kept != i)
			instance->records[kept] = *record;
		++kept;
	}
	instance->record_count = kept;

	/* There is room: the id array is as long as the record array. */
	for (uint32_t i = 0; i < kept; ++i)
		instance->ids[i] = instance->records[i].id;
	instance->id_count = kept;

	return CR_SUCCESS;
}

/* ---- Gamepads ---- */

CR_RESULT crinput_instance_get_gamepads(
    crinput_instance instance,
    const crinput_gamepad_id **out_gamepads,
    uint32_t *out_gamepad_count) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_instance_get_gamepads", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (!out_gamepads || !out_gamepad_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	*out_gamepads      = instance->id_count ? instance->ids : NULL;
	*out_gamepad_count = instance->id_count;
	return CR_SUCCESS;
}

CR_RESULT crinput_gamepad_get_properties(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_PROPERTIES *out_properties) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_properties", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	const crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!out_properties)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_result = cr_struct_out_check(&out_properties->base, CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES);
	if (base_result != CR_SUCCESS)
		return base_result;

	*out_properties = record->properties;
	/* The record's copy has no base of its own; the check above leaves the
	 * caller's as this one. */
	out_properties->base = cr_struct_base_out(CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES);
	return CR_SUCCESS;
}

CR_RESULT crinput_gamepad_get_state(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_STATE *out_state) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_state", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	const crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!out_state)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	*out_state = record->state;
	return CR_SUCCESS;
}

/* ---- Rumble ---- */

CR_RESULT crinput_gamepad_set_rumble(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    const CRINPUT_GAMEPAD_RUMBLE *rumble) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_set_rumble", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!rumble)
		return CR_RESULT_NULL_DESC;
	/* Written so that a level that is not a number fails as well. */
	if (!crinput_is_unit(rumble->low_frequency) || !crinput_is_unit(rumble->high_frequency) || !crinput_is_unit(rumble->left_trigger) ||
	    !crinput_is_unit(rumble->right_trigger))
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	const CRINPUT_GAMEPAD_FEATURES features = record->properties.features;
	if ((rumble->low_frequency > 0.0f && !(features & CRINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY)) ||
	    (rumble->high_frequency > 0.0f && !(features & CRINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY)) ||
	    (rumble->left_trigger > 0.0f && !(features & CRINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER)) ||
	    (rumble->right_trigger > 0.0f && !(features & CRINPUT_GAMEPAD_FEATURE_RUMBLE_RIGHT_TRIGGER)))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;

	return crinput_backend_set_rumble(instance, record, rumble);
}

/* ---- Battery ---- */

CR_RESULT crinput_gamepad_get_battery(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_BATTERY *out_battery) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_battery", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	const crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_BATTERY))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (!out_battery)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	*out_battery = record->battery;
	return CR_SUCCESS;
}

/* ---- Motion sensors ---- */

#define CRINPUT_MOTION_FEATURES (CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER | CRINPUT_GAMEPAD_FEATURE_GYROSCOPE)

CR_RESULT crinput_gamepad_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    bool enabled) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_set_motion_enabled", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_MOTION_FEATURES))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (enabled && instance->desc.motion_sample_capacity == 0)
		return CR_RESULT_ZERO_SIZE;
	if (enabled == record->motion_enabled)
		return CR_SUCCESS;

	if (enabled && !record->motion_samples) {
		record->motion_samples =
		    CR_NEW_ARRAY_ZEROED(&instance->head.allocator, CRINPUT_GAMEPAD_MOTION_SAMPLE, instance->desc.motion_sample_capacity);
		if (!record->motion_samples)
			return CR_RESULT_OUT_OF_MEMORY;
	}

	const CR_RESULT result = crinput_backend_set_motion_enabled(instance, record, enabled);
	if (result != CR_SUCCESS)
		return result;

	record->motion_enabled = enabled;
	if (!enabled) {
		/* Off: nothing is handed out from here on. */
		record->motion_sample_count  = 0;
		record->motion_dropped_count = 0;
	}
	return CR_SUCCESS;
}

CR_RESULT crinput_gamepad_get_motion(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    const CRINPUT_GAMEPAD_MOTION_SAMPLE **out_samples,
    uint32_t *out_sample_count,
    uint32_t *out_dropped_count) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_motion", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	const crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_MOTION_FEATURES))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (!out_samples || !out_sample_count || !out_dropped_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	*out_samples       = record->motion_sample_count ? record->motion_samples : NULL;
	*out_sample_count  = record->motion_sample_count;
	*out_dropped_count = record->motion_dropped_count;
	return CR_SUCCESS;
}

/* ---- Touch surfaces ---- */

CR_RESULT crinput_gamepad_get_touch(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    uint32_t surface,
    CRINPUT_GAMEPAD_TOUCH_STATE *out_touch) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_touch", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	const crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (surface >= record->properties.touch_surface_count)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (!out_touch)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	*out_touch = record->touch[surface];
	return CR_SUCCESS;
}

/* ---- Lights ---- */

CR_RESULT crinput_gamepad_set_light_color(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    float red,
    float green,
    float blue) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_set_light_color", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (!crinput_is_unit(red) || !crinput_is_unit(green) || !crinput_is_unit(blue))
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	return crinput_backend_set_light_color(instance, record, red, green, blue);
}

CR_RESULT crinput_gamepad_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    uint32_t player) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_set_player_indicator", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (player > record->properties.player_indicator_max)
		return CR_RESULT_INDEX_OUT_OF_RANGE;

	return crinput_backend_set_player_indicator(instance, record, player);
}

/* ---- Labels ---- */

CR_RESULT crinput_gamepad_get_button_label(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_button_label", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_LABELS))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	/* No bit, a bit this header doesn't name, or more than one. */
	if (button == 0 || (button & ~CRINPUT_ALL_BUTTONS) || (button & (button - 1)))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!out_label)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (label_size == 0)
		return CR_RESULT_ZERO_SIZE;

	out_label[0] = '\0';
	crinput_backend_get_button_label(instance, record, button, out_label, label_size);
	return CR_SUCCESS;
}

CR_RESULT crinput_gamepad_get_axis_label(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size) {
	if (!instance)
		return CR_RESULT_INPUT_INVALID_INSTANCE;
	if (CR_STALE(instance, "crinput_gamepad_get_axis_label", "instance"))
		return CR_RESULT_INPUT_INVALID_INSTANCE;

	crinput_gamepad_record *record = crinput_find_gamepad(instance, gamepad);
	if (!record)
		return CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_LABELS))
		return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
	if (axis >= CRINPUT_GAMEPAD_AXIS_COUNT)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (!out_label)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (label_size == 0)
		return CR_RESULT_ZERO_SIZE;

	out_label[0] = '\0';
	crinput_backend_get_axis_label(instance, record, axis, out_label, label_size);
	return CR_SUCCESS;
}

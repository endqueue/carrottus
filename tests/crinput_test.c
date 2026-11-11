/*
 * crinput_test — Carrottus Input's front end, and whatever gamepads are there.
 *
 * It needs no gamepad: with none connected it checks everything that can be
 * checked without one (what each call returns for a bad argument and for a
 * gamepad that isn't there, and that a poll with nothing to find succeeds),
 * so it is a ctest. With one or more connected it also reads each, checks
 * that what it reports agrees with itself, and prints it, which is as far as
 * a test with nobody holding the gamepad can go. It moves no motor and sets
 * no light.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <crinput.h>

/* The front end's own helpers, for the one that has no public call of its
 * own to be reached through. */
#include "../src/input/crinputshared.h"
#include "crtest_allocator.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if CRINPUT_COMPILE_GAMECONTROLLER
#include <CoreFoundation/CoreFoundation.h>
#endif

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

/* The native API this build has, or NONE. */
static CRINPUT_NATIVE_API native_api(void) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	return CRINPUT_NATIVE_API_GAMECONTROLLER;
#elif CRINPUT_COMPILE_GAMEINPUT
	return CRINPUT_NATIVE_API_GAMEINPUT;
#elif CRINPUT_COMPILE_XINPUT
	return CRINPUT_NATIVE_API_XINPUT;
#elif CRINPUT_COMPILE_EVDEV
	return CRINPUT_NATIVE_API_EVDEV;
#else
	return CRINPUT_NATIVE_API_NONE;
#endif
}

/* Lets the platform notice devices for a moment, where it needs the caller
 * to: GameController's arrive only while the main run loop runs. */
static void let_devices_arrive(void) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
#endif
}

static void test_copy_utf8(void) {
	char out[8];

	crinput_shared_copy_utf8(out, sizeof(out), "pad");
	CHECK(strcmp(out, "pad") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), NULL);
	CHECK(strcmp(out, "") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), "");
	CHECK(strcmp(out, "") == 0);

	/* Exactly fits, and one too many. */
	crinput_shared_copy_utf8(out, sizeof(out), "1234567");
	CHECK(strcmp(out, "1234567") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), "12345678");
	CHECK(strcmp(out, "1234567") == 0);

	/* Room for the terminator alone. */
	crinput_shared_copy_utf8(out, 1, "pad");
	CHECK(strcmp(out, "") == 0);

	/* A cut that would fall inside a character falls before it instead:
	 * U+00E9 is two bytes, U+20AC three, U+1F3AE four. */
	crinput_shared_copy_utf8(out, sizeof(out), "123456\xC3\xA9");
	CHECK(strcmp(out, "123456") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), "12345\xE2\x82\xAC");
	CHECK(strcmp(out, "12345") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), "1234\xF0\x9F\x8E\xAE");
	CHECK(strcmp(out, "1234") == 0);
	/* And one that fits whole is kept whole. */
	crinput_shared_copy_utf8(out, sizeof(out), "12345\xC3\xA9");
	CHECK(strcmp(out, "12345\xC3\xA9") == 0);
	crinput_shared_copy_utf8(out, sizeof(out), "123\xF0\x9F\x8E\xAE");
	CHECK(strcmp(out, "123\xF0\x9F\x8E\xAE") == 0);
}

static void test_create_arguments(void) {
	CRINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	crinput_instance instance = (crinput_instance)(uintptr_t)1;

	CHECK(crinput_instance_create(NULL, &instance) == CR_RESULT_NULL_DESC);

	/* Struct bases: a zeroed base, another struct's type and a chained
	 * struct are each refused, before anything after the desc is looked at. */
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_STRUCT_WRONG_TYPE);
	CHECK(crinput_instance_create(&desc, NULL) == CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_STRUCT_WRONG_TYPE);
	CRINPUT_INSTANCE_DESC chained;
	memset(&chained, 0, sizeof(chained));
	chained.base.type = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.base.type    = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.base.next    = &chained.base;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	desc.base.next = NULL;

	CHECK(crinput_instance_create(&desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	/* A desc with only its base set has not chosen an allocator, and one
	 * missing a function is refused. */
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_NULL_ALLOCATOR);
	CHECK(instance == NULL);
	CR_ALLOCATOR allocator = cr_allocator_system();
	allocator.allocate       = NULL;
	desc.allocator           = &allocator;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_INVALID_ALLOCATOR);
	allocator      = cr_allocator_system();
	allocator.free = NULL;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_INVALID_ALLOCATOR);
	allocator = cr_allocator_system();

	/* Nor an API. */
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_INVALID_API);
	CHECK(instance == NULL);

	/* One no build has, and every one this build doesn't. */
	desc.native_api = 99;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_INVALID_API);
	for (CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_GAMECONTROLLER; api <= CRINPUT_NATIVE_API_EVDEV; ++api) {
		if (api == native_api())
			continue;
		desc.native_api = api;
		instance        = (crinput_instance)(uintptr_t)1;
		CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_INVALID_API);
		CHECK(instance == NULL);
	}

	crinput_instance_destroy(NULL);
}

static void test_null_instance(void) {
	const crinput_gamepad_id *ids = NULL;
	uint32_t count                  = 0;
	CRINPUT_GAMEPAD_PROPERTIES properties;
	CRINPUT_GAMEPAD_STATE state;
	CRINPUT_GAMEPAD_BATTERY battery;
	CRINPUT_GAMEPAD_TOUCH_STATE touch;
	const CRINPUT_GAMEPAD_MOTION_SAMPLE *samples = NULL;
	uint32_t dropped                               = 0;
	CRINPUT_GAMEPAD_RUMBLE rumble;
	memset(&rumble, 0, sizeof(rumble));
	char label[16];

	const CR_RESULT invalid = CR_RESULT_INPUT_INVALID_INSTANCE;
	CHECK(crinput_instance_get_native_api(NULL) == CRINPUT_NATIVE_API_NONE);
	CHECK(crinput_instance_poll(NULL) == invalid);
	CHECK(crinput_instance_get_gamepads(NULL, &ids, &count) == invalid);
	CHECK(crinput_gamepad_get_properties(NULL, 1, &properties) == invalid);
	CHECK(crinput_gamepad_get_state(NULL, 1, &state) == invalid);
	CHECK(crinput_gamepad_set_rumble(NULL, 1, &rumble) == invalid);
	CHECK(crinput_gamepad_get_battery(NULL, 1, &battery) == invalid);
	CHECK(crinput_gamepad_set_motion_enabled(NULL, 1, true) == invalid);
	CHECK(crinput_gamepad_get_motion(NULL, 1, &samples, &count, &dropped) == invalid);
	CHECK(crinput_gamepad_get_touch(NULL, 1, 0, &touch) == invalid);
	CHECK(crinput_gamepad_set_light_color(NULL, 1, 0.0f, 0.0f, 0.0f) == invalid);
	CHECK(crinput_gamepad_set_player_indicator(NULL, 1, 0) == invalid);
	CHECK(crinput_gamepad_get_button_label(NULL, 1, CRINPUT_GAMEPAD_BUTTON_SOUTH, label, sizeof(label)) == invalid);
	CHECK(crinput_gamepad_get_axis_label(NULL, 1, CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X, label, sizeof(label)) == invalid);
}

/* Every call that takes a gamepad, given one that is not there. */
static void test_absent_gamepad(
    crinput_instance instance,
    crinput_gamepad_id gamepad) {
	CRINPUT_GAMEPAD_PROPERTIES properties;
	CRINPUT_GAMEPAD_STATE state;
	CRINPUT_GAMEPAD_BATTERY battery;
	CRINPUT_GAMEPAD_TOUCH_STATE touch;
	const CRINPUT_GAMEPAD_MOTION_SAMPLE *samples = NULL;
	uint32_t count                                 = 0;
	uint32_t dropped                               = 0;
	CRINPUT_GAMEPAD_RUMBLE rumble;
	memset(&rumble, 0, sizeof(rumble));
	char label[16];

	const CR_RESULT gone = CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED;
	CHECK(crinput_gamepad_get_properties(instance, gamepad, &properties) == gone);
	CHECK(crinput_gamepad_get_state(instance, gamepad, &state) == gone);
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == gone);
	CHECK(crinput_gamepad_get_battery(instance, gamepad, &battery) == gone);
	CHECK(crinput_gamepad_set_motion_enabled(instance, gamepad, true) == gone);
	CHECK(crinput_gamepad_get_motion(instance, gamepad, &samples, &count, &dropped) == gone);
	CHECK(crinput_gamepad_get_touch(instance, gamepad, 0, &touch) == gone);
	CHECK(crinput_gamepad_set_light_color(instance, gamepad, 0.0f, 0.0f, 0.0f) == gone);
	CHECK(crinput_gamepad_set_player_indicator(instance, gamepad, 0) == gone);
	CHECK(crinput_gamepad_get_button_label(instance, gamepad, CRINPUT_GAMEPAD_BUTTON_SOUTH, label, sizeof(label)) == gone);
	CHECK(crinput_gamepad_get_axis_label(instance, gamepad, CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X, label, sizeof(label)) == gone);

	/* That the gamepad is not there comes before anything else that is
	 * wrong with the call. */
	CHECK(crinput_gamepad_get_properties(instance, gamepad, NULL) == gone);
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, NULL) == gone);
	CHECK(crinput_gamepad_get_button_label(instance, gamepad, 0, NULL, 0) == gone);
}

/* One connected gamepad: what it says of itself has to agree with itself.
 * Nothing here needs anyone to touch it. */
static void test_connected_gamepad(
    crinput_instance instance,
    crinput_gamepad_id gamepad) {
	CRINPUT_GAMEPAD_PROPERTIES properties;
	memset(&properties, 0xff, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES;
	properties.base.next = NULL;
	CHECK(crinput_gamepad_get_properties(instance, gamepad, &properties) == CR_SUCCESS);
	CHECK(crinput_gamepad_get_properties(instance, gamepad, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	/* The caller's base is left as it was. */
	CHECK(properties.base.type == CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES);
	CHECK(properties.base.next == NULL);

	/* Struct bases: a zeroed base, another struct's type and a chained
	 * struct are each refused, and nothing is written. */
	CRINPUT_GAMEPAD_PROPERTIES refused;
	memset(&refused, 0, sizeof(refused));
	CHECK(crinput_gamepad_get_properties(instance, gamepad, &refused) == CR_RESULT_STRUCT_WRONG_TYPE);
	refused.base.type = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	CHECK(crinput_gamepad_get_properties(instance, gamepad, &refused) == CR_RESULT_STRUCT_WRONG_TYPE);
	CRINPUT_GAMEPAD_PROPERTIES chained;
	memset(&chained, 0, sizeof(chained));
	chained.base.type = CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES;
	refused.base.type = CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES;
	refused.base.next = &chained.base;
	CHECK(crinput_gamepad_get_properties(instance, gamepad, &refused) == CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	CHECK(refused.name[0] == '\0');
	CHECK(refused.buttons == 0);
	CHECK(refused.base.next == &chained.base);

	/* The name is terminated inside its array. */
	CHECK(memchr(properties.name, '\0', CRINPUT_GAMEPAD_NAME_SIZE) != NULL);
	CHECK(properties.touch_surface_count <= CRINPUT_GAMEPAD_TOUCH_SURFACE_MAX);
	for (uint32_t i = 0; i < properties.touch_surface_count; ++i)
		CHECK(properties.touch_surfaces[i].contact_max <= CRINPUT_GAMEPAD_TOUCH_CONTACT_MAX);
	if (!(properties.features & CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR))
		CHECK(properties.player_indicator_max == 0);

	printf("  gamepad %" PRIu64 ": \"%s\" vendor %04x product %04x\n", gamepad, properties.name, properties.vendor_id, properties.product_id);
	printf(
	    "    buttons %05x axes %02x analog triggers %d features %03x touch surfaces %u player indicator max %u\n", properties.buttons, properties.axes,
	    (int)properties.analog_triggers, properties.features, properties.touch_surface_count, properties.player_indicator_max);

	CRINPUT_GAMEPAD_STATE state;
	CRINPUT_GAMEPAD_STATE again;
	CHECK(crinput_gamepad_get_state(instance, gamepad, &state) == CR_SUCCESS);
	CHECK(crinput_gamepad_get_state(instance, gamepad, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	/* Reading changes nothing. */
	CHECK(crinput_gamepad_get_state(instance, gamepad, &again) == CR_SUCCESS);
	CHECK(memcmp(&state, &again, sizeof(state)) == 0);

	/* Nothing it doesn't have is down, and each axis is in its range. */
	CHECK((state.buttons & ~properties.buttons) == 0);
	CHECK((state.buttons_went_down & ~properties.buttons) == 0);
	CHECK((state.buttons_went_up & ~properties.buttons) == 0);
	for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
		const float low = axis < CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER ? -1.0f : 0.0f;
		CHECK(state.axes[axis] >= low);
		CHECK(state.axes[axis] <= 1.0f);
		if (!(properties.axes & (1u << axis)))
			CHECK(state.axes[axis] == 0.0f);
	}
	printf(
	    "    state: buttons %05x axes %+.3f %+.3f %+.3f %+.3f %.3f %.3f\n", state.buttons, state.axes[0], state.axes[1], state.axes[2], state.axes[3],
	    state.axes[4], state.axes[5]);

	/* Rumble: the checks that come before any motor is touched. All four
	 * at 0 is asked of every gamepad, and moves nothing. */
	CRINPUT_GAMEPAD_RUMBLE rumble;
	memset(&rumble, 0, sizeof(rumble));
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, NULL) == CR_RESULT_NULL_DESC);
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_SUCCESS);
	rumble.low_frequency = -0.1f;
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_RESULT_DESC_INVALID_PARAMETERS);
	rumble.low_frequency = 1.5f;
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_RESULT_DESC_INVALID_PARAMETERS);
	rumble.low_frequency = NAN;
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_RESULT_DESC_INVALID_PARAMETERS);
	rumble.low_frequency = 0.0f;
	rumble.right_trigger = NAN;
	CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_RESULT_DESC_INVALID_PARAMETERS);
	rumble.right_trigger = 0.0f;
	if (!(properties.features & CRINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER)) {
		rumble.left_trigger = 0.5f;
		CHECK(crinput_gamepad_set_rumble(instance, gamepad, &rumble) == CR_RESULT_INPUT_FEATURE_NOT_PRESENT);
		rumble.left_trigger = 0.0f;
	}

	CRINPUT_GAMEPAD_BATTERY battery;
	if (properties.features & CRINPUT_GAMEPAD_FEATURE_BATTERY) {
		CHECK(crinput_gamepad_get_battery(instance, gamepad, &battery) == CR_SUCCESS);
		CHECK(crinput_gamepad_get_battery(instance, gamepad, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(battery.state <= CRINPUT_BATTERY_STATE_FULL);
		CHECK(battery.level >= 0.0f);
		CHECK(battery.level <= 1.0f);
		if (!battery.level_known)
			CHECK(battery.level == 0.0f);
		printf("    battery: state %u level %.2f known %d steps %u\n", battery.state, battery.level, (int)battery.level_known, battery.level_steps);
	} else {
		CHECK(crinput_gamepad_get_battery(instance, gamepad, &battery) == CR_RESULT_INPUT_FEATURE_NOT_PRESENT);
	}

	/* Motion: the instance was made keeping no samples, so the sensors
	 * can't be switched on, and nothing is handed out. */
	const CRINPUT_GAMEPAD_MOTION_SAMPLE *samples = NULL;
	uint32_t sample_count                          = 7;
	uint32_t dropped_count                         = 7;
	if (properties.features & (CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER | CRINPUT_GAMEPAD_FEATURE_GYROSCOPE)) {
		CHECK(crinput_gamepad_set_motion_enabled(instance, gamepad, true) == CR_RESULT_ZERO_SIZE);
		CHECK(crinput_gamepad_set_motion_enabled(instance, gamepad, false) == CR_SUCCESS);
		CHECK(crinput_gamepad_get_motion(instance, gamepad, &samples, &sample_count, &dropped_count) == CR_SUCCESS);
		CHECK(samples == NULL);
		CHECK(sample_count == 0);
		CHECK(dropped_count == 0);
		CHECK(crinput_gamepad_get_motion(instance, gamepad, NULL, &sample_count, &dropped_count) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crinput_gamepad_get_motion(instance, gamepad, &samples, NULL, &dropped_count) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crinput_gamepad_get_motion(instance, gamepad, &samples, &sample_count, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	} else {
		const CR_RESULT absent = CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
		CHECK(crinput_gamepad_set_motion_enabled(instance, gamepad, true) == absent);
		CHECK(crinput_gamepad_get_motion(instance, gamepad, &samples, &sample_count, &dropped_count) == absent);
	}

	CRINPUT_GAMEPAD_TOUCH_STATE touch;
	for (uint32_t surface = 0; surface < properties.touch_surface_count; ++surface) {
		CHECK(crinput_gamepad_get_touch(instance, gamepad, surface, &touch) == CR_SUCCESS);
		CHECK(crinput_gamepad_get_touch(instance, gamepad, surface, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(touch.contact_count <= properties.touch_surfaces[surface].contact_max);
		for (uint32_t i = 0; i < touch.contact_count && i < CRINPUT_GAMEPAD_TOUCH_CONTACT_MAX; ++i) {
			CHECK(touch.contacts[i].x >= 0.0f);
			CHECK(touch.contacts[i].x <= 1.0f);
			CHECK(touch.contacts[i].y >= 0.0f);
			CHECK(touch.contacts[i].y <= 1.0f);
		}
	}
	CHECK(crinput_gamepad_get_touch(instance, gamepad, properties.touch_surface_count, &touch) == CR_RESULT_INDEX_OUT_OF_RANGE);

	/* Lights: only what is refused. Nothing is lit or put out. */
	if (properties.features & CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR) {
		CHECK(crinput_gamepad_set_light_color(instance, gamepad, 1.5f, 0.0f, 0.0f) == CR_RESULT_DESC_INVALID_PARAMETERS);
		CHECK(crinput_gamepad_set_light_color(instance, gamepad, 0.0f, -0.5f, 0.0f) == CR_RESULT_DESC_INVALID_PARAMETERS);
		CHECK(crinput_gamepad_set_light_color(instance, gamepad, 0.0f, 0.0f, NAN) == CR_RESULT_DESC_INVALID_PARAMETERS);
	} else {
		CHECK(crinput_gamepad_set_light_color(instance, gamepad, 0.0f, 0.0f, 0.0f) == CR_RESULT_INPUT_FEATURE_NOT_PRESENT);
	}
	if (properties.features & CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR) {
		CHECK(crinput_gamepad_set_player_indicator(instance, gamepad, properties.player_indicator_max + 1) == CR_RESULT_INDEX_OUT_OF_RANGE);
	} else {
		CHECK(crinput_gamepad_set_player_indicator(instance, gamepad, 0) == CR_RESULT_INPUT_FEATURE_NOT_PRESENT);
	}

	char label[64];
	if (properties.features & CRINPUT_GAMEPAD_FEATURE_LABELS) {
		const CRINPUT_GAMEPAD_BUTTONS south = CRINPUT_GAMEPAD_BUTTON_SOUTH;
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, 0, label, sizeof(label)) == CR_RESULT_DESC_INVALID_PARAMETERS);
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, south | CRINPUT_GAMEPAD_BUTTON_EAST, label, sizeof(label)) ==
		      CR_RESULT_DESC_INVALID_PARAMETERS);
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, 1u << 30, label, sizeof(label)) == CR_RESULT_DESC_INVALID_PARAMETERS);
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, south, NULL, sizeof(label)) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, south, label, 0) == CR_RESULT_ZERO_SIZE);
		CHECK(crinput_gamepad_get_axis_label(instance, gamepad, CRINPUT_GAMEPAD_AXIS_COUNT, label, sizeof(label)) == CR_RESULT_INDEX_OUT_OF_RANGE);
		CHECK(crinput_gamepad_get_axis_label(instance, gamepad, 0, NULL, sizeof(label)) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crinput_gamepad_get_axis_label(instance, gamepad, 0, label, 0) == CR_RESULT_ZERO_SIZE);

		printf("    labels:");
		for (CRINPUT_GAMEPAD_BUTTONS bit = 1u; bit <= CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_1; bit <<= 1) {
			memset(label, 0x7f, sizeof(label));
			CHECK(crinput_gamepad_get_button_label(instance, gamepad, bit, label, sizeof(label)) == CR_SUCCESS);
			CHECK(memchr(label, '\0', sizeof(label)) != NULL);
			if (label[0])
				printf(" [%s]", label);

			/* Into one byte: the terminator and nothing else. */
			char tiny[1] = {0x7f};
			CHECK(crinput_gamepad_get_button_label(instance, gamepad, bit, tiny, 1) == CR_SUCCESS);
			CHECK(tiny[0] == '\0');
		}
		for (CRINPUT_GAMEPAD_AXIS axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
			CHECK(crinput_gamepad_get_axis_label(instance, gamepad, axis, label, sizeof(label)) == CR_SUCCESS);
			CHECK(memchr(label, '\0', sizeof(label)) != NULL);
			if (label[0])
				printf(" <%s>", label);
		}
		printf("\n");
	} else {
		CHECK(crinput_gamepad_get_button_label(instance, gamepad, CRINPUT_GAMEPAD_BUTTON_SOUTH, label, sizeof(label)) ==
		      CR_RESULT_INPUT_FEATURE_NOT_PRESENT);
	}
}

static void test_instance(void) {
	/* Everything the instances below allocate is through this, and checked
	 * at the end to have been freed as it was allocated. */
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);

	CRINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type  = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.allocator  = &allocator;
	desc.native_api = native_api();
	/* Only while this process is the one in use, and keeping no motion
	 * samples: the test switches no sensor on. */
	desc.read_in_background     = false;
	desc.motion_sample_capacity = 0;

#if CRINPUT_COMPILE_XINPUT || CRINPUT_COMPILE_EVDEV
	/* These always read; saying otherwise is refused. */
	crinput_instance refused = (crinput_instance)(uintptr_t)1;
	CHECK(crinput_instance_create(&desc, &refused) == CR_RESULT_INPUT_UNSUPPORTED);
	CHECK(refused == NULL);
	desc.read_in_background = true;
#endif

	/* No memory: refused, with nothing made and nothing kept. */
	crinput_instance instance = (crinput_instance)(uintptr_t)1;
	tracker.refuse_after        = 0;
	CHECK(crinput_instance_create(&desc, &instance) == CR_RESULT_OUT_OF_MEMORY);
	CHECK(instance == NULL);
	CHECK(crtest_allocator_clean(&tracker));
	tracker.refuse_after = SIZE_MAX;

	/* The caller's struct need not outlive the call: the instance keeps a
	 * copy, so this one is overwritten once it is made. */
	CR_ALLOCATOR passed = allocator;
	desc.allocator        = &passed;
	CHECK(crinput_instance_create(&desc, &instance) == CR_SUCCESS);
	memset(&passed, 0, sizeof(passed));
	desc.allocator = &allocator;
	if (!instance)
		return;
	CHECK(tracker.given > 0);
	CHECK(crinput_instance_get_native_api(instance) == native_api());

	const crinput_gamepad_id *ids = (const crinput_gamepad_id *)(uintptr_t)1;
	uint32_t count                  = 7;

	/* Before the first poll there is nothing, whatever is plugged in. */
	CHECK(crinput_instance_get_gamepads(instance, &ids, &count) == CR_SUCCESS);
	CHECK(ids == NULL);
	CHECK(count == 0);
	CHECK(crinput_instance_get_gamepads(instance, NULL, &count) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crinput_instance_get_gamepads(instance, &ids, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	test_absent_gamepad(instance, CRINPUT_GAMEPAD_ID_NONE);
	test_absent_gamepad(instance, 1);

	let_devices_arrive();
	CHECK(crinput_instance_poll(instance) == CR_SUCCESS);
	CHECK(crinput_instance_get_gamepads(instance, &ids, &count) == CR_SUCCESS);
	CHECK((ids == NULL) == (count == 0));
	printf("crinput_test: %u gamepad(s) connected\n", count);
	if (count == 0)
		printf("crinput_test: no gamepad, so crinput_gamepad_get_properties' struct-base refusals are skipped\n");

	/* Ids are given out from 1, each once, in the order first seen. */
	crinput_gamepad_id highest = CRINPUT_GAMEPAD_ID_NONE;
	for (uint32_t i = 0; i < count; ++i) {
		CHECK(ids[i] > highest);
		highest = ids[i];
	}
	test_absent_gamepad(instance, CRINPUT_GAMEPAD_ID_NONE);
	test_absent_gamepad(instance, highest + 1);
	test_absent_gamepad(instance, UINT64_MAX);

	/* A copy: the array is the instance's and the next poll may move it. */
	crinput_gamepad_id seen[16];
	const uint32_t seen_count = count < 16 ? count : 16;
	for (uint32_t i = 0; i < seen_count; ++i)
		seen[i] = ids[i];
	for (uint32_t i = 0; i < seen_count; ++i)
		test_connected_gamepad(instance, seen[i]);

	/* More polls, as a caller makes them: nothing has been unplugged in
	 * these few milliseconds, so the same gamepads keep the same ids. */
	for (int i = 0; i < 20; ++i)
		CHECK(crinput_instance_poll(instance) == CR_SUCCESS);
	CHECK(crinput_instance_get_gamepads(instance, &ids, &count) == CR_SUCCESS);
	CHECK(count >= seen_count);
	for (uint32_t i = 0; i < seen_count && i < count; ++i)
		CHECK(ids[i] == seen[i]);

	/* After the first poll nothing has gone down "since the poll before"
	 * unless someone is pressing: it can't be checked, but the state can
	 * still be read. */
	CRINPUT_GAMEPAD_STATE state;
	for (uint32_t i = 0; i < seen_count; ++i)
		CHECK(crinput_gamepad_get_state(instance, seen[i], &state) == CR_SUCCESS);

	crinput_instance_destroy(instance);

	/* A second instance, made and destroyed with nothing asked of it, and
	 * one that polls: an instance holds no state of another's. */
	CHECK(crinput_instance_create(&desc, &instance) == CR_SUCCESS);
	crinput_instance_destroy(instance);
	CHECK(crinput_instance_create(&desc, &instance) == CR_SUCCESS);
	if (instance) {
		CHECK(crinput_instance_poll(instance) == CR_SUCCESS);
		CHECK(crinput_instance_get_gamepads(instance, &ids, &count) == CR_SUCCESS);
		/* Its ids start from 1 again. */
		if (count > 0)
			CHECK(ids[0] == 1);
		crinput_instance_destroy(instance);
	}

	CHECK(crtest_allocator_clean(&tracker));
}

int main(void) {
	test_copy_utf8();
	test_create_arguments();
	test_null_instance();
	if (native_api() == CRINPUT_NATIVE_API_NONE)
		printf("crinput_test: no native input API in this build; the instance tests are skipped\n");
	else
		test_instance();

	if (failures)
		fprintf(stderr, "crinput_test: %d failure(s)\n", failures);
	else
		printf("crinput_test: all passed\n");
	return failures ? 1 : 0;
}

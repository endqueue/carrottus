// Only when the GameInput API is compiled in (see crinput.h).
#if CRINPUT_COMPILE_GAMEINPUT

// Part of src/crinput.c's translation unit, which includes it; never
// compiled on its own (see src/input/crinputshared.h).
#ifndef CRINPUT_FRONT_END
#error "crinputgameinput.c is included by src/crinput.c, not compiled on its own."
#endif

/*
 * Carrottus Input on GameInput: Windows.
 *
 * The GameInput of the Windows SDK (GameInput.h, gameinput.lib, the
 * GameInput.dll the system or its redistributable installs). A device is a
 * gamepad when GameInput presents it as GameInputKindGamepad, and
 * GameInputGamepadState is the whole of the mapping: there is no table of
 * devices in this file.
 *
 * GameInput says which devices come and go only through a device callback.
 * The gamepads connected when the instance is made are listed then, with a
 * blocking enumeration, so the first poll has them. After that, so that
 * nothing runs on a thread Carrottus Input didn't ask for, the instance makes a
 * dispatcher, which takes GameInput's work off its own threads: the work,
 * and the callbacks with it, then run inside the dispatches each poll
 * makes. The callback only writes down which devices are connected; the
 * poll turns that into records. The list has a lock all the same, taken
 * only outside a dispatch, in case a runtime delivers a callback elsewhere.
 *
 * GameInput keeps a history of each device's readings, so a poll walks
 * every reading since the one it last took, and a press shorter than the
 * time between two polls is still in buttons_went_down and buttons_went_up.
 * When the reading taken last has left the history
 * (GAMEINPUT_E_REFERENCE_READING_TOO_OLD), the poll takes the newest one and
 * what happened in between is reported as the difference.
 *
 * GameInput's gamepad, as this file reads it:
 * - Sticks: -1 to 1, y positive away from the user, as crinput.h has it.
 * - Triggers: 0 to 1.
 * - Face buttons: A south, B east, X west, Y north. View is BACK, Menu is
 *   START. The guide button is not part of GameInput's gamepad state.
 *
 * Rumble is SetRumbleState with the four motor levels, which GameInput holds
 * until they are set again. A gamepad has the battery feature when GameInput
 * says anything of its battery when it is first seen.
 *
 * Not given: motion sensors, touch surfaces, lights and labels. GameInput's
 * gamepad has no sensors or touch surfaces, no way to set a light, and its
 * labels are an enum rather than the platform's words.
 *
 * The SDK's C macros for IGameInputDispatcher are missing a closing
 * parenthesis, so every method here is called through lpVtbl.
 */

#include "../../crinputshared.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GameInput.h>

#include <stdlib.h>
#include <string.h>

/* The most dispatches one poll makes. Each does the work that can be done
 * without waiting; more than a few in a row only happen when devices come
 * and go in a burst, and what is left is done at the next poll. */
#define CRINPUT_GAMEINPUT_DISPATCH_MAX 16

/* One gamepad: the device, and the reading of it taken last. */
typedef struct crinput_gameinput_pad {
	IGameInputDevice *device;
	/* Null until the first reading. */
	IGameInputReading *reading;
} crinput_gameinput_pad;

typedef EQ_ARRAY(IGameInputDevice *) crinput_gameinput_device_array;

typedef struct crinput_gameinput_instance {
	IGameInput *game_input;
	IGameInputDispatcher *dispatcher;
	GameInputCallbackToken device_token;
	/* The instance's allocator, for the device callback, which is given only
	 * this struct. */
	const CR_ALLOCATOR *allocator;
	/* eqstd's view of it (cr_eq_allocator), which `connected` points at. */
	EQ_ALLOCATOR eq_allocator;

	/* Guards what follows, which the device callback writes. */
	SRWLOCK lock;
	/* The gamepads connected, as the callback has said, each with a
	 * reference of its own. */
	crinput_gameinput_device_array connected;
	/* A callback could not write a change down for want of memory: the
	 * list is to be made again from GameInput. */
	bool lost;
} crinput_gameinput_instance;

/* GameInput's gamepad buttons, each with crinput.h's. */
static const struct {
	GameInputGamepadButtons native;
	CRINPUT_GAMEPAD_BUTTONS button;
} crinput_gameinput_buttons[] = {
	{GameInputGamepadA, CRINPUT_GAMEPAD_BUTTON_SOUTH},
	{GameInputGamepadB, CRINPUT_GAMEPAD_BUTTON_EAST},
	{GameInputGamepadX, CRINPUT_GAMEPAD_BUTTON_WEST},
	{GameInputGamepadY, CRINPUT_GAMEPAD_BUTTON_NORTH},
	{GameInputGamepadLeftShoulder, CRINPUT_GAMEPAD_BUTTON_LEFT_SHOULDER},
	{GameInputGamepadRightShoulder, CRINPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER},
	{GameInputGamepadLeftThumbstick, CRINPUT_GAMEPAD_BUTTON_LEFT_STICK},
	{GameInputGamepadRightThumbstick, CRINPUT_GAMEPAD_BUTTON_RIGHT_STICK},
	{GameInputGamepadView, CRINPUT_GAMEPAD_BUTTON_BACK},
	{GameInputGamepadMenu, CRINPUT_GAMEPAD_BUTTON_START},
	{GameInputGamepadDPadUp, CRINPUT_GAMEPAD_BUTTON_DPAD_UP},
	{GameInputGamepadDPadDown, CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN},
	{GameInputGamepadDPadLeft, CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT},
	{GameInputGamepadDPadRight, CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT},
};

#define CRINPUT_GAMEINPUT_BUTTON_COUNT (sizeof(crinput_gameinput_buttons) / sizeof(crinput_gameinput_buttons[0]))

static CRINPUT_GAMEPAD_BUTTONS crinput_gameinput_map_buttons(GameInputGamepadButtons native) {
	CRINPUT_GAMEPAD_BUTTONS buttons = 0;
	for (size_t i = 0; i < CRINPUT_GAMEINPUT_BUTTON_COUNT; ++i) {
		if (native & crinput_gameinput_buttons[i].native)
			buttons |= crinput_gameinput_buttons[i].button;
	}
	return buttons;
}

/* ---- Which devices are connected ---- */

static int64_t crinput_gameinput_find_connected(
    const crinput_gameinput_instance *backend,
    const IGameInputDevice *device) {
	for (size_t i = 0; i < backend->connected.count; ++i) {
		if (backend->connected.data[i] == device)
			return (int64_t)i;
	}
	return -1;
}

/* Drops every device the list holds. Called with the lock held, or when
 * nothing else can reach the list. */
static void crinput_gameinput_clear_connected(crinput_gameinput_instance *backend) {
	for (size_t i = 0; i < backend->connected.count; ++i)
		backend->connected.data[i]->lpVtbl->Release(backend->connected.data[i]);
	eq_array_clear(&backend->connected);
}

/* GameInput says a gamepad came or went. Only the move between connected
 * and not is written down. */
static void CALLBACK crinput_gameinput_device_callback(
    GameInputCallbackToken token,
    void *context,
    IGameInputDevice *device,
    uint64_t timestamp,
    GameInputDeviceStatus current_status,
    GameInputDeviceStatus previous_status) {
	(void)token;
	(void)timestamp;
	crinput_gameinput_instance *backend = (crinput_gameinput_instance *)context;
	const bool is_connected               = (current_status & GameInputDeviceConnected) != 0;
	const bool was_connected              = (previous_status & GameInputDeviceConnected) != 0;
	if (is_connected == was_connected)
		return;

	AcquireSRWLockExclusive(&backend->lock);
	const int64_t index = crinput_gameinput_find_connected(backend, device);
	if (is_connected && index < 0) {
		if (eq_array_push(&backend->connected, &device) != EQ_SUCCESS) {
			backend->lost = true;
			ReleaseSRWLockExclusive(&backend->lock);
			return;
		}
		device->lpVtbl->AddRef(device);
	} else if (!is_connected && index >= 0) {
		device->lpVtbl->Release(device);
		// An index inside the list, so the removal can't fail.
		(void)eq_array_remove_swap(&backend->connected, (size_t)index);
	}
	ReleaseSRWLockExclusive(&backend->lock);
}

/* Asks GameInput to tell the callback about every gamepad, those connected
 * now included: before this returns with GameInputBlockingEnumeration, from
 * the next dispatch on with GameInputAsyncEnumeration. */
static CR_RESULT crinput_gameinput_register(
    crinput_gameinput_instance *backend,
    GameInputEnumerationKind enumeration) {
	const HRESULT hr = backend->game_input->lpVtbl->RegisterDeviceCallback(backend->game_input,
	    NULL,
	    GameInputKindGamepad,
	    GameInputDeviceConnected,
	    enumeration,
	    backend,
	    crinput_gameinput_device_callback,
	    &backend->device_token);
	if (FAILED(hr)) {
		backend->device_token = 0;
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	return CR_SUCCESS;
}

static void crinput_gameinput_unregister(crinput_gameinput_instance *backend) {
	if (!backend->device_token)
		return;
	backend->game_input->lpVtbl->UnregisterCallback(backend->game_input, backend->device_token, UINT64_MAX);
	backend->device_token = 0;
}

/* ---- One gamepad ---- */

static void crinput_gameinput_stop_rumble(IGameInputDevice *device) {
	const GameInputRumbleParams stopped = {0.0f, 0.0f, 0.0f, 0.0f};
	device->lpVtbl->SetRumbleState(device, &stopped);
}

/* What GameInput says of a gamepad's battery, as crinput.h has it. */
static CRINPUT_GAMEPAD_BATTERY crinput_gameinput_battery(const GameInputBatteryState *native) {
	CRINPUT_GAMEPAD_BATTERY battery = {0};
	switch (native->status) {
	case GameInputBatteryNotPresent:
		battery.state = CRINPUT_BATTERY_STATE_NONE;
		return battery;
	case GameInputBatteryDischarging:
		battery.state = CRINPUT_BATTERY_STATE_DISCHARGING;
		break;
	/* On power, and neither filling nor emptying. */
	case GameInputBatteryIdle:
		battery.state = CRINPUT_BATTERY_STATE_FULL;
		break;
	case GameInputBatteryCharging:
		battery.state = CRINPUT_BATTERY_STATE_CHARGING;
		break;
	default:
		battery.state = CRINPUT_BATTERY_STATE_UNKNOWN;
		return battery;
	}
	if (native->fullChargeCapacity > 0.0f) {
		float level = native->remainingCapacity / native->fullChargeCapacity;
		if (level < 0.0f)
			level = 0.0f;
		if (level > 1.0f)
			level = 1.0f;
		battery.level_known = true;
		battery.level       = level;
	}
	return battery;
}

static void crinput_gameinput_fill_properties(
    crinput_gamepad_record *record,
    IGameInputDevice *device) {
	const GameInputDeviceInfo *info           = device->lpVtbl->GetDeviceInfo(device);
	CRINPUT_GAMEPAD_PROPERTIES *properties = &record->properties;

	crinput_shared_copy_utf8(properties->name, sizeof(properties->name), info->displayName ? info->displayName->data : NULL);
	properties->vendor_id  = info->vendorId;
	properties->product_id = info->productId;
	for (size_t i = 0; i < CRINPUT_GAMEINPUT_BUTTON_COUNT; ++i)
		properties->buttons |= crinput_gameinput_buttons[i].button;
	properties->axes            = (1u << CRINPUT_GAMEPAD_AXIS_COUNT) - 1;
	properties->analog_triggers = true;

	if (info->supportedRumbleMotors & GameInputRumbleLowFrequency)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY;
	if (info->supportedRumbleMotors & GameInputRumbleHighFrequency)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY;
	if (info->supportedRumbleMotors & GameInputRumbleLeftTrigger)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER;
	if (info->supportedRumbleMotors & GameInputRumbleRightTrigger)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_RIGHT_TRIGGER;

	GameInputBatteryState battery;
	device->lpVtbl->GetBatteryState(device, &battery);
	if (battery.status != GameInputBatteryUnknown)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_BATTERY;
}

/* Takes one reading into the record's state: the buttons that changed since
 * the state before it, and the axes as they are in it. */
static void crinput_gameinput_take_reading(
    crinput_gamepad_record *record,
    IGameInputReading *reading) {
	GameInputGamepadState native;
	if (!reading->lpVtbl->GetGamepadState(reading, &native))
		return;

	CRINPUT_GAMEPAD_STATE *state          = &record->state;
	const CRINPUT_GAMEPAD_BUTTONS buttons = crinput_gameinput_map_buttons(native.buttons);
	state->buttons_went_down |= buttons & ~state->buttons;
	state->buttons_went_up |= state->buttons & ~buttons;
	state->buttons = buttons;

	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X]  = native.leftThumbstickX;
	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y]  = native.leftThumbstickY;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_X] = native.rightThumbstickX;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y] = native.rightThumbstickY;
	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER]  = native.leftTrigger;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER] = native.rightTrigger;
}

/* Every reading of the gamepad since the one taken last, into its record.
 * The first time, the newest reading alone: the buttons down in it are the
 * ones that went down. */
static void crinput_gameinput_read_pad(
    IGameInput *game_input,
    crinput_gamepad_record *record) {
	crinput_gameinput_pad *pad    = (crinput_gameinput_pad *)record->native;
	record->state.buttons_went_down = 0;
	record->state.buttons_went_up   = 0;

	if (pad->reading) {
		for (;;) {
			IGameInputReading *next = NULL;
			const HRESULT hr        = game_input->lpVtbl->GetNextReading(game_input, pad->reading, GameInputKindGamepad, pad->device, &next);
			if (SUCCEEDED(hr)) {
				crinput_gameinput_take_reading(record, next);
				pad->reading->lpVtbl->Release(pad->reading);
				pad->reading = next;
				continue;
			}
			/* No newer reading: what was there has all been taken. */
			if (hr != GAMEINPUT_E_REFERENCE_READING_TOO_OLD)
				return;
			/* The one taken last has left the history: start again from the
			 * newest. */
			pad->reading->lpVtbl->Release(pad->reading);
			pad->reading = NULL;
			break;
		}
	}

	IGameInputReading *current = NULL;
	if (FAILED(game_input->lpVtbl->GetCurrentReading(game_input, GameInputKindGamepad, pad->device, &current)))
		return;
	crinput_gameinput_take_reading(record, current);
	pad->reading = current;
}

static void crinput_gameinput_read_battery(crinput_gamepad_record *record) {
	if (!(record->properties.features & CRINPUT_GAMEPAD_FEATURE_BATTERY))
		return;
	const crinput_gameinput_pad *pad = (const crinput_gameinput_pad *)record->native;
	GameInputBatteryState battery;
	pad->device->lpVtbl->GetBatteryState(pad->device, &battery);
	record->battery = crinput_gameinput_battery(&battery);
}

/* ---- Backend ---- */

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_create(crinput_instance instance) {
	const CR_ALLOCATOR *allocator       = &instance->head.allocator;
	crinput_gameinput_instance *backend = CR_NEW_ZEROED(allocator, crinput_gameinput_instance);
	if (!backend)
		return CR_RESULT_OUT_OF_MEMORY;
	backend->allocator    = allocator;
	backend->eq_allocator = cr_eq_allocator(allocator);
	eq_array_init(&backend->connected, &backend->eq_allocator);
	InitializeSRWLock(&backend->lock);

	if (FAILED(GameInputCreate(&backend->game_input))) {
		CR_DELETE(allocator, crinput_gameinput_instance, backend);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	/* GameInput's own default reads in the background. */
	backend->game_input->lpVtbl->SetFocusPolicy(
	    backend->game_input, instance->desc.read_in_background ? GameInputDefaultFocusPolicy : GameInputDisableBackgroundInput);

	/* The gamepads connected now are in the list before this returns, so
	 * the first poll lists them. Before the dispatcher: a blocking
	 * enumeration waits on GameInput's work, which with a dispatcher is
	 * done only when one is dispatched. */
	const CR_RESULT result = crinput_gameinput_register(backend, GameInputBlockingEnumeration);
	if (result != CR_SUCCESS) {
		backend->game_input->lpVtbl->Release(backend->game_input);
		eq_array_free(&backend->connected);
		CR_DELETE(allocator, crinput_gameinput_instance, backend);
		return result;
	}

	/* From here GameInput's work, the callback's included, is done in the
	 * polls' dispatches. */
	if (FAILED(backend->game_input->lpVtbl->CreateDispatcher(backend->game_input, &backend->dispatcher))) {
		crinput_gameinput_unregister(backend);
		crinput_gameinput_clear_connected(backend);
		eq_array_free(&backend->connected);
		backend->game_input->lpVtbl->Release(backend->game_input);
		CR_DELETE(allocator, crinput_gameinput_instance, backend);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	instance->backend = backend;
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_gameinput_destroy(crinput_instance instance) {
	crinput_gameinput_instance *backend = (crinput_gameinput_instance *)instance->backend;

	crinput_gameinput_unregister(backend);
	crinput_gameinput_clear_connected(backend);
	eq_array_free(&backend->connected);
	backend->dispatcher->lpVtbl->Release(backend->dispatcher);
	backend->game_input->lpVtbl->Release(backend->game_input);
	CR_DELETE(&instance->head.allocator, crinput_gameinput_instance, backend);
	instance->backend = NULL;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_poll(crinput_instance instance) {
	crinput_gameinput_instance *backend = (crinput_gameinput_instance *)instance->backend;

	/* GameInput's work, the device callbacks among it, without the lock:
	 * the callback takes it. */
	for (int i = 0; i < CRINPUT_GAMEINPUT_DISPATCH_MAX; ++i) {
		if (!backend->dispatcher->lpVtbl->Dispatch(backend->dispatcher, 0))
			break;
	}

	AcquireSRWLockExclusive(&backend->lock);
	const bool lost = backend->lost;
	ReleaseSRWLockExclusive(&backend->lock);
	if (lost) {
		/* A change went unrecorded: ask GameInput for every gamepad again.
		 * Until it has answered, none is connected; the records are kept as
		 * they are by failing this poll. */
		crinput_gameinput_unregister(backend);
		AcquireSRWLockExclusive(&backend->lock);
		crinput_gameinput_clear_connected(backend);
		backend->lost = false;
		ReleaseSRWLockExclusive(&backend->lock);
		/* Asynchronous: with the dispatcher made, a blocking enumeration
		 * would wait on work only a dispatch does. */
		const CR_RESULT result = crinput_gameinput_register(backend, GameInputAsyncEnumeration);
		return result == CR_SUCCESS ? CR_RESULT_OUT_OF_MEMORY : result;
	}

	AcquireSRWLockShared(&backend->lock);

	/* Everything that can fail for want of memory, before any record is
	 * written to: a pad for each device that has no record yet, and room
	 * for them in the table. */
	uint32_t arrived_count = 0;
	for (size_t i = 0; i < backend->connected.count; ++i) {
		bool known = false;
		for (uint32_t j = 0; j < instance->record_count && !known; ++j)
			known = ((crinput_gameinput_pad *)instance->records[j].native)->device == backend->connected.data[i];
		if (!known)
			++arrived_count;
	}

	crinput_gameinput_pad **arrived = NULL;
	if (arrived_count) {
		arrived = CR_NEW_ARRAY_ZEROED(&instance->head.allocator, crinput_gameinput_pad *, arrived_count);
		if (!arrived) {
			ReleaseSRWLockShared(&backend->lock);
			return CR_RESULT_OUT_OF_MEMORY;
		}
		for (uint32_t i = 0; i < arrived_count; ++i) {
			arrived[i] = CR_NEW_ZEROED(&instance->head.allocator, crinput_gameinput_pad);
			if (!arrived[i]) {
				for (uint32_t j = 0; j < i; ++j)
					CR_DELETE(&instance->head.allocator, crinput_gameinput_pad, arrived[j]);
				CR_DELETE_ARRAY(&instance->head.allocator, crinput_gameinput_pad *, arrived, arrived_count);
				ReleaseSRWLockShared(&backend->lock);
				return CR_RESULT_OUT_OF_MEMORY;
			}
		}
	}
	const CR_RESULT reserved = crinput_shared_reserve(instance, arrived_count);
	if (reserved != CR_SUCCESS) {
		for (uint32_t i = 0; i < arrived_count; ++i)
			CR_DELETE(&instance->head.allocator, crinput_gameinput_pad, arrived[i]);
		CR_DELETE_ARRAY(&instance->head.allocator, crinput_gameinput_pad *, arrived, arrived_count);
		ReleaseSRWLockShared(&backend->lock);
		return reserved;
	}

	/* The gamepads already known that are still there. */
	const uint32_t known_count = instance->record_count;
	for (uint32_t j = 0; j < known_count; ++j) {
		crinput_gamepad_record *record = &instance->records[j];
		crinput_gameinput_pad *pad     = (crinput_gameinput_pad *)record->native;
		if (crinput_gameinput_find_connected(backend, pad->device) < 0)
			continue;
		crinput_gameinput_read_pad(backend->game_input, record);
		crinput_gameinput_read_battery(record);
		record->seen = true;
	}

	/* The new ones. */
	uint32_t next = 0;
	for (size_t i = 0; i < backend->connected.count; ++i) {
		IGameInputDevice *device = backend->connected.data[i];
		bool known               = false;
		for (uint32_t j = 0; j < known_count && !known; ++j)
			known = ((crinput_gameinput_pad *)instance->records[j].native)->device == device;
		if (known)
			continue;

		crinput_gameinput_pad *pad = arrived[next++];
		pad->device                  = device;
		device->lpVtbl->AddRef(device);
		crinput_gameinput_stop_rumble(device);

		crinput_gamepad_record *record = crinput_shared_add(instance, pad);
		crinput_gameinput_fill_properties(record, device);
		crinput_gameinput_read_pad(backend->game_input, record);
		crinput_gameinput_read_battery(record);
	}
	ReleaseSRWLockShared(&backend->lock);
	CR_DELETE_ARRAY(&instance->head.allocator, crinput_gameinput_pad *, arrived, arrived_count);

	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_gameinput_release_gamepad(
    crinput_instance instance,
    crinput_gamepad_record *record) {
	crinput_gameinput_pad *pad = (crinput_gameinput_pad *)record->native;
	crinput_gameinput_stop_rumble(pad->device);
	if (pad->reading)
		pad->reading->lpVtbl->Release(pad->reading);
	pad->device->lpVtbl->Release(pad->device);
	CR_DELETE(&instance->head.allocator, crinput_gameinput_pad, pad);
	record->native = NULL;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_set_rumble(
    crinput_instance instance,
    crinput_gamepad_record *record,
    const CRINPUT_GAMEPAD_RUMBLE *rumble) {
	(void)instance;
	const crinput_gameinput_pad *pad = (const crinput_gameinput_pad *)record->native;
	const GameInputRumbleParams params = {rumble->low_frequency, rumble->high_frequency, rumble->left_trigger, rumble->right_trigger};
	pad->device->lpVtbl->SetRumbleState(pad->device, &params);
	return CR_SUCCESS;
}

/* What follows is never reached: no gamepad is given the feature it is
 * about, and the front end checks that first. */

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_record *record,
    bool enabled) {
	(void)instance;
	(void)record;
	(void)enabled;
	return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_set_light_color(
    crinput_instance instance,
    crinput_gamepad_record *record,
    float red,
    float green,
    float blue) {
	(void)instance;
	(void)record;
	(void)red;
	(void)green;
	(void)blue;
	return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gameinput_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_record *record,
    uint32_t player) {
	(void)instance;
	(void)record;
	(void)player;
	return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
}

CRINPUT_BACKEND_FUNCTION void crinput_gameinput_get_button_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	(void)record;
	(void)button;
	(void)out_label;
	(void)label_size;
}

CRINPUT_BACKEND_FUNCTION void crinput_gameinput_get_axis_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	(void)record;
	(void)axis;
	(void)out_label;
	(void)label_size;
}

#endif // CRINPUT_COMPILE_GAMEINPUT

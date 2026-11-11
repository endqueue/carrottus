// Only when the GameController API is compiled in (see crinput.h).
#if CRINPUT_COMPILE_GAMECONTROLLER

// Part of src/crinput.c's translation unit, which includes it; never
// compiled on its own (see src/input/crinputshared.h).
#ifndef CRINPUT_FRONT_END
#error "crinputgamecontroller.m is included by src/crinput.c, not compiled on its own."
#endif

/*
 * Carrottus Input on the GameController framework: macOS, iOS and iPadOS.
 *
 * GameController keeps its own list of controllers and its own picture of
 * each, both brought up to date as the main thread's run loop runs. A poll
 * here reads that list and those pictures; nothing waits and no thread is
 * started. Only a controller with an extended gamepad profile is a gamepad
 * in crinput.h's sense, and that profile is the whole of the mapping:
 * there is no table of devices in this file.
 *
 * What the framework reports as it happens, and not only as it stands, is
 * taken by handlers it calls on the main queue between polls: button
 * changes, so that a press shorter than a poll is still seen, and motion
 * samples. They write only to this file's own object for the device, which
 * the next poll reads, and they run on the thread that polls, so nothing
 * is locked.
 *
 * Rumble is CoreHaptics underneath, which plays patterns, not motor levels.
 * Each motor in use is one haptic engine for one place on the controller,
 * playing one endless steady event whose intensity is the level.
 *
 * Plain Objective-C with manual retain/release, like the Metal backend.
 */

#if __has_feature(objc_arc)
#error "crinputgamecontroller.m is written for manual retain/release; compile it without -fobjc-arc."
#endif

#include "../../crinputshared.h"

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Standard gravity, in metres per second squared: GameController gives
 * acceleration in multiples of it. */
#define CRINPUT_GC_STANDARD_GRAVITY 9.80665f

/* The motors of CRINPUT_GAMEPAD_RUMBLE, in its order. */
enum {
	CRINPUT_GC_MOTOR_LOW_FREQUENCY = 0,
	CRINPUT_GC_MOTOR_HIGH_FREQUENCY,
	CRINPUT_GC_MOTOR_LEFT_TRIGGER,
	CRINPUT_GC_MOTOR_RIGHT_TRIGGER,
	CRINPUT_GC_MOTOR_COUNT
};

/*
 * Set by a haptic engine's handlers, which CoreHaptics calls on a thread of
 * its own, when the engine has stopped or been reset: whatever was playing
 * no longer is. An object, so that the handlers keep it alive for as long
 * as they may still be called, whatever has happened to the motor.
 */
@interface CrInputHapticFlag : NSObject {
  @public
	atomic_bool stopped;
}
@end

@implementation CrInputHapticFlag
@end

/* One motor of one gamepad. All nil until it is first given a level above
 * 0, and again whenever it is back at 0. */
typedef struct crinput_gc_motor {
	CHHapticEngine *engine;           /* retained */
	id<CHHapticPatternPlayer> player; /* retained */
	CrInputHapticFlag *flag;        /* retained */
	/* As last set. */
	float level;
} crinput_gc_motor;

/* This backend's object for one controller: crinput_gamepad_record's
 * `native`. */
typedef struct crinput_gc_pad {
	GCController *controller; /* retained */

	/* The buttons as of the last change handled or the last poll,
	 * whichever came later, and what has gone down and come up since the
	 * last poll. */
	CRINPUT_GAMEPAD_BUTTONS buttons_handled;
	CRINPUT_GAMEPAD_BUTTONS went_down;
	CRINPUT_GAMEPAD_BUTTONS went_up;

	/* Motion samples that have arrived since the last poll: a ring of
	 * `pending_capacity`, the oldest at `pending_first`. Null until the
	 * sensors are first switched on. */
	CRINPUT_GAMEPAD_MOTION_SAMPLE *pending;
	uint32_t pending_capacity;
	uint32_t pending_first;
	uint32_t pending_count;
	uint32_t pending_dropped;

	crinput_gc_motor motors[CRINPUT_GC_MOTOR_COUNT];
} crinput_gc_pad;

/* ---- Reading a controller ---- */

/* The touch surface button of a controller that has one, or nil. */
static GCControllerButtonInput *crinput_gc_touch_button(GCExtendedGamepad *gamepad) {
	if ([gamepad isKindOfClass:[GCDualSenseGamepad class]])
		return ((GCDualSenseGamepad *)gamepad).touchpadButton;
	if ([gamepad isKindOfClass:[GCDualShockGamepad class]])
		return ((GCDualShockGamepad *)gamepad).touchpadButton;
	return nil;
}

/* The pad that follows one finger on a controller's touch surface, or nil:
 * the first finger down, or the second. */
static GCControllerDirectionPad *crinput_gc_touch_pad(
    GCExtendedGamepad *gamepad,
    uint32_t finger) {
	if ([gamepad isKindOfClass:[GCDualSenseGamepad class]]) {
		GCDualSenseGamepad *sense = (GCDualSenseGamepad *)gamepad;
		return finger == 0 ? sense.touchpadPrimary : sense.touchpadSecondary;
	}
	if ([gamepad isKindOfClass:[GCDualShockGamepad class]]) {
		GCDualShockGamepad *shock = (GCDualShockGamepad *)gamepad;
		return finger == 0 ? shock.touchpadPrimary : shock.touchpadSecondary;
	}
	return nil;
}

/* The element of an extended gamepad that is one button of crinput.h, or
 * nil where this controller has none. */
static GCControllerButtonInput *crinput_gc_button_element(
    GCExtendedGamepad *gamepad,
    CRINPUT_GAMEPAD_BUTTONS button) {
	switch (button) {
	case CRINPUT_GAMEPAD_BUTTON_SOUTH:
		return gamepad.buttonA;
	case CRINPUT_GAMEPAD_BUTTON_EAST:
		return gamepad.buttonB;
	case CRINPUT_GAMEPAD_BUTTON_WEST:
		return gamepad.buttonX;
	case CRINPUT_GAMEPAD_BUTTON_NORTH:
		return gamepad.buttonY;
	case CRINPUT_GAMEPAD_BUTTON_LEFT_SHOULDER:
		return gamepad.leftShoulder;
	case CRINPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER:
		return gamepad.rightShoulder;
	case CRINPUT_GAMEPAD_BUTTON_LEFT_STICK:
		return gamepad.leftThumbstickButton;
	case CRINPUT_GAMEPAD_BUTTON_RIGHT_STICK:
		return gamepad.rightThumbstickButton;
	case CRINPUT_GAMEPAD_BUTTON_BACK:
		return gamepad.buttonOptions;
	case CRINPUT_GAMEPAD_BUTTON_START:
		return gamepad.buttonMenu;
	case CRINPUT_GAMEPAD_BUTTON_GUIDE:
		return gamepad.buttonHome;
	case CRINPUT_GAMEPAD_BUTTON_DPAD_UP:
		return gamepad.dpad.up;
	case CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN:
		return gamepad.dpad.down;
	case CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT:
		return gamepad.dpad.left;
	case CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT:
		return gamepad.dpad.right;
	case CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_0:
		return crinput_gc_touch_button(gamepad);
	default:
		return nil;
	}
}

/* Calls `body` with each button bit crinput.h names, lowest first. */
#define CRINPUT_GC_FOR_EACH_BUTTON(bit) \
	for (CRINPUT_GAMEPAD_BUTTONS bit = 1u; bit <= CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_1; bit <<= 1)

/* The buttons of a controller that are down now. */
static CRINPUT_GAMEPAD_BUTTONS crinput_gc_read_buttons(GCExtendedGamepad *gamepad) {
	CRINPUT_GAMEPAD_BUTTONS buttons = 0;
	CRINPUT_GC_FOR_EACH_BUTTON(bit) {
		/* A message to nil answers NO: a button the controller lacks is
		 * never down. */
		if (crinput_gc_button_element(gamepad, bit).pressed)
			buttons |= bit;
	}
	return buttons;
}

/* The buttons a controller has. */
static CRINPUT_GAMEPAD_BUTTONS crinput_gc_present_buttons(GCExtendedGamepad *gamepad) {
	CRINPUT_GAMEPAD_BUTTONS buttons = 0;
	CRINPUT_GC_FOR_EACH_BUTTON(bit) {
		if (crinput_gc_button_element(gamepad, bit))
			buttons |= bit;
	}
	return buttons;
}

/* Takes a controller's buttons as they are now into what has changed since
 * the last poll. */
static void crinput_gc_note_buttons(
    crinput_gc_pad *pad,
    GCExtendedGamepad *gamepad) {
	const CRINPUT_GAMEPAD_BUTTONS now     = crinput_gc_read_buttons(gamepad);
	const CRINPUT_GAMEPAD_BUTTONS changed = now ^ pad->buttons_handled;
	pad->went_down |= changed & now;
	pad->went_up |= changed & ~now;
	pad->buttons_handled = now;
}

/* Nanoseconds on a clock that only goes forwards. */
static uint64_t crinput_gc_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* Puts one motion sample at the end of a pad's ring, dropping the oldest
 * if it is full. */
static void crinput_gc_push_motion(
    crinput_gc_pad *pad,
    GCMotion *motion) {
	if (!pad->pending)
		return;

	if (pad->pending_count == pad->pending_capacity) {
		pad->pending_first = (pad->pending_first + 1) % pad->pending_capacity;
		--pad->pending_count;
		++pad->pending_dropped;
	}
	CRINPUT_GAMEPAD_MOTION_SAMPLE *sample = &pad->pending[(pad->pending_first + pad->pending_count) % pad->pending_capacity];
	++pad->pending_count;

	/* GameController gives no time for a sample; this is when it reached
	 * this process, which is later and less even than when it was taken. */
	sample->timestamp_ns = crinput_gc_now_ns();

	/* The whole acceleration, gravity included, where the framework has
	 * already split it in two. */
	GCAcceleration acceleration = motion.acceleration;
	if (motion.hasGravityAndUserAcceleration) {
		const GCAcceleration gravity = motion.gravity;
		const GCAcceleration user    = motion.userAcceleration;
		acceleration.x               = gravity.x + user.x;
		acceleration.y               = gravity.y + user.y;
		acceleration.z               = gravity.z + user.z;
	}
	/* Apple's sign is the other way from an accelerometer's reading: at
	 * rest it points the way things fall, where crinput.h has what the
	 * sensor reads, which is away from it. */
	sample->acceleration[0] = (float)-acceleration.x * CRINPUT_GC_STANDARD_GRAVITY;
	sample->acceleration[1] = (float)-acceleration.y * CRINPUT_GC_STANDARD_GRAVITY;
	sample->acceleration[2] = (float)-acceleration.z * CRINPUT_GC_STANDARD_GRAVITY;

	const GCRotationRate rate   = motion.rotationRate;
	sample->angular_velocity[0] = (float)rate.x;
	sample->angular_velocity[1] = (float)rate.y;
	sample->angular_velocity[2] = (float)rate.z;
}

/* ---- Haptics ---- */

/* Where on a controller one motor of crinput.h is, to CoreHaptics. */
static GCHapticsLocality crinput_gc_motor_locality(uint32_t motor) {
	switch (motor) {
	case CRINPUT_GC_MOTOR_LOW_FREQUENCY:
		return GCHapticsLocalityLeftHandle;
	case CRINPUT_GC_MOTOR_HIGH_FREQUENCY:
		return GCHapticsLocalityRightHandle;
	case CRINPUT_GC_MOTOR_LEFT_TRIGGER:
		return GCHapticsLocalityLeftTrigger;
	default:
		return GCHapticsLocalityRightTrigger;
	}
}

/* Stops a motor and lets go of everything it had. */
static void crinput_gc_motor_stop(crinput_gc_motor *motor) {
	if (motor->player)
		[motor->player stopAtTime:CHHapticTimeImmediate error:NULL];
	if (motor->engine) {
		/* The handlers keep the flag alive on their own; nothing of the
		 * motor is reached through them. */
		[motor->engine stopWithCompletionHandler:nil];
	}
	[motor->player release];
	[motor->engine release];
	[motor->flag release];
	motor->player = nil;
	motor->engine = nil;
	motor->flag   = nil;
}

/* Sends a running motor its level. */
static bool crinput_gc_motor_send(
    crinput_gc_motor *motor,
    float level) {
	CHHapticDynamicParameter *intensity = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
	                                                                                      value:level
	                                                                               relativeTime:0];
	NSError *error                      = nil;
	const BOOL sent                     = [motor->player sendParameters:@[ intensity ] atTime:CHHapticTimeImmediate error:&error];
	[intensity release];
	return sent;
}

/* Starts a motor that is stopped: an engine for its place on the
 * controller, playing one steady event with no end. */
static bool crinput_gc_motor_start(
    crinput_gc_motor *motor,
    GCController *controller,
    uint32_t index) {
	CHHapticEngine *engine = [controller.haptics createEngineWithLocality:crinput_gc_motor_locality(index)];
	if (!engine)
		return false;

	CrInputHapticFlag *flag = [[CrInputHapticFlag alloc] init];
	atomic_init(&flag->stopped, false);
	engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
	  (void)reason;
	  atomic_store(&flag->stopped, true);
	};
	engine.resetHandler = ^{
	  atomic_store(&flag->stopped, true);
	};

	motor->engine = [engine retain];
	motor->flag   = flag;

	NSError *error = nil;
	if (![engine startAndReturnError:&error]) {
		crinput_gc_motor_stop(motor);
		return false;
	}

	/* At full intensity; the level is sent as the control on top of it. */
	CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
	CHHapticEvent *event              = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:@[ intensity ]
                                                       relativeTime:0
                                                           duration:GCHapticDurationInfinite];
	CHHapticPattern *pattern          = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
	id<CHHapticPatternPlayer> player  = pattern ? [engine createPlayerWithPattern:pattern error:&error] : nil;
	[pattern release];
	[event release];
	[intensity release];
	if (!player) {
		crinput_gc_motor_stop(motor);
		return false;
	}
	motor->player = [player retain];

	if (![player startAtTime:CHHapticTimeImmediate error:&error]) {
		crinput_gc_motor_stop(motor);
		return false;
	}
	return true;
}

/* Brings one motor to a level, starting or stopping it as that needs. */
static bool crinput_gc_motor_set(
    crinput_gc_motor *motor,
    GCController *controller,
    uint32_t index,
    float level) {
	motor->level = level;
	if (level <= 0.0f) {
		crinput_gc_motor_stop(motor);
		return true;
	}

	/* One the system stopped under us is started afresh. */
	if (motor->flag && atomic_load(&motor->flag->stopped))
		crinput_gc_motor_stop(motor);
	if (!motor->engine) {
		if (!crinput_gc_motor_start(motor, controller, index))
			return false;
	}

	if (!crinput_gc_motor_send(motor, level)) {
		crinput_gc_motor_stop(motor);
		return false;
	}
	return true;
}

/* ---- A controller's record ---- */

/* The record of a controller, or NULL if it has none yet. */
static crinput_gamepad_record *crinput_gc_find(
    crinput_instance instance,
    GCController *controller) {
	for (uint32_t i = 0; i < instance->record_count; ++i) {
		const crinput_gc_pad *pad = (const crinput_gc_pad *)instance->records[i].native;
		if (pad->controller == controller)
			return &instance->records[i];
	}
	return NULL;
}

/* What a controller is: written once, when it is first seen. */
static void crinput_gc_fill_properties(
    CRINPUT_GAMEPAD_PROPERTIES *properties,
    GCController *controller) {
	GCExtendedGamepad *gamepad = controller.extendedGamepad;

	crinput_shared_copy_utf8(properties->name, CRINPUT_GAMEPAD_NAME_SIZE, controller.vendorName.UTF8String);
	/* GameController gives neither number. */
	properties->vendor_id  = 0;
	properties->product_id = 0;

	properties->buttons = crinput_gc_present_buttons(gamepad);
	/* Every extended gamepad has both sticks and both triggers. */
	properties->axes            = (1u << CRINPUT_GAMEPAD_AXIS_COUNT) - 1;
	properties->analog_triggers = gamepad.leftTrigger.isAnalog && gamepad.rightTrigger.isAnalog;

	CRINPUT_GAMEPAD_FEATURES features = CRINPUT_GAMEPAD_FEATURE_LABELS;

	NSSet<GCHapticsLocality> *localities = controller.haptics.supportedLocalities;
	if ([localities containsObject:GCHapticsLocalityLeftHandle])
		features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY;
	if ([localities containsObject:GCHapticsLocalityRightHandle])
		features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY;
	if ([localities containsObject:GCHapticsLocalityLeftTrigger])
		features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER;
	if ([localities containsObject:GCHapticsLocalityRightTrigger])
		features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_RIGHT_TRIGGER;

	if (controller.battery)
		features |= CRINPUT_GAMEPAD_FEATURE_BATTERY;
	if (controller.motion) {
		features |= CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER;
		if (controller.motion.hasRotationRate)
			features |= CRINPUT_GAMEPAD_FEATURE_GYROSCOPE;
	}
	if (controller.light)
		features |= CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR;

	/* GameController takes a player index for any controller and says
	 * nothing of whether there are lights to show it: 1 to 4. */
	features |= CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR;
	properties->player_indicator_max = 4;

	properties->features = features;

	if (crinput_gc_touch_pad(gamepad, 0)) {
		properties->touch_surface_count           = 1;
		properties->touch_surfaces[0].contact_max = 2;
		/* Its size is not reported. */
	}
}

/* One touch surface finger, if it is down. */
static void crinput_gc_read_touch(
    CRINPUT_GAMEPAD_TOUCH_STATE *touch,
    GCExtendedGamepad *gamepad,
    uint32_t finger) {
	GCControllerDirectionPad *pad = crinput_gc_touch_pad(gamepad, finger);
	if (!pad)
		return;

	/* Each runs from -1 to 1 with 0 in the middle, and reads 0, 0 when
	 * nothing touches: GameController has no other way of saying a finger
	 * is down, so one resting exactly on the middle is not seen. */
	const float x = pad.xAxis.value;
	const float y = pad.yAxis.value;
	if (x == 0.0f && y == 0.0f)
		return;

	CRINPUT_GAMEPAD_TOUCH_CONTACT *contact = &touch->contacts[touch->contact_count++];
	contact->id                              = finger;
	contact->x                               = (x + 1.0f) * 0.5f;
	/* Apple's y is up, away from the user; crinput.h's grows towards. */
	contact->y = (1.0f - y) * 0.5f;
}

/* What a controller's power is now. */
static void crinput_gc_read_battery(
    CRINPUT_GAMEPAD_BATTERY *battery,
    GCDeviceBattery *device) {
	memset(battery, 0, sizeof(*battery));
	if (!device)
		return;

	switch (device.batteryState) {
	case GCDeviceBatteryStateDischarging:
		battery->state = CRINPUT_BATTERY_STATE_DISCHARGING;
		break;
	case GCDeviceBatteryStateCharging:
		battery->state = CRINPUT_BATTERY_STATE_CHARGING;
		break;
	case GCDeviceBatteryStateFull:
		battery->state = CRINPUT_BATTERY_STATE_FULL;
		break;
	default:
		battery->state = CRINPUT_BATTERY_STATE_UNKNOWN;
		return;
	}
	/* A fraction, so no steps. */
	battery->level_known = true;
	battery->level       = device.batteryLevel;
}

/* Everything about a controller that a poll reports. */
static void crinput_gc_fill_record(
    crinput_gamepad_record *record,
    GCController *controller) {
	crinput_gc_pad *pad      = (crinput_gc_pad *)record->native;
	GCExtendedGamepad *gamepad = controller.extendedGamepad;

	/* Whatever the change handler has not been told of yet. */
	crinput_gc_note_buttons(pad, gamepad);

	CRINPUT_GAMEPAD_STATE *state = &record->state;
	state->buttons                 = pad->buttons_handled;
	state->buttons_went_down       = pad->went_down;
	state->buttons_went_up         = pad->went_up;
	pad->went_down                 = 0;
	pad->went_up                   = 0;

	/* Apple's sticks are already positive to the right and away from the
	 * user, and its triggers 0 to 1. */
	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X]  = gamepad.leftThumbstick.xAxis.value;
	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y]  = gamepad.leftThumbstick.yAxis.value;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_X] = gamepad.rightThumbstick.xAxis.value;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y] = gamepad.rightThumbstick.yAxis.value;
	state->axes[CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER]  = gamepad.leftTrigger.value;
	state->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER] = gamepad.rightTrigger.value;

	crinput_gc_read_battery(&record->battery, controller.battery);

	memset(&record->touch[0], 0, sizeof(record->touch[0]));
	crinput_gc_read_touch(&record->touch[0], gamepad, 0);
	crinput_gc_read_touch(&record->touch[0], gamepad, 1);

	/* The ring, oldest first, into what the caller is handed. The record's
	 * array is as long as the ring. */
	record->motion_sample_count  = 0;
	record->motion_dropped_count = 0;
	if (record->motion_enabled) {
		for (uint32_t i = 0; i < pad->pending_count; ++i)
			record->motion_samples[i] = pad->pending[(pad->pending_first + i) % pad->pending_capacity];
		record->motion_sample_count  = pad->pending_count;
		record->motion_dropped_count = pad->pending_dropped;
	}
	pad->pending_first   = 0;
	pad->pending_count   = 0;
	pad->pending_dropped = 0;

	/* A motor the system stopped while it should be running is started
	 * again: this is the keeping going that crinput.h asks polls for. */
	for (uint32_t i = 0; i < CRINPUT_GC_MOTOR_COUNT; ++i) {
		crinput_gc_motor *motor = &pad->motors[i];
		if (motor->level > 0.0f && motor->flag && atomic_load(&motor->flag->stopped))
			crinput_gc_motor_set(motor, controller, i, motor->level);
	}
}

/* ---- The backend ---- */

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_create(crinput_instance instance) {
#if TARGET_OS_OSX
	/* The framework's switch is the process's, not an instance's: two
	 * instances that ask for different answers both get the later one. */
	GCController.shouldMonitorBackgroundEvents = instance->desc.read_in_background ? YES : NO;
#else
	/* iOS and iPadOS never let a process in the background read. */
	if (instance->desc.read_in_background)
		return CR_RESULT_INPUT_UNSUPPORTED;
#endif
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_gamecontroller_destroy(crinput_instance instance) { (void)instance; }

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_poll(crinput_instance instance) {
	/* The framework's list and its handlers belong to the main thread. */
	if (![NSThread isMainThread])
		return CR_RESULT_GENERAL_FAILURE;

	@autoreleasepool {
		NSArray<GCController *> *controllers = [GCController controllers];

		uint32_t arrived = 0;
		for (GCController *controller in controllers) {
			if (controller.extendedGamepad && !crinput_gc_find(instance, controller))
				++arrived;
		}

		/* Everything that can fail for want of memory, before any record
		 * is written to: room in the table, then each new one's object. */
		CR_RESULT result = crinput_shared_reserve(instance, arrived);
		if (result != CR_SUCCESS)
			return result;

		for (GCController *controller in controllers) {
			GCExtendedGamepad *gamepad = controller.extendedGamepad;
			if (!gamepad || crinput_gc_find(instance, controller))
				continue;

			crinput_gc_pad *pad = CR_NEW_ZEROED(&instance->head.allocator, crinput_gc_pad);
			if (!pad)
				return CR_RESULT_OUT_OF_MEMORY;
			pad->controller = [controller retain];

			/* Each change of each button as it happens. `pad` is this
			 * file's own and outlives the handler, which is taken off
			 * before it is freed. */
			gamepad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
			  (void)element;
			  crinput_gc_note_buttons(pad, changed);
			};

			crinput_gamepad_record *record = crinput_shared_add(instance, pad);
			crinput_gc_fill_properties(&record->properties, controller);
		}

		for (GCController *controller in controllers) {
			crinput_gamepad_record *record = crinput_gc_find(instance, controller);
			if (!record)
				continue;

			crinput_gc_fill_record(record, controller);
			record->seen = true;
		}
	}
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_gamecontroller_release_gamepad(
    crinput_instance instance,
    crinput_gamepad_record *record) {
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	@autoreleasepool {
		/* Nothing may call into the pad once it is freed. */
		pad->controller.extendedGamepad.valueChangedHandler = nil;
		GCMotion *motion                                    = pad->controller.motion;
		if (motion) {
			motion.valueChangedHandler = nil;
			if (record->motion_enabled && motion.sensorsRequireManualActivation)
				motion.sensorsActive = NO;
		}

		for (uint32_t i = 0; i < CRINPUT_GC_MOTOR_COUNT; ++i)
			crinput_gc_motor_stop(&pad->motors[i]);

		[pad->controller release];
	}
	CR_DELETE_ARRAY(&instance->head.allocator, CRINPUT_GAMEPAD_MOTION_SAMPLE, pad->pending, pad->pending_capacity);
	CR_DELETE(&instance->head.allocator, crinput_gc_pad, pad);
	record->native = NULL;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_set_rumble(
    crinput_instance instance,
    crinput_gamepad_record *record,
    const CRINPUT_GAMEPAD_RUMBLE *rumble) {
	(void)instance;
	/* The framework's controllers and haptics belong to the main thread
	 * (crinput.h), as the poll's do. */
	if (![NSThread isMainThread])
		return CR_RESULT_GENERAL_FAILURE;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	const float levels[CRINPUT_GC_MOTOR_COUNT] = {
	    rumble->low_frequency,
	    rumble->high_frequency,
	    rumble->left_trigger,
	    rumble->right_trigger,
	};

	CR_RESULT result = CR_SUCCESS;
	@autoreleasepool {
		for (uint32_t i = 0; i < CRINPUT_GC_MOTOR_COUNT; ++i) {
			/* A motor the gamepad lacks is at 0 and stays untouched. */
			if (levels[i] <= 0.0f && !pad->motors[i].engine) {
				pad->motors[i].level = 0.0f;
				continue;
			}
			if (!crinput_gc_motor_set(&pad->motors[i], pad->controller, i, levels[i]))
				result = CR_RESULT_API_SPECIFIC_FAILURE;
		}
	}
	return result;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_record *record,
    bool enabled) {
	/* The main thread's, as the poll (crinput.h). */
	if (![NSThread isMainThread])
		return CR_RESULT_GENERAL_FAILURE;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	if (enabled && !pad->pending) {
		pad->pending = CR_NEW_ARRAY_ZEROED(&instance->head.allocator, CRINPUT_GAMEPAD_MOTION_SAMPLE, instance->desc.motion_sample_capacity);
		if (!pad->pending)
			return CR_RESULT_OUT_OF_MEMORY;
		pad->pending_capacity = instance->desc.motion_sample_capacity;
	}

	@autoreleasepool {
		GCMotion *motion = pad->controller.motion;
		if (!enabled) {
			motion.valueChangedHandler = nil;
			if (motion.sensorsRequireManualActivation)
				motion.sensorsActive = NO;
			pad->pending_first   = 0;
			pad->pending_count   = 0;
			pad->pending_dropped = 0;
			return CR_SUCCESS;
		}

		/* As for the buttons: `pad` outlives the handler. */
		motion.valueChangedHandler = ^(GCMotion *changed) {
		  crinput_gc_push_motion(pad, changed);
		};
		if (motion.sensorsRequireManualActivation)
			motion.sensorsActive = YES;
	}
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_set_light_color(
    crinput_instance instance,
    crinput_gamepad_record *record,
    float red,
    float green,
    float blue) {
	(void)instance;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	@autoreleasepool {
		GCColor *color              = [[GCColor alloc] initWithRed:red green:green blue:blue];
		pad->controller.light.color = color;
		[color release];
	}
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_gamecontroller_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_record *record,
    uint32_t player) {
	(void)instance;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	/* GCControllerPlayerIndex1 is 0. */
	pad->controller.playerIndex = player == 0 ? GCControllerPlayerIndexUnset : (GCControllerPlayerIndex)(player - 1);
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_gamecontroller_get_button_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	@autoreleasepool {
		GCControllerButtonInput *element = crinput_gc_button_element(pad->controller.extendedGamepad, button);
		crinput_shared_copy_utf8(out_label, label_size, element.localizedName.UTF8String);
	}
}

CRINPUT_BACKEND_FUNCTION void crinput_gamecontroller_get_axis_label(
    crinput_instance instance,
    crinput_gamepad_record *record,
    CRINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	crinput_gc_pad *pad = (crinput_gc_pad *)record->native;

	@autoreleasepool {
		GCExtendedGamepad *gamepad   = pad->controller.extendedGamepad;
		GCControllerElement *element = nil;
		switch (axis) {
		case CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X:
		case CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y:
			element = gamepad.leftThumbstick;
			break;
		case CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_X:
		case CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y:
			element = gamepad.rightThumbstick;
			break;
		case CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER:
			element = gamepad.leftTrigger;
			break;
		default:
			element = gamepad.rightTrigger;
			break;
		}
		crinput_shared_copy_utf8(out_label, label_size, element.localizedName.UTF8String);
	}
}

#endif // CRINPUT_COMPILE_GAMECONTROLLER

// Only when the GameController API is compiled in (see spudinput.h).
#if SPUDINPUT_COMPILE_GAMECONTROLLER

// Part of src/spudinput.c's translation unit, which includes it; never
// compiled on its own (see src/input/spudinputshared.h).
#ifndef SPUDINPUT_FRONT_END
#error "spudinputgamecontroller.m is included by src/spudinput.c, not compiled on its own."
#endif

/*
 * SpudInput on the GameController framework: macOS, iOS and iPadOS.
 *
 * GameController keeps its own list of controllers and its own picture of
 * each, both brought up to date as the main thread's run loop runs. A poll
 * here reads that list and those pictures; nothing waits and no thread is
 * started. Only a controller with an extended gamepad profile is a gamepad
 * in spudinput.h's sense, and that profile is the whole of the mapping:
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
#error "spudinputgamecontroller.m is written for manual retain/release; compile it without -fobjc-arc."
#endif

#include "../../spudinputshared.h"

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
#define SPUDINPUT_GC_STANDARD_GRAVITY 9.80665f

/* The motors of SPUDINPUT_GAMEPAD_RUMBLE, in its order. */
enum {
	SPUDINPUT_GC_MOTOR_LOW_FREQUENCY = 0,
	SPUDINPUT_GC_MOTOR_HIGH_FREQUENCY,
	SPUDINPUT_GC_MOTOR_LEFT_TRIGGER,
	SPUDINPUT_GC_MOTOR_RIGHT_TRIGGER,
	SPUDINPUT_GC_MOTOR_COUNT
};

/*
 * Set by a haptic engine's handlers, which CoreHaptics calls on a thread of
 * its own, when the engine has stopped or been reset: whatever was playing
 * no longer is. An object, so that the handlers keep it alive for as long
 * as they may still be called, whatever has happened to the motor.
 */
@interface SpudInputHapticFlag : NSObject {
  @public
	atomic_bool stopped;
}
@end

@implementation SpudInputHapticFlag
@end

/* One motor of one gamepad. All nil until it is first given a level above
 * 0, and again whenever it is back at 0. */
typedef struct spudinput_gc_motor {
	CHHapticEngine *engine;           /* retained */
	id<CHHapticPatternPlayer> player; /* retained */
	SpudInputHapticFlag *flag;        /* retained */
	/* As last set. */
	float level;
} spudinput_gc_motor;

/* This backend's object for one controller: spudinput_gamepad_record's
 * `native`. */
typedef struct spudinput_gc_pad {
	GCController *controller; /* retained */

	/* The buttons as of the last change handled or the last poll,
	 * whichever came later, and what has gone down and come up since the
	 * last poll. */
	SPUDINPUT_GAMEPAD_BUTTONS buttons_handled;
	SPUDINPUT_GAMEPAD_BUTTONS went_down;
	SPUDINPUT_GAMEPAD_BUTTONS went_up;

	/* Motion samples that have arrived since the last poll: a ring of
	 * `pending_capacity`, the oldest at `pending_first`. Null until the
	 * sensors are first switched on. */
	SPUDINPUT_GAMEPAD_MOTION_SAMPLE *pending;
	uint32_t pending_capacity;
	uint32_t pending_first;
	uint32_t pending_count;
	uint32_t pending_dropped;

	spudinput_gc_motor motors[SPUDINPUT_GC_MOTOR_COUNT];
} spudinput_gc_pad;

/* ---- Reading a controller ---- */

/* The touch surface button of a controller that has one, or nil. */
static GCControllerButtonInput *spudinput_gc_touch_button(GCExtendedGamepad *gamepad) {
	if ([gamepad isKindOfClass:[GCDualSenseGamepad class]])
		return ((GCDualSenseGamepad *)gamepad).touchpadButton;
	if ([gamepad isKindOfClass:[GCDualShockGamepad class]])
		return ((GCDualShockGamepad *)gamepad).touchpadButton;
	return nil;
}

/* The pad that follows one finger on a controller's touch surface, or nil:
 * the first finger down, or the second. */
static GCControllerDirectionPad *spudinput_gc_touch_pad(
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

/* The element of an extended gamepad that is one button of spudinput.h, or
 * nil where this controller has none. */
static GCControllerButtonInput *spudinput_gc_button_element(
    GCExtendedGamepad *gamepad,
    SPUDINPUT_GAMEPAD_BUTTONS button) {
	switch (button) {
	case SPUDINPUT_GAMEPAD_BUTTON_SOUTH:
		return gamepad.buttonA;
	case SPUDINPUT_GAMEPAD_BUTTON_EAST:
		return gamepad.buttonB;
	case SPUDINPUT_GAMEPAD_BUTTON_WEST:
		return gamepad.buttonX;
	case SPUDINPUT_GAMEPAD_BUTTON_NORTH:
		return gamepad.buttonY;
	case SPUDINPUT_GAMEPAD_BUTTON_LEFT_SHOULDER:
		return gamepad.leftShoulder;
	case SPUDINPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER:
		return gamepad.rightShoulder;
	case SPUDINPUT_GAMEPAD_BUTTON_LEFT_STICK:
		return gamepad.leftThumbstickButton;
	case SPUDINPUT_GAMEPAD_BUTTON_RIGHT_STICK:
		return gamepad.rightThumbstickButton;
	case SPUDINPUT_GAMEPAD_BUTTON_BACK:
		return gamepad.buttonOptions;
	case SPUDINPUT_GAMEPAD_BUTTON_START:
		return gamepad.buttonMenu;
	case SPUDINPUT_GAMEPAD_BUTTON_GUIDE:
		return gamepad.buttonHome;
	case SPUDINPUT_GAMEPAD_BUTTON_DPAD_UP:
		return gamepad.dpad.up;
	case SPUDINPUT_GAMEPAD_BUTTON_DPAD_DOWN:
		return gamepad.dpad.down;
	case SPUDINPUT_GAMEPAD_BUTTON_DPAD_LEFT:
		return gamepad.dpad.left;
	case SPUDINPUT_GAMEPAD_BUTTON_DPAD_RIGHT:
		return gamepad.dpad.right;
	case SPUDINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_0:
		return spudinput_gc_touch_button(gamepad);
	default:
		return nil;
	}
}

/* Calls `body` with each button bit spudinput.h names, lowest first. */
#define SPUDINPUT_GC_FOR_EACH_BUTTON(bit) \
	for (SPUDINPUT_GAMEPAD_BUTTONS bit = 1u; bit <= SPUDINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_1; bit <<= 1)

/* The buttons of a controller that are down now. */
static SPUDINPUT_GAMEPAD_BUTTONS spudinput_gc_read_buttons(GCExtendedGamepad *gamepad) {
	SPUDINPUT_GAMEPAD_BUTTONS buttons = 0;
	SPUDINPUT_GC_FOR_EACH_BUTTON(bit) {
		/* A message to nil answers NO: a button the controller lacks is
		 * never down. */
		if (spudinput_gc_button_element(gamepad, bit).pressed)
			buttons |= bit;
	}
	return buttons;
}

/* The buttons a controller has. */
static SPUDINPUT_GAMEPAD_BUTTONS spudinput_gc_present_buttons(GCExtendedGamepad *gamepad) {
	SPUDINPUT_GAMEPAD_BUTTONS buttons = 0;
	SPUDINPUT_GC_FOR_EACH_BUTTON(bit) {
		if (spudinput_gc_button_element(gamepad, bit))
			buttons |= bit;
	}
	return buttons;
}

/* Takes a controller's buttons as they are now into what has changed since
 * the last poll. */
static void spudinput_gc_note_buttons(
    spudinput_gc_pad *pad,
    GCExtendedGamepad *gamepad) {
	const SPUDINPUT_GAMEPAD_BUTTONS now     = spudinput_gc_read_buttons(gamepad);
	const SPUDINPUT_GAMEPAD_BUTTONS changed = now ^ pad->buttons_handled;
	pad->went_down |= changed & now;
	pad->went_up |= changed & ~now;
	pad->buttons_handled = now;
}

/* Nanoseconds on a clock that only goes forwards. */
static uint64_t spudinput_gc_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* Puts one motion sample at the end of a pad's ring, dropping the oldest
 * if it is full. */
static void spudinput_gc_push_motion(
    spudinput_gc_pad *pad,
    GCMotion *motion) {
	if (!pad->pending)
		return;

	if (pad->pending_count == pad->pending_capacity) {
		pad->pending_first = (pad->pending_first + 1) % pad->pending_capacity;
		--pad->pending_count;
		++pad->pending_dropped;
	}
	SPUDINPUT_GAMEPAD_MOTION_SAMPLE *sample = &pad->pending[(pad->pending_first + pad->pending_count) % pad->pending_capacity];
	++pad->pending_count;

	/* GameController gives no time for a sample; this is when it reached
	 * this process, which is later and less even than when it was taken. */
	sample->timestamp_ns = spudinput_gc_now_ns();

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
	 * rest it points the way things fall, where spudinput.h has what the
	 * sensor reads, which is away from it. */
	sample->acceleration[0] = (float)-acceleration.x * SPUDINPUT_GC_STANDARD_GRAVITY;
	sample->acceleration[1] = (float)-acceleration.y * SPUDINPUT_GC_STANDARD_GRAVITY;
	sample->acceleration[2] = (float)-acceleration.z * SPUDINPUT_GC_STANDARD_GRAVITY;

	const GCRotationRate rate   = motion.rotationRate;
	sample->angular_velocity[0] = (float)rate.x;
	sample->angular_velocity[1] = (float)rate.y;
	sample->angular_velocity[2] = (float)rate.z;
}

/* ---- Haptics ---- */

/* Where on a controller one motor of spudinput.h is, to CoreHaptics. */
static GCHapticsLocality spudinput_gc_motor_locality(uint32_t motor) {
	switch (motor) {
	case SPUDINPUT_GC_MOTOR_LOW_FREQUENCY:
		return GCHapticsLocalityLeftHandle;
	case SPUDINPUT_GC_MOTOR_HIGH_FREQUENCY:
		return GCHapticsLocalityRightHandle;
	case SPUDINPUT_GC_MOTOR_LEFT_TRIGGER:
		return GCHapticsLocalityLeftTrigger;
	default:
		return GCHapticsLocalityRightTrigger;
	}
}

/* Stops a motor and lets go of everything it had. */
static void spudinput_gc_motor_stop(spudinput_gc_motor *motor) {
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
static bool spudinput_gc_motor_send(
    spudinput_gc_motor *motor,
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
static bool spudinput_gc_motor_start(
    spudinput_gc_motor *motor,
    GCController *controller,
    uint32_t index) {
	CHHapticEngine *engine = [controller.haptics createEngineWithLocality:spudinput_gc_motor_locality(index)];
	if (!engine)
		return false;

	SpudInputHapticFlag *flag = [[SpudInputHapticFlag alloc] init];
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
		spudinput_gc_motor_stop(motor);
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
		spudinput_gc_motor_stop(motor);
		return false;
	}
	motor->player = [player retain];

	if (![player startAtTime:CHHapticTimeImmediate error:&error]) {
		spudinput_gc_motor_stop(motor);
		return false;
	}
	return true;
}

/* Brings one motor to a level, starting or stopping it as that needs. */
static bool spudinput_gc_motor_set(
    spudinput_gc_motor *motor,
    GCController *controller,
    uint32_t index,
    float level) {
	motor->level = level;
	if (level <= 0.0f) {
		spudinput_gc_motor_stop(motor);
		return true;
	}

	/* One the system stopped under us is started afresh. */
	if (motor->flag && atomic_load(&motor->flag->stopped))
		spudinput_gc_motor_stop(motor);
	if (!motor->engine) {
		if (!spudinput_gc_motor_start(motor, controller, index))
			return false;
	}

	if (!spudinput_gc_motor_send(motor, level)) {
		spudinput_gc_motor_stop(motor);
		return false;
	}
	return true;
}

/* ---- A controller's record ---- */

/* The record of a controller, or NULL if it has none yet. */
static spudinput_gamepad_record *spudinput_gc_find(
    spudinput_instance instance,
    GCController *controller) {
	for (uint32_t i = 0; i < instance->record_count; ++i) {
		const spudinput_gc_pad *pad = (const spudinput_gc_pad *)instance->records[i].native;
		if (pad->controller == controller)
			return &instance->records[i];
	}
	return NULL;
}

/* What a controller is: written once, when it is first seen. */
static void spudinput_gc_fill_properties(
    SPUDINPUT_GAMEPAD_PROPERTIES *properties,
    GCController *controller) {
	GCExtendedGamepad *gamepad = controller.extendedGamepad;

	spudinput_shared_copy_utf8(properties->name, SPUDINPUT_GAMEPAD_NAME_SIZE, controller.vendorName.UTF8String);
	/* GameController gives neither number. */
	properties->vendor_id  = 0;
	properties->product_id = 0;

	properties->buttons = spudinput_gc_present_buttons(gamepad);
	/* Every extended gamepad has both sticks and both triggers. */
	properties->axes            = (1u << SPUDINPUT_GAMEPAD_AXIS_COUNT) - 1;
	properties->analog_triggers = gamepad.leftTrigger.isAnalog && gamepad.rightTrigger.isAnalog;

	SPUDINPUT_GAMEPAD_FEATURES features = SPUDINPUT_GAMEPAD_FEATURE_LABELS;

	NSSet<GCHapticsLocality> *localities = controller.haptics.supportedLocalities;
	if ([localities containsObject:GCHapticsLocalityLeftHandle])
		features |= SPUDINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY;
	if ([localities containsObject:GCHapticsLocalityRightHandle])
		features |= SPUDINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY;
	if ([localities containsObject:GCHapticsLocalityLeftTrigger])
		features |= SPUDINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER;
	if ([localities containsObject:GCHapticsLocalityRightTrigger])
		features |= SPUDINPUT_GAMEPAD_FEATURE_RUMBLE_RIGHT_TRIGGER;

	if (controller.battery)
		features |= SPUDINPUT_GAMEPAD_FEATURE_BATTERY;
	if (controller.motion) {
		features |= SPUDINPUT_GAMEPAD_FEATURE_ACCELEROMETER;
		if (controller.motion.hasRotationRate)
			features |= SPUDINPUT_GAMEPAD_FEATURE_GYROSCOPE;
	}
	if (controller.light)
		features |= SPUDINPUT_GAMEPAD_FEATURE_LIGHT_COLOR;

	/* GameController takes a player index for any controller and says
	 * nothing of whether there are lights to show it: 1 to 4. */
	features |= SPUDINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR;
	properties->player_indicator_max = 4;

	properties->features = features;

	if (spudinput_gc_touch_pad(gamepad, 0)) {
		properties->touch_surface_count           = 1;
		properties->touch_surfaces[0].contact_max = 2;
		/* Its size is not reported. */
	}
}

/* One touch surface finger, if it is down. */
static void spudinput_gc_read_touch(
    SPUDINPUT_GAMEPAD_TOUCH_STATE *touch,
    GCExtendedGamepad *gamepad,
    uint32_t finger) {
	GCControllerDirectionPad *pad = spudinput_gc_touch_pad(gamepad, finger);
	if (!pad)
		return;

	/* Each runs from -1 to 1 with 0 in the middle, and reads 0, 0 when
	 * nothing touches: GameController has no other way of saying a finger
	 * is down, so one resting exactly on the middle is not seen. */
	const float x = pad.xAxis.value;
	const float y = pad.yAxis.value;
	if (x == 0.0f && y == 0.0f)
		return;

	SPUDINPUT_GAMEPAD_TOUCH_CONTACT *contact = &touch->contacts[touch->contact_count++];
	contact->id                              = finger;
	contact->x                               = (x + 1.0f) * 0.5f;
	/* Apple's y is up, away from the user; spudinput.h's grows towards. */
	contact->y = (1.0f - y) * 0.5f;
}

/* What a controller's power is now. */
static void spudinput_gc_read_battery(
    SPUDINPUT_GAMEPAD_BATTERY *battery,
    GCDeviceBattery *device) {
	memset(battery, 0, sizeof(*battery));
	if (!device)
		return;

	switch (device.batteryState) {
	case GCDeviceBatteryStateDischarging:
		battery->state = SPUDINPUT_BATTERY_STATE_DISCHARGING;
		break;
	case GCDeviceBatteryStateCharging:
		battery->state = SPUDINPUT_BATTERY_STATE_CHARGING;
		break;
	case GCDeviceBatteryStateFull:
		battery->state = SPUDINPUT_BATTERY_STATE_FULL;
		break;
	default:
		battery->state = SPUDINPUT_BATTERY_STATE_UNKNOWN;
		return;
	}
	/* A fraction, so no steps. */
	battery->level_known = true;
	battery->level       = device.batteryLevel;
}

/* Everything about a controller that a poll reports. */
static void spudinput_gc_fill_record(
    spudinput_gamepad_record *record,
    GCController *controller) {
	spudinput_gc_pad *pad      = (spudinput_gc_pad *)record->native;
	GCExtendedGamepad *gamepad = controller.extendedGamepad;

	/* Whatever the change handler has not been told of yet. */
	spudinput_gc_note_buttons(pad, gamepad);

	SPUDINPUT_GAMEPAD_STATE *state = &record->state;
	state->buttons                 = pad->buttons_handled;
	state->buttons_went_down       = pad->went_down;
	state->buttons_went_up         = pad->went_up;
	pad->went_down                 = 0;
	pad->went_up                   = 0;

	/* Apple's sticks are already positive to the right and away from the
	 * user, and its triggers 0 to 1. */
	state->axes[SPUDINPUT_GAMEPAD_AXIS_LEFT_STICK_X]  = gamepad.leftThumbstick.xAxis.value;
	state->axes[SPUDINPUT_GAMEPAD_AXIS_LEFT_STICK_Y]  = gamepad.leftThumbstick.yAxis.value;
	state->axes[SPUDINPUT_GAMEPAD_AXIS_RIGHT_STICK_X] = gamepad.rightThumbstick.xAxis.value;
	state->axes[SPUDINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y] = gamepad.rightThumbstick.yAxis.value;
	state->axes[SPUDINPUT_GAMEPAD_AXIS_LEFT_TRIGGER]  = gamepad.leftTrigger.value;
	state->axes[SPUDINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER] = gamepad.rightTrigger.value;

	spudinput_gc_read_battery(&record->battery, controller.battery);

	memset(&record->touch[0], 0, sizeof(record->touch[0]));
	spudinput_gc_read_touch(&record->touch[0], gamepad, 0);
	spudinput_gc_read_touch(&record->touch[0], gamepad, 1);

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
	 * again: this is the keeping going that spudinput.h asks polls for. */
	for (uint32_t i = 0; i < SPUDINPUT_GC_MOTOR_COUNT; ++i) {
		spudinput_gc_motor *motor = &pad->motors[i];
		if (motor->level > 0.0f && motor->flag && atomic_load(&motor->flag->stopped))
			spudinput_gc_motor_set(motor, controller, i, motor->level);
	}
}

/* ---- The backend ---- */

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_create(spudinput_instance instance) {
#if TARGET_OS_OSX
	/* The framework's switch is the process's, not an instance's: two
	 * instances that ask for different answers both get the later one. */
	GCController.shouldMonitorBackgroundEvents = instance->desc.read_in_background ? YES : NO;
#else
	/* iOS and iPadOS never let a process in the background read. */
	if (instance->desc.read_in_background)
		return SPUDRESULT_SPUDINPUT_UNSUPPORTED;
#endif
	return SPUD_SUCCESS;
}

SPUDINPUT_BACKEND_FUNCTION void spudinput_gamecontroller_destroy(spudinput_instance instance) { (void)instance; }

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_poll(spudinput_instance instance) {
	/* The framework's list and its handlers belong to the main thread. */
	if (![NSThread isMainThread])
		return SPUDRESULT_GENERAL_FAILURE;

	@autoreleasepool {
		NSArray<GCController *> *controllers = [GCController controllers];

		uint32_t arrived = 0;
		for (GCController *controller in controllers) {
			if (controller.extendedGamepad && !spudinput_gc_find(instance, controller))
				++arrived;
		}

		/* Everything that can fail for want of memory, before any record
		 * is written to: room in the table, then each new one's object. */
		SPUDRESULT result = spudinput_shared_reserve(instance, arrived);
		if (result != SPUD_SUCCESS)
			return result;

		for (GCController *controller in controllers) {
			GCExtendedGamepad *gamepad = controller.extendedGamepad;
			if (!gamepad || spudinput_gc_find(instance, controller))
				continue;

			spudinput_gc_pad *pad = SPUD_NEW_ZEROED(&instance->head.allocator, spudinput_gc_pad);
			if (!pad)
				return SPUDRESULT_OUT_OF_MEMORY;
			pad->controller = [controller retain];

			/* Each change of each button as it happens. `pad` is this
			 * file's own and outlives the handler, which is taken off
			 * before it is freed. */
			gamepad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
			  (void)element;
			  spudinput_gc_note_buttons(pad, changed);
			};

			spudinput_gamepad_record *record = spudinput_shared_add(instance, pad);
			spudinput_gc_fill_properties(&record->properties, controller);
		}

		for (GCController *controller in controllers) {
			spudinput_gamepad_record *record = spudinput_gc_find(instance, controller);
			if (!record)
				continue;

			spudinput_gc_fill_record(record, controller);
			record->seen = true;
		}
	}
	return SPUD_SUCCESS;
}

SPUDINPUT_BACKEND_FUNCTION void spudinput_gamecontroller_release_gamepad(
    spudinput_instance instance,
    spudinput_gamepad_record *record) {
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	@autoreleasepool {
		/* Nothing may call into the pad once it is freed. */
		pad->controller.extendedGamepad.valueChangedHandler = nil;
		GCMotion *motion                                    = pad->controller.motion;
		if (motion) {
			motion.valueChangedHandler = nil;
			if (record->motion_enabled && motion.sensorsRequireManualActivation)
				motion.sensorsActive = NO;
		}

		for (uint32_t i = 0; i < SPUDINPUT_GC_MOTOR_COUNT; ++i)
			spudinput_gc_motor_stop(&pad->motors[i]);

		[pad->controller release];
	}
	SPUD_DELETE_ARRAY(&instance->head.allocator, SPUDINPUT_GAMEPAD_MOTION_SAMPLE, pad->pending, pad->pending_capacity);
	SPUD_DELETE(&instance->head.allocator, spudinput_gc_pad, pad);
	record->native = NULL;
}

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_set_rumble(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    const SPUDINPUT_GAMEPAD_RUMBLE *rumble) {
	(void)instance;
	/* The framework's controllers and haptics belong to the main thread
	 * (spudinput.h), as the poll's do. */
	if (![NSThread isMainThread])
		return SPUDRESULT_GENERAL_FAILURE;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	const float levels[SPUDINPUT_GC_MOTOR_COUNT] = {
	    rumble->low_frequency,
	    rumble->high_frequency,
	    rumble->left_trigger,
	    rumble->right_trigger,
	};

	SPUDRESULT result = SPUD_SUCCESS;
	@autoreleasepool {
		for (uint32_t i = 0; i < SPUDINPUT_GC_MOTOR_COUNT; ++i) {
			/* A motor the gamepad lacks is at 0 and stays untouched. */
			if (levels[i] <= 0.0f && !pad->motors[i].engine) {
				pad->motors[i].level = 0.0f;
				continue;
			}
			if (!spudinput_gc_motor_set(&pad->motors[i], pad->controller, i, levels[i]))
				result = SPUDRESULT_API_SPECIFIC_FAILURE;
		}
	}
	return result;
}

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_set_motion_enabled(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    bool enabled) {
	/* The main thread's, as the poll (spudinput.h). */
	if (![NSThread isMainThread])
		return SPUDRESULT_GENERAL_FAILURE;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	if (enabled && !pad->pending) {
		pad->pending = SPUD_NEW_ARRAY_ZEROED(&instance->head.allocator, SPUDINPUT_GAMEPAD_MOTION_SAMPLE, instance->desc.motion_sample_capacity);
		if (!pad->pending)
			return SPUDRESULT_OUT_OF_MEMORY;
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
			return SPUD_SUCCESS;
		}

		/* As for the buttons: `pad` outlives the handler. */
		motion.valueChangedHandler = ^(GCMotion *changed) {
		  spudinput_gc_push_motion(pad, changed);
		};
		if (motion.sensorsRequireManualActivation)
			motion.sensorsActive = YES;
	}
	return SPUD_SUCCESS;
}

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_set_light_color(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    float red,
    float green,
    float blue) {
	(void)instance;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	@autoreleasepool {
		GCColor *color              = [[GCColor alloc] initWithRed:red green:green blue:blue];
		pad->controller.light.color = color;
		[color release];
	}
	return SPUD_SUCCESS;
}

SPUDINPUT_BACKEND_FUNCTION SPUDRESULT spudinput_gamecontroller_set_player_indicator(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    uint32_t player) {
	(void)instance;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	/* GCControllerPlayerIndex1 is 0. */
	pad->controller.playerIndex = player == 0 ? GCControllerPlayerIndexUnset : (GCControllerPlayerIndex)(player - 1);
	return SPUD_SUCCESS;
}

SPUDINPUT_BACKEND_FUNCTION void spudinput_gamecontroller_get_button_label(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    SPUDINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	@autoreleasepool {
		GCControllerButtonInput *element = spudinput_gc_button_element(pad->controller.extendedGamepad, button);
		spudinput_shared_copy_utf8(out_label, label_size, element.localizedName.UTF8String);
	}
}

SPUDINPUT_BACKEND_FUNCTION void spudinput_gamecontroller_get_axis_label(
    spudinput_instance instance,
    spudinput_gamepad_record *record,
    SPUDINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size) {
	(void)instance;
	spudinput_gc_pad *pad = (spudinput_gc_pad *)record->native;

	@autoreleasepool {
		GCExtendedGamepad *gamepad   = pad->controller.extendedGamepad;
		GCControllerElement *element = nil;
		switch (axis) {
		case SPUDINPUT_GAMEPAD_AXIS_LEFT_STICK_X:
		case SPUDINPUT_GAMEPAD_AXIS_LEFT_STICK_Y:
			element = gamepad.leftThumbstick;
			break;
		case SPUDINPUT_GAMEPAD_AXIS_RIGHT_STICK_X:
		case SPUDINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y:
			element = gamepad.rightThumbstick;
			break;
		case SPUDINPUT_GAMEPAD_AXIS_LEFT_TRIGGER:
			element = gamepad.leftTrigger;
			break;
		default:
			element = gamepad.rightTrigger;
			break;
		}
		spudinput_shared_copy_utf8(out_label, label_size, element.localizedName.UTF8String);
	}
}

#endif // SPUDINPUT_COMPILE_GAMECONTROLLER

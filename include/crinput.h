/**
 * @file crinput.h
 * @brief Carrottus Input: input devices read directly, with no window. Gamepads
 * only so far. Known problems: the GameController (Apple), evdev (Linux)
 * and GameInput (Windows) backends are written; only GameController has
 * run with a gamepad being used, and only its sticks, triggers and one
 * button; XInput is not written.
 *
 * The overview is under @ref crinput "Carrottus Input". Its result codes are
 * `CR_RESULT_CRINPUT_*`, in crcore.h.
 */

#ifndef CARROTTUS_CRINPUT_H
#define CARROTTUS_CRINPUT_H

#include "crcore.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @defgroup crinput Carrottus Input
 * @brief Input devices the platform lets a process read directly, with no
 * window (GameController / GameInput / XInput / evdev): which are
 * connected, what each is, and what state each is in. Gamepads only so far.
 * Known problems: only the GameController, evdev and GameInput backends
 * are written.
 * GameController has read a gamepad held and used on macOS, through a
 * caller polling once a frame (on the process's main thread, which
 * GameController requires): found when connected,
 * both sticks and both triggers read in the directions and ranges stated
 * here, and a button (the left stick, pressed in) read down and up. Its
 * other buttons one by one, the rumble, battery, motion, touch surfaces,
 * lights and labels have not been run. On it, a
 * finger resting exactly on the middle of a touch surface is not seen, a
 * motion sample's time is when it reached the process and not when it was
 * taken, and the direction of the motion axes has not been checked against
 * a device. evdev has found a gamepad and read its properties and its state
 * at rest, and its battery and lights have run only against copies of what
 * drivers put in sysfs; it gives no gamepad motion sensors, touch surfaces
 * or labels; a light usually answers CR_RESULT_INPUT_ACCESS_DENIED,
 * since sysfs LEDs are root's to write unless the system says otherwise;
 * and its face buttons are where the kernel driver says they are, which for
 * xpad (Xbox) puts the left face button at CRINPUT_GAMEPAD_BUTTON_NORTH
 * and the top one at WEST. GameInput has found an Xbox gamepad and read
 * its properties and its state at rest; it gives no motion sensors, touch
 * surfaces, lights or labels, and no name for that gamepad.
 *
 * A translation of the platform's device API, nothing more.
 *
 * What is not here, and never will be: anything that reaches a process as
 * an event of a window. Key presses, the pointer, the cursor, typed text
 * and input methods belong to whoever owns the window, and Carrottus owns
 * none. A keyboard or a mouse read as a bare device, where a platform
 * allows it, would belong here; none is yet.
 *
 * What is here is left as the platform gives it:
 *
 * - Never picks a device and never numbers players. Every gamepad the
 *   platform presents is listed; which one is whose is the caller's.
 * - Never shapes a value. No dead zone, no response curve, no smoothing and
 *   no key repeat: a stick at rest reads whatever the hardware reads, which
 *   is seldom exactly 0. The only arithmetic is bringing each platform's
 *   range and direction to the one this header states.
 * - Never maps a device the platform doesn't call a gamepad. Each platform
 *   has its own notion of a standard gamepad (GCExtendedGamepad, the
 *   XInput and GameInput gamepad, the Linux kernel's gamepad button and
 *   axis codes), and a device is listed when the platform presents it that
 *   way. There is no table of other devices' layouts here; a joystick or a
 *   wheel the platform shows only as raw buttons and axes is not listed.
 * - Never starts a thread and never calls back. Everything is read when the
 *   caller asks: crinput_instance_poll() takes what the platform has,
 *   within the call, and the other calls answer from that until the next
 *   poll.
 *
 * The same shape as Carrottus Net's: names are `crinput_<object>_<verb>`, an
 * instance is made by its `_create` and released by its `_destroy`, and a
 * desc has no defaults.
 *
 * A gamepad is named by a value, a crinput_gamepad_id, not by a handle:
 * a device can be unplugged at any moment, and a value that has stopped
 * naming anything is told apart from one that still does, which a pointer
 * could not be.
 *
 * What a gamepad has beyond buttons and axes differs from one device to the
 * next and from one platform to the next, so each such thing is a bit in
 * CRINPUT_GAMEPAD_PROPERTIES::features, found when the gamepad is first
 * seen. Asking a gamepad for one it doesn't have is
 * CR_RESULT_INPUT_FEATURE_NOT_PRESENT: nothing is quietly left undone.
 *
 * The outputs (rumble, lights) are set and held: a call says how things are
 * to be from now on, and Carrottus Input keeps no time. How long a motor runs and
 * what pattern it makes are the caller's, by calling again.
 *
 * Not here: haptics beyond motor levels (resisting triggers, played
 * waveforms), a gamepad's speaker and microphone (those are audio
 * endpoints), and any picture of a button.
 *
 * @par Threads
 * The library rule holds (@ref cr_threads "Threads", crcore.h): an
 * instance is used by one caller at a time, and every call on one of its
 * gamepads is a use of the instance, since a crinput_gamepad_id is a
 * value and not an object of its own. What is particular to Carrottus Input is
 * marked `Thread:` where it applies: GameController's poll is confined to
 * the process's main thread, and GameInput may call the instance's
 * allocator on a thread of its own.
 */

/**
 * @defgroup crinput_config Compile-time configuration
 * @ingroup crinput
 * @brief Macros that say which native input APIs are built in. Known
 * problem: CRINPUT_COMPILE_XINPUT is never 1; its backend is not
 * written. With none, no CRINPUT_NATIVE_API is accepted
 * and no instance can be made.
 *
 * carrottus's CMake defines a `CRINPUT_COMPILE_*` for each native input API
 * built in, as PUBLIC definitions - so everything that includes this header
 * sees the same values and knows at compile time which CRINPUT_NATIVE_API
 * values crinput_instance_create() accepts, rather than finding
 * CR_RESULT_INVALID_API at runtime. Anything not defined is 0.
 *
 * | Platform                       | Native API macro                                         |
 * |--------------------------------|----------------------------------------------------------|
 * | Windows                        | CRINPUT_COMPILE_GAMEINPUT, CRINPUT_COMPILE_XINPUT    |
 * | Linux                          | CRINPUT_COMPILE_EVDEV                                  |
 * | macOS, iOS and iPadOS          | CRINPUT_COMPILE_GAMECONTROLLER                         |
 * @{
 */

#ifndef CRINPUT_COMPILE_GAMECONTROLLER
/** @brief 1 when the GameController backend (Apple) is built in, 0 otherwise. Defined by the build. */
#define CRINPUT_COMPILE_GAMECONTROLLER 0
#endif
#ifndef CRINPUT_COMPILE_GAMEINPUT
/** @brief 1 when the GameInput backend (Windows) is built in, 0 otherwise. Defined by the build. */
#define CRINPUT_COMPILE_GAMEINPUT 0
#endif
#ifndef CRINPUT_COMPILE_XINPUT
/** @brief 1 when the XInput backend (Windows) is built in, 0 otherwise. Defined by the build. */
#define CRINPUT_COMPILE_XINPUT 0
#endif
#ifndef CRINPUT_COMPILE_EVDEV
/** @brief 1 when the evdev backend (Linux) is built in, 0 otherwise. Defined by the build. */
#define CRINPUT_COMPILE_EVDEV 0
#endif

/** @} */

/**
 * @defgroup crinput_instances Instance
 * @ingroup crinput
 * @brief Carrottus Input bound to one native input API, and the poll that reads
 * it.
 * @{
 */

/** @brief Opaque handle to Carrottus Input bound to one native input API. */
typedef struct crinput_instance_t *crinput_instance;

/** @brief Which native input API an instance is bound to. */
typedef uint32_t CRINPUT_NATIVE_API;
/** @brief Values of CRINPUT_NATIVE_API. */
enum {
	CRINPUT_NATIVE_API_NONE           = 0, /**< No API; what a NULL instance reports. */
	CRINPUT_NATIVE_API_GAMECONTROLLER = 1, /**< The GameController framework (macOS, iOS, iPadOS). */
	CRINPUT_NATIVE_API_GAMEINPUT      = 2, /**< GameInput (Windows). */
	/** XInput (Windows). It has four fixed slots, no device names and no
	 * vendor or product numbers. */
	CRINPUT_NATIVE_API_XINPUT         = 3,
	CRINPUT_NATIVE_API_EVDEV          = 4  /**< The kernel's event devices, /dev/input/event* (Linux). */
};

/**
 * @brief What the caller decides when creating an instance. No field has a
 * default: a zeroed desc is refused, for its `base`, and one with only its
 * `base` set is refused for its `allocator`.
 */
typedef struct CRINPUT_INSTANCE_DESC {
	/** `type` is CR_STRUCT_TYPE_INPUT_INSTANCE_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** Where the instance's memory comes from (CR_ALLOCATOR). Copied, so
	 * the struct need not outlive the call; its functions and `user_data`
	 * must outlive the instance.
	 *
	 * Thread: called only while one of this module's calls on the instance
	 * is running, though not always on that call's thread: GameInput may
	 * run the device callback that allocates on a thread of its own. */
	const CR_ALLOCATOR *allocator;
	/** The native input API to bind to. One this build has
	 * (@ref crinput_config "Compile-time configuration"). */
	CRINPUT_NATIVE_API native_api;
	/**
	 * Whether devices are to be read while the process is not the one the
	 * user is working in. Passed to the platform where the platform makes
	 * it a choice, and refused where it does not:
	 *
	 * | Native API     | false                              | true                               |
	 * |----------------|------------------------------------|------------------------------------|
	 * | GameController | Read only while the app is active  | Read always (macOS); refused on iOS and iPadOS, which never allow it |
	 * | GameInput      | Read only while in focus           | Read always                        |
	 * | XInput         | Refused: it always reads           | Read always                        |
	 * | evdev          | Refused: it always reads           | Read always                        |
	 *
	 * A refusal is CR_RESULT_INPUT_UNSUPPORTED. Nothing is emulated: an
	 * API that always reads is not made to look as though it stops.
	 *
	 * GameController's switch on macOS is the whole process's, not one
	 * instance's: of two instances that choose differently, both get what
	 * the one created later chose.
	 */
	bool read_in_background;
	/**
	 * How many motion samples are kept for each gamepad between two polls
	 * (@ref crinput_motion "Motion sensors"). Sensors report several
	 * hundred times a second, so what is enough depends on how often the
	 * caller polls, which only the caller knows. When more arrive than
	 * this, the oldest are dropped and counted. 0 keeps none, and
	 * crinput_gamepad_set_motion_enabled() then refuses to switch any
	 * sensor on.
	 */
	uint32_t motion_sample_capacity;
} CRINPUT_INSTANCE_DESC;

/**
 * @brief Creates an instance bound to one native input API.
 *
 * No device is listed until the first crinput_instance_poll().
 *
 * @param[in]  desc         What the caller decides.
 * @param[out] out_instance Receives the instance; NULL on failure.
 *
 * @retval CR_SUCCESS The instance was created.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_INPUT_INSTANCE_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_instance` is NULL.
 * @retval CR_RESULT_NULL_ALLOCATOR `desc->allocator` is NULL.
 * @retval CR_RESULT_INVALID_ALLOCATOR `desc->allocator` lacks a function.
 * @retval CR_RESULT_INVALID_API `desc->native_api` is not one this build
 *         has.
 * @retval CR_RESULT_INPUT_UNSUPPORTED `desc->read_in_background` is a
 *         choice the native API doesn't offer.
 * @retval CR_RESULT_OUT_OF_MEMORY The instance could not be allocated.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API would not
 *         start.
 */
CR_RESULT crinput_instance_create(
    const CRINPUT_INSTANCE_DESC *desc,
    crinput_instance *out_instance);

/**
 * @brief Destroys an instance. Every crinput_gamepad_id it gave out stops
 * naming anything.
 *
 * @param[in] instance The instance to destroy; NULL does nothing.
 */
void crinput_instance_destroy(crinput_instance instance);

/**
 * @brief Returns the native API an instance is bound to.
 *
 * @param[in] instance The instance to ask.
 *
 * @return The instance's native API.
 * @retval CRINPUT_NATIVE_API_NONE `instance` is NULL.
 */
CRINPUT_NATIVE_API crinput_instance_get_native_api(crinput_instance instance);

/**
 * @brief Takes what the platform has: which devices have arrived and gone
 * since the last poll, and the state of each that is connected.
 *
 * Never waits. Everything the other calls report is as of the last poll and
 * does not change between two of them, so a caller that polls once a frame
 * reads one consistent picture for that frame.
 *
 * A use of the instance, like every other call on it
 * (@ref cr_threads "Threads").
 *
 * Thread: with CRINPUT_NATIVE_API_GAMECONTROLLER, only on the process's
 * main thread, which GameController requires: its list of controllers and
 * its change handlers belong to the main thread, and a device arrives only
 * while the main thread's run loop is being run, which is the caller's to
 * do. A poll made on any other thread returns CR_RESULT_GENERAL_FAILURE
 * and reads nothing. Other native APIs take it from any thread.
 *
 * @param[in] instance The instance to poll.
 *
 * @retval CR_SUCCESS The poll was made. That no device is connected is
 *         not a failure.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY A list could not be grown; what the
 *         calls report is as it was before the poll, and the motion samples
 *         that had arrived are kept for the next.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API failed.
 * @retval CR_RESULT_GENERAL_FAILURE With
 *         CRINPUT_NATIVE_API_GAMECONTROLLER, the call was not made on the
 *         main thread; nothing was read.
 */
CR_RESULT crinput_instance_poll(crinput_instance instance);

/** @} */

/**
 * @defgroup crinput_gamepads Gamepads
 * @ingroup crinput
 * @brief Which gamepads are connected, what each is and what state each is
 * in.
 * @{
 */

/**
 * @brief Names one connection of one gamepad to one instance.
 *
 * Given out when a poll first sees the device, and never given out again by
 * that instance: a gamepad unplugged and plugged back in has a new one,
 * since no platform says for certain that it is the same device. A value
 * that names nothing - never given out, or of a gamepad that has gone -
 * gets CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED.
 */
typedef uint64_t crinput_gamepad_id;

/** @brief The crinput_gamepad_id that never names a gamepad. */
#define CRINPUT_GAMEPAD_ID_NONE ((crinput_gamepad_id)0)

/**
 * @brief Lists the gamepads connected as of the last poll.
 *
 * @param[in]  instance          The instance to ask.
 * @param[out] out_gamepads      Receives an array of ids, owned by the
 *                               instance, in the order the gamepads were
 *                               first seen. Valid until the next
 *                               crinput_instance_poll() or until the
 *                               instance is destroyed. NULL when the count
 *                               is 0.
 * @param[out] out_gamepad_count Receives the number of ids in
 *                               `out_gamepads`.
 *
 * @retval CR_SUCCESS The list was returned.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_gamepads` or
 *         `out_gamepad_count` is NULL.
 */
CR_RESULT crinput_instance_get_gamepads(
    crinput_instance instance,
    const crinput_gamepad_id **out_gamepads,
    uint32_t *out_gamepad_count);

/**
 * @brief One button of a gamepad, as a bit.
 *
 * The four face buttons are named by where they are, not by what is printed
 * on them, which differs from one maker to the next: the south button is A
 * on an Xbox gamepad, cross on a PlayStation one and B on a Nintendo one.
 *
 * The two triggers are not here. They are axes
 * (CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER), on every gamepad, whether or not
 * the hardware's are analogue.
 */
typedef uint32_t CRINPUT_GAMEPAD_BUTTONS;
/** @brief Bits of CRINPUT_GAMEPAD_BUTTONS. */
enum {
	CRINPUT_GAMEPAD_BUTTON_SOUTH          = 1u << 0,  /**< The lowest face button. */
	CRINPUT_GAMEPAD_BUTTON_EAST           = 1u << 1,  /**< The rightmost face button. */
	CRINPUT_GAMEPAD_BUTTON_WEST           = 1u << 2,  /**< The leftmost face button. */
	CRINPUT_GAMEPAD_BUTTON_NORTH          = 1u << 3,  /**< The highest face button. */
	CRINPUT_GAMEPAD_BUTTON_LEFT_SHOULDER  = 1u << 4,  /**< The button above the left trigger. */
	CRINPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER = 1u << 5,  /**< The button above the right trigger. */
	CRINPUT_GAMEPAD_BUTTON_LEFT_STICK     = 1u << 6,  /**< The left stick, pressed in. */
	CRINPUT_GAMEPAD_BUTTON_RIGHT_STICK    = 1u << 7,  /**< The right stick, pressed in. */
	/** The centre button on the left: Back, View, Share, Select, Create. */
	CRINPUT_GAMEPAD_BUTTON_BACK           = 1u << 8,
	/** The centre button on the right: Start, Menu, Options. */
	CRINPUT_GAMEPAD_BUTTON_START          = 1u << 9,
	/** The maker's own button: Xbox, PS, Home. Many platforms keep it for
	 * themselves and never report it. */
	CRINPUT_GAMEPAD_BUTTON_GUIDE          = 1u << 10,
	CRINPUT_GAMEPAD_BUTTON_DPAD_UP        = 1u << 11, /**< The directional pad, up. */
	CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN      = 1u << 12, /**< The directional pad, down. */
	CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT      = 1u << 13, /**< The directional pad, left. */
	CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT     = 1u << 14, /**< The directional pad, right. */
	/** The first touch surface, pressed in until it clicks
	 * (@ref crinput_touch "Touch surfaces"). */
	CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_0 = 1u << 15,
	/** The second touch surface, pressed in until it clicks. */
	CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_1 = 1u << 16
};

/** @brief One axis of a gamepad: an index into CRINPUT_GAMEPAD_STATE::axes. */
typedef uint32_t CRINPUT_GAMEPAD_AXIS;
/** @brief Values of CRINPUT_GAMEPAD_AXIS. */
enum {
	CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X  = 0, /**< -1 fully left to +1 fully right. */
	CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y  = 1, /**< -1 fully towards the user to +1 fully away. */
	CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_X = 2, /**< -1 fully left to +1 fully right. */
	CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y = 3, /**< -1 fully towards the user to +1 fully away. */
	CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER  = 4, /**< 0 let go to 1 fully pulled. */
	CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER = 5  /**< 0 let go to 1 fully pulled. */
};
/** @brief Number of axes a gamepad has: the length of CRINPUT_GAMEPAD_STATE::axes. */
#define CRINPUT_GAMEPAD_AXIS_COUNT 6

/** @brief Bytes in CRINPUT_GAMEPAD_PROPERTIES::name, its terminator included. */
#define CRINPUT_GAMEPAD_NAME_SIZE 128

/**
 * @brief What a gamepad has beyond buttons and axes, as bits. Each is a fact
 * about one device on one platform: the same gamepad may have a feature
 * through one native API and not through another.
 */
typedef uint32_t CRINPUT_GAMEPAD_FEATURES;
/** @brief Bits of CRINPUT_GAMEPAD_FEATURES. */
enum {
	/** A motor for slow, heavy shaking: CRINPUT_GAMEPAD_RUMBLE::low_frequency. */
	CRINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY  = 1u << 0,
	/** A motor for fast, light buzzing: CRINPUT_GAMEPAD_RUMBLE::high_frequency. */
	CRINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY = 1u << 1,
	/** A motor in the left trigger: CRINPUT_GAMEPAD_RUMBLE::left_trigger. */
	CRINPUT_GAMEPAD_FEATURE_RUMBLE_LEFT_TRIGGER   = 1u << 2,
	/** A motor in the right trigger: CRINPUT_GAMEPAD_RUMBLE::right_trigger. */
	CRINPUT_GAMEPAD_FEATURE_RUMBLE_RIGHT_TRIGGER  = 1u << 3,
	/** The platform reports its power: crinput_gamepad_get_battery(). */
	CRINPUT_GAMEPAD_FEATURE_BATTERY               = 1u << 4,
	/** An accelerometer: CRINPUT_GAMEPAD_MOTION_SAMPLE::acceleration. */
	CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER         = 1u << 5,
	/** A gyroscope: CRINPUT_GAMEPAD_MOTION_SAMPLE::angular_velocity. */
	CRINPUT_GAMEPAD_FEATURE_GYROSCOPE             = 1u << 6,
	/** A light whose colour can be set: crinput_gamepad_set_light_color(). */
	CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR           = 1u << 7,
	/** Lights that show a player's number, which can be set:
	 * crinput_gamepad_set_player_indicator(). Not set where the platform
	 * numbers them itself and lets no process change it (XInput). */
	CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR      = 1u << 8,
	/** The platform has words for this gamepad's buttons and axes:
	 * crinput_gamepad_get_button_label(). */
	CRINPUT_GAMEPAD_FEATURE_LABELS                = 1u << 9
};

/** @brief The most touch surfaces a gamepad is reported with: the length of
 * CRINPUT_GAMEPAD_PROPERTIES::touch_surfaces. */
#define CRINPUT_GAMEPAD_TOUCH_SURFACE_MAX 2

/** @brief What one touch surface of a gamepad is. */
/* Frozen: held by value in CRINPUT_GAMEPAD_PROPERTIES. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_TOUCH_SURFACE {
	/** How many fingers it follows at once; no more than
	 * CRINPUT_GAMEPAD_TOUCH_CONTACT_MAX. */
	uint32_t contact_max;
	/** Its width in millimetres, left to right as the gamepad is held; 0
	 * where the platform doesn't say. With `height_mm`, what turns a
	 * position on it into a distance. */
	float width_mm;
	/** Its height in millimetres, far edge to near edge; 0 where the
	 * platform doesn't say. */
	float height_mm;
} CRINPUT_GAMEPAD_TOUCH_SURFACE;

/**
 * @brief What a gamepad is. It does not change while the gamepad stays
 * connected.
 */
typedef struct CRINPUT_GAMEPAD_PROPERTIES {
	/** `type` is CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES; `next` is NULL (nothing is chained onto
	 * this struct yet). The caller builds the chain and sets each type; Carrottus Input fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The platform's name for the device, UTF-8, null-terminated, cut
	 * short at a whole character if it is longer than fits. Empty where
	 * the platform has none (XInput, and GameInput for some devices). */
	char name[CRINPUT_GAMEPAD_NAME_SIZE];
	/** The device's USB or Bluetooth vendor number; 0 where the platform
	 * doesn't say (XInput, GameController). */
	uint16_t vendor_id;
	/** The device's product number; 0 where the platform doesn't say. */
	uint16_t product_id;
	/** The buttons the platform says this gamepad has. A button not in it
	 * never reads as down. */
	CRINPUT_GAMEPAD_BUTTONS buttons;
	/** The axes the platform says this gamepad has, bit `1u << axis` for
	 * each CRINPUT_GAMEPAD_AXIS. An axis not in it always reads 0. */
	uint32_t axes;
	/** Whether each trigger reports how far it is pulled. Where false,
	 * its axis is only ever 0 or 1. */
	bool analog_triggers;
	/** What else the gamepad has. */
	CRINPUT_GAMEPAD_FEATURES features;
	/** The greatest number crinput_gamepad_set_player_indicator() can
	 * show; 0 without CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR. */
	uint32_t player_indicator_max;
	/** How many touch surfaces it has; 0 for none. Where there are two,
	 * surface 0 is the left one. */
	uint32_t touch_surface_count;
	/** What each of them is; the entries from `touch_surface_count` on are
	 * zeroed. */
	CRINPUT_GAMEPAD_TOUCH_SURFACE touch_surfaces[CRINPUT_GAMEPAD_TOUCH_SURFACE_MAX];
} CRINPUT_GAMEPAD_PROPERTIES;

/**
 * @brief Reports what a gamepad is.
 *
 * @param[in]  instance       The instance that gave out `gamepad`.
 * @param[in]  gamepad        The gamepad to ask about.
 * @param[out] out_properties Receives the properties.
 *
 * @retval CR_SUCCESS The properties were written.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_properties->base.next` is
 *         not NULL.
 */
CR_RESULT crinput_gamepad_get_properties(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_PROPERTIES *out_properties);

/**
 * @brief The state of a gamepad as of the last poll.
 */
/* Frozen: read every frame; a new button is a free bit of CRINPUT_GAMEPAD_BUTTONS. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_STATE {
	/** The buttons that are down. */
	CRINPUT_GAMEPAD_BUTTONS buttons;
	/**
	 * The buttons that went down at some moment between the poll before
	 * the last and the last, whether or not they are still down. On the
	 * first poll that sees the gamepad, the ones that are down.
	 *
	 * Where the platform reports each change (GameInput, evdev,
	 * GameController), a button pressed and let go between two polls is in
	 * this and in `buttons_went_up` and not in `buttons`. Where it reports
	 * only how things stand (XInput), these two are the difference between
	 * the two polls and such a press is not seen at all.
	 */
	CRINPUT_GAMEPAD_BUTTONS buttons_went_down;
	/** The buttons that came up at some moment in the same time, whether
	 * or not they are down again. */
	CRINPUT_GAMEPAD_BUTTONS buttons_went_up;
	/**
	 * Each axis, indexed by CRINPUT_GAMEPAD_AXIS, in the range and
	 * direction that enum states, and otherwise as the hardware reads: no
	 * dead zone is taken out, so a stick at rest is near 0 and seldom 0.
	 */
	float axes[CRINPUT_GAMEPAD_AXIS_COUNT];
} CRINPUT_GAMEPAD_STATE;

/**
 * @brief Reads the state of a gamepad as of the last poll.
 *
 * Reading it changes nothing: two reads between the same two polls give the
 * same state, `buttons_went_down` and `buttons_went_up` included.
 *
 * @param[in]  instance  The instance that gave out `gamepad`.
 * @param[in]  gamepad   The gamepad to read.
 * @param[out] out_state Receives the state.
 *
 * @retval CR_SUCCESS The state was written.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_state` is NULL.
 */
CR_RESULT crinput_gamepad_get_state(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_STATE *out_state);

/** @} */

/**
 * @defgroup crinput_rumble Rumble
 * @ingroup crinput
 * @brief How hard each of a gamepad's motors runs.
 *
 * The platforms differ underneath: XInput and GameInput take motor levels,
 * evdev takes an effect that is uploaded and played, and GameController
 * takes haptic patterns. The header says none of that. One rule holds on
 * all of them: a level is set and stays until it is set again.
 * @{
 */

/**
 * @brief The longest a caller may go between two polls while a motor runs,
 * in milliseconds. What a running motor does once longer than this has
 * passed without a crinput_instance_poll() is undefined: some platforms
 * keep it running and some need the poll to.
 */
#define CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS 1000

/**
 * @brief A level for each motor a gamepad can have: 0 is stopped, 1 is as
 * hard as it goes. What a level in between feels like is the device's.
 */
/* Frozen: a value; a new kind of motor is a new feature bit and a new call. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_RUMBLE {
	/** The motor for slow, heavy shaking, usually in the left grip. */
	float low_frequency;
	/** The motor for fast, light buzzing, usually in the right grip. */
	float high_frequency;
	/** The motor in the left trigger. */
	float left_trigger;
	/** The motor in the right trigger. */
	float right_trigger;
} CRINPUT_GAMEPAD_RUMBLE;

/**
 * @brief Sets how hard each motor of a gamepad runs, from now until it is
 * set again.
 *
 * Takes effect at once, not at the next poll. The motors stop when the
 * gamepad goes and when the instance is destroyed, and are stopped when a
 * gamepad is first seen.
 *
 * Two rules hold on every backend, whatever the platform underneath does:
 *
 * - Keep polling while a motor runs, at least every
 *   CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS.
 * - What the motors do while the instance is not being let read the
 *   gamepad (CRINPUT_INSTANCE_DESC::read_in_background false, and the
 *   process not the one in use) is undefined. A caller that wants them
 *   stopped then sets them to 0 before it happens.
 *
 * @param[in] instance The instance that gave out `gamepad`.
 * @param[in] gamepad  The gamepad.
 * @param[in] rumble   The levels. All four 0 stops every motor.
 *
 * Thread: with CRINPUT_NATIVE_API_GAMECONTROLLER, only on the process's
 * main thread, as crinput_poll: the framework's controllers and haptics
 * belong to it. A call on any other thread returns
 * CR_RESULT_GENERAL_FAILURE and changes nothing. Other native APIs take
 * it from any thread.
 *
 * @retval CR_SUCCESS The levels were set.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_NULL_DESC `rumble` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS A level is below 0, above 1 or
 *         not a number.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT A level other than 0 is
 *         for a motor the gamepad doesn't have
 *         (CRINPUT_GAMEPAD_PROPERTIES::features); no motor was changed.
 * @retval CR_RESULT_INPUT_ACCESS_DENIED The platform won't let this
 *         process write to the device.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API failed.
 * @retval CR_RESULT_GENERAL_FAILURE On GameController, called off the
 *         main thread.
 */
CR_RESULT crinput_gamepad_set_rumble(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    const CRINPUT_GAMEPAD_RUMBLE *rumble);

/** @} */

/**
 * @defgroup crinput_battery Battery
 * @ingroup crinput
 * @brief How a gamepad is powered and how much charge it has.
 * @{
 */

/** @brief How a gamepad is powered. */
typedef uint32_t CRINPUT_BATTERY_STATE;
/** @brief Values of CRINPUT_BATTERY_STATE. */
enum {
	CRINPUT_BATTERY_STATE_UNKNOWN     = 0, /**< The platform doesn't say. */
	CRINPUT_BATTERY_STATE_NONE        = 1, /**< It has no battery: it runs from its cable. */
	CRINPUT_BATTERY_STATE_DISCHARGING = 2, /**< Running from its battery. */
	CRINPUT_BATTERY_STATE_CHARGING    = 3, /**< On power, and its battery is filling. */
	CRINPUT_BATTERY_STATE_FULL        = 4  /**< On power, and its battery is full. */
};

/** @brief A gamepad's power as of the last poll. */
/* Frozen: a value, read as of the last poll. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_BATTERY {
	/** How it is powered. */
	CRINPUT_BATTERY_STATE state;
	/** Whether `level` means anything. False with
	 * CRINPUT_BATTERY_STATE_NONE, and wherever the platform gives a
	 * state and no level. */
	bool level_known;
	/** How full the battery is: 0 empty to 1 full. 0 when `level_known` is
	 * false. */
	float level;
	/**
	 * How many different values the platform can report for `level`,
	 * evenly spread from 0 to 1; 0 where it reports a fraction or a
	 * percentage. XInput has 4 (empty, low, medium, full), which read as
	 * 0, 1/3, 2/3 and 1: a level of 1/3 there means "low", not a third.
	 */
	uint32_t level_steps;
} CRINPUT_GAMEPAD_BATTERY;

/**
 * @brief Reads a gamepad's power as of the last poll.
 *
 * @param[in]  instance    The instance that gave out `gamepad`.
 * @param[in]  gamepad     The gamepad to ask about.
 * @param[out] out_battery Receives its power.
 *
 * @retval CR_SUCCESS The power was written.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has no
 *         CRINPUT_GAMEPAD_FEATURE_BATTERY.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_battery` is NULL.
 */
CR_RESULT crinput_gamepad_get_battery(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_BATTERY *out_battery);

/** @} */

/**
 * @defgroup crinput_motion Motion sensors
 * @ingroup crinput
 * @brief What a gamepad's accelerometer and gyroscope read, sample by
 * sample.
 *
 * Sensors are off until the caller switches them on: they cost the gamepad
 * power, and some platforms don't run them unasked.
 *
 * They report far more often than a caller polls, and what is done with a
 * gyroscope's readings is adding them up, so every reading matters and the
 * last one alone is no use. A poll therefore takes every sample that
 * arrived since the poll before, up to
 * CRINPUT_INSTANCE_DESC::motion_sample_capacity, and
 * crinput_gamepad_get_motion() hands them out in order.
 *
 * Readings are in the gamepad's own frame, the same on every platform. With
 * the gamepad held level, its buttons facing up: +X is to the user's right,
 * +Y is up out of its face, +Z is towards the user. It is right-handed.
 *
 * They are what the sensors read, with nothing worked out from them: no
 * orientation, no gravity taken out, no drift corrected, no filter.
 * @{
 */

/** @brief One reading of a gamepad's motion sensors. */
/* Frozen: an element of an array Carrottus Input owns and hands out, read every frame. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_MOTION_SAMPLE {
	/**
	 * When the reading was taken, in nanoseconds. Only the difference
	 * between two samples of one gamepad means anything: it is the time
	 * between them. It is on no clock the caller can read.
	 */
	uint64_t timestamp_ns;
	/**
	 * What the accelerometer reads on X, Y and Z, in metres per second
	 * squared, gravity included: a gamepad lying still and level reads
	 * about +9.81 on Y. All 0 without
	 * CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER.
	 */
	float acceleration[3];
	/**
	 * How fast the gamepad is turning about X, Y and Z, in radians per
	 * second, positive by the right-hand rule: turning it to the left, as
	 * seen from above, is positive about Y. All 0 without
	 * CRINPUT_GAMEPAD_FEATURE_GYROSCOPE.
	 */
	float angular_velocity[3];
} CRINPUT_GAMEPAD_MOTION_SAMPLE;

/**
 * @brief Switches a gamepad's motion sensors on or off. They are off when a
 * gamepad is first seen.
 *
 * Both sensors together: no platform switches one alone. Samples start
 * arriving with a later poll, not at once.
 *
 * @param[in] instance The instance that gave out `gamepad`.
 * @param[in] gamepad  The gamepad.
 * @param[in] enabled  true for on, false for off. Setting what already
 *                     holds succeeds and changes nothing.
 *
 * Thread: with CRINPUT_NATIVE_API_GAMECONTROLLER, only on the process's
 * main thread, as crinput_poll: the framework's controllers belong to
 * it. A call on any other thread returns CR_RESULT_GENERAL_FAILURE and
 * changes nothing. Other native APIs take it from any thread.
 *
 * @retval CR_SUCCESS The sensors are as asked.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has neither
 *         CRINPUT_GAMEPAD_FEATURE_ACCELEROMETER nor
 *         CRINPUT_GAMEPAD_FEATURE_GYROSCOPE.
 * @retval CR_RESULT_ZERO_SIZE `enabled` is true and the instance was made
 *         with a `motion_sample_capacity` of 0.
 * @retval CR_RESULT_INPUT_ACCESS_DENIED The platform won't let this
 *         process read the sensors.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API failed.
 * @retval CR_RESULT_GENERAL_FAILURE On GameController, called off the
 *         main thread.
 */
CR_RESULT crinput_gamepad_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    bool enabled);

/**
 * @brief Hands out the motion samples a gamepad's sensors gave between the
 * poll before the last and the last.
 *
 * Reading them changes nothing: two calls between the same two polls give
 * the same samples. The next poll replaces them.
 *
 * @param[in]  instance          The instance that gave out `gamepad`.
 * @param[in]  gamepad           The gamepad to read.
 * @param[out] out_samples       Receives an array of samples, owned by the
 *                               instance, oldest first. Valid until the
 *                               next crinput_instance_poll() or until
 *                               the instance is destroyed. NULL when the
 *                               count is 0.
 * @param[out] out_sample_count  Receives the number of samples in
 *                               `out_samples`; 0 while the sensors are off.
 * @param[out] out_dropped_count Receives how many samples arrived in the
 *                               same time and were dropped for want of
 *                               room. They were older than every one in
 *                               `out_samples`.
 *
 * @retval CR_SUCCESS The samples were handed out.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has neither
 *         motion sensor.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_samples`,
 *         `out_sample_count` or `out_dropped_count` is NULL.
 */
CR_RESULT crinput_gamepad_get_motion(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    const CRINPUT_GAMEPAD_MOTION_SAMPLE **out_samples,
    uint32_t *out_sample_count,
    uint32_t *out_dropped_count);

/** @} */

/**
 * @defgroup crinput_touch Touch surfaces
 * @ingroup crinput
 * @brief Where fingers are on a gamepad's touch surfaces.
 *
 * A surface that clicks when pressed in reports the click as a button
 * (CRINPUT_GAMEPAD_BUTTON_TOUCH_SURFACE_0), in the gamepad's state. What
 * is here is only where the fingers are, as of the last poll: a finger put
 * down and lifted between two polls is not seen.
 *
 * Nothing is made of the positions: no taps, no swipes, no pointer.
 * @{
 */

/** @brief The most fingers reported on one touch surface: the length of
 * CRINPUT_GAMEPAD_TOUCH_STATE::contacts. */
#define CRINPUT_GAMEPAD_TOUCH_CONTACT_MAX 4

/** @brief One finger on a touch surface. */
/* Frozen: held by value in CRINPUT_GAMEPAD_TOUCH_STATE. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_TOUCH_CONTACT {
	/**
	 * Tells this finger from the others on the same surface, and stays the
	 * same for as long as it stays down. Once it lifts, the value may be
	 * given to the next finger. Where the platform numbers fingers only by
	 * the order they came down (GameController), that number.
	 */
	uint32_t id;
	/** Where it is across the surface: 0 at the left edge to 1 at the
	 * right, as the gamepad is held. */
	float x;
	/** Where it is along the surface: 0 at the edge away from the user to
	 * 1 at the edge nearest. */
	float y;
} CRINPUT_GAMEPAD_TOUCH_CONTACT;

/** @brief The fingers on one touch surface as of the last poll. */
/* Frozen: read every frame. It never changes; a new need gets a new type. */
typedef struct CRINPUT_GAMEPAD_TOUCH_STATE {
	/** How many fingers are down; no more than the surface's
	 * `contact_max`. */
	uint32_t contact_count;
	/** Each of them, in no stated order; the entries from `contact_count`
	 * on are zeroed. */
	CRINPUT_GAMEPAD_TOUCH_CONTACT contacts[CRINPUT_GAMEPAD_TOUCH_CONTACT_MAX];
} CRINPUT_GAMEPAD_TOUCH_STATE;

/**
 * @brief Reads the fingers on one touch surface of a gamepad as of the last
 * poll.
 *
 * @param[in]  instance  The instance that gave out `gamepad`.
 * @param[in]  gamepad   The gamepad to read.
 * @param[in]  surface   Which surface, from 0.
 * @param[out] out_touch Receives the fingers.
 *
 * @retval CR_SUCCESS The fingers were written.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `surface` is not below the
 *         gamepad's `touch_surface_count`, which covers a gamepad with
 *         none.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_touch` is NULL.
 */
CR_RESULT crinput_gamepad_get_touch(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    uint32_t surface,
    CRINPUT_GAMEPAD_TOUCH_STATE *out_touch);

/** @} */

/**
 * @defgroup crinput_lights Lights
 * @ingroup crinput
 * @brief A gamepad's coloured light and its player-number lights.
 *
 * Carrottus Input sets neither on its own. What a gamepad shows before the caller
 * sets it is the platform's doing, and a platform may number a gamepad's
 * lights itself. Each is set and held, like rumble, and what a gamepad
 * shows after the instance is destroyed is the platform's again.
 * @{
 */

/**
 * @brief Sets the colour of a gamepad's light, from now until it is set
 * again.
 *
 * The three numbers are passed to the platform as they are. What colour and
 * how bright the light then is are the device's; all three 0 is off.
 *
 * @param[in] instance The instance that gave out `gamepad`.
 * @param[in] gamepad  The gamepad.
 * @param[in] red      0 to 1.
 * @param[in] green    0 to 1.
 * @param[in] blue     0 to 1.
 *
 * @retval CR_SUCCESS The colour was set.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has no
 *         CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS A number is below 0, above 1
 *         or not a number.
 * @retval CR_RESULT_INPUT_ACCESS_DENIED The platform won't let this
 *         process write to the device.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API failed.
 */
CR_RESULT crinput_gamepad_set_light_color(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    float red,
    float green,
    float blue);

/**
 * @brief Sets which player's number a gamepad's lights show, from now until
 * it is set again.
 *
 * Which number goes with which gamepad is the caller's: nothing here ties
 * it to the order gamepads are listed in.
 *
 * @param[in] instance The instance that gave out `gamepad`.
 * @param[in] gamepad  The gamepad.
 * @param[in] player   The number to show, from 1; 0 shows none.
 *
 * @retval CR_SUCCESS The number was set.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has no
 *         CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `player` is above the gamepad's
 *         `player_indicator_max`.
 * @retval CR_RESULT_INPUT_ACCESS_DENIED The platform won't let this
 *         process write to the device.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform's API failed.
 */
CR_RESULT crinput_gamepad_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    uint32_t player);

/** @} */

/**
 * @defgroup crinput_labels Labels
 * @ingroup crinput
 * @brief The platform's own words for a gamepad's buttons and axes.
 *
 * Only what the platform says, passed on: "Cross Button", "L2". Where it
 * says nothing the label is empty, and nothing is worked out from the
 * maker's number or the device's name to fill it in. No table of gamepads
 * is kept here, and no picture of a button: both are the caller's.
 *
 * A label is in the language the platform is set to, for a person to read.
 * It is not a name to compare against.
 * @{
 */

/**
 * @brief Copies out the platform's label for one button of a gamepad.
 *
 * @param[in]  instance   The instance that gave out `gamepad`.
 * @param[in]  gamepad    The gamepad to ask about.
 * @param[in]  button     The button: exactly one bit of
 *                        CRINPUT_GAMEPAD_BUTTONS.
 * @param[out] out_label  Receives the label, UTF-8, null-terminated, cut
 *                        short at a whole character if it is longer than
 *                        fits. Empty where the platform has none for this
 *                        button.
 * @param[in]  label_size Bytes `out_label` has room for, the terminator
 *                        included.
 *
 * @retval CR_SUCCESS The label was written, empty or not.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has no
 *         CRINPUT_GAMEPAD_FEATURE_LABELS.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `button` is not exactly one
 *         bit this header names.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_label` is NULL.
 * @retval CR_RESULT_ZERO_SIZE `label_size` is 0.
 */
CR_RESULT crinput_gamepad_get_button_label(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_BUTTONS button,
    char *out_label,
    uint32_t label_size);

/**
 * @brief Copies out the platform's label for the control one axis of a
 * gamepad belongs to: a stick, for either of its two axes, or a trigger.
 *
 * @param[in]  instance   The instance that gave out `gamepad`.
 * @param[in]  gamepad    The gamepad to ask about.
 * @param[in]  axis       The axis.
 * @param[out] out_label  Receives the label, as in
 *                        crinput_gamepad_get_button_label().
 * @param[in]  label_size Bytes `out_label` has room for, the terminator
 *                        included.
 *
 * @retval CR_SUCCESS The label was written, empty or not.
 * @retval CR_RESULT_INPUT_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED `gamepad` names
 *         nothing as of the last poll.
 * @retval CR_RESULT_INPUT_FEATURE_NOT_PRESENT The gamepad has no
 *         CRINPUT_GAMEPAD_FEATURE_LABELS.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `axis` is not below
 *         CRINPUT_GAMEPAD_AXIS_COUNT.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_label` is NULL.
 * @retval CR_RESULT_ZERO_SIZE `label_size` is 0.
 */
CR_RESULT crinput_gamepad_get_axis_label(
    crinput_instance instance,
    crinput_gamepad_id gamepad,
    CRINPUT_GAMEPAD_AXIS axis,
    char *out_label,
    uint32_t label_size);

/** @} */

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CARROTTUS_CRINPUT_H

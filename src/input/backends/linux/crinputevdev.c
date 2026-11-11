// Only when the evdev API is compiled in (see crinput.h).
#if CRINPUT_COMPILE_EVDEV

// Part of src/crinput.c's translation unit, which includes it; never
// compiled on its own (see src/input/crinputshared.h).
#ifndef CRINPUT_FRONT_END
#error "crinputevdev.c is included by src/crinput.c, not compiled on its own."
#endif

/*
 * Carrottus Input on the kernel's event devices: Linux.
 *
 * Each gamepad is one /dev/input/event* node, opened by this file and read
 * without waiting. A device is a gamepad when the kernel gives it
 * BTN_GAMEPAD (BTN_SOUTH), and the kernel's gamepad codes
 * (Documentation/input/gamepad.rst) are the whole of the mapping: there is
 * no table of devices in this file.
 *
 * Which nodes there are is learnt from inotify on /dev/input, read at each
 * poll: a new node, or one whose permissions change (udev gives the user
 * access after it creates the node), makes the poll look through the
 * directory again. A device that has gone is seen when reading it fails
 * with ENODEV. Nothing waits and no thread is started.
 *
 * Every change the kernel reports is taken, not only how things stand at
 * the poll, so a press shorter than the time between two polls is still in
 * buttons_went_down and buttons_went_up. When the kernel's buffer
 * overflowed (SYN_DROPPED), the device's state is read again whole.
 *
 * The kernel's codes, as this file reads them:
 * - Sticks: ABS_X and ABS_Y, ABS_RX and ABS_RY. Its y grows towards the
 *   user; crinput.h's grows away.
 * - Triggers: ABS_HAT2Y (left) and ABS_HAT2X (right), as gamepad.rst has
 *   them, else ABS_Z and ABS_RZ, which is where the common drivers (xpad,
 *   hid-playstation) put them; else BTN_TL2 and BTN_TR2, read as 0 or 1.
 * - D-pad: BTN_DPAD_*, and ABS_HAT0X and ABS_HAT0Y, whichever the device
 *   has.
 * - Face buttons by their kernel names, BTN_SOUTH, BTN_EAST, BTN_NORTH and
 *   BTN_WEST, as the driver reports them.
 *
 * Rumble is FF_RUMBLE: one effect per gamepad, uploaded with the two motor
 * levels and played. Each play lasts CRINPUT_EVDEV_RUMBLE_LENGTH_MS, and
 * a poll plays it again once CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS has gone
 * by, so that a motor runs for as long as the polls keep coming and stops
 * soon after they don't.
 *
 * The battery and the lights are not in the event node: the driver gives
 * them in sysfs, under the device the node belongs to (the node's input
 * device, or the HID device or USB interface above it), and this file
 * looks in both when it first opens a gamepad. What it finds there then is
 * what the gamepad has: a battery or a light the driver registers later is
 * not seen.
 *
 * - Battery: the first power_supply there of type Battery. Its files are
 *   kept open and read again at each poll: status, and capacity (a
 *   percentage) or else capacity_level (five steps from Critical to Full).
 * - Light colour: a multicolor LED with red, green and blue channels (its
 *   multi_intensity, then its brightness at the most), else three LEDs whose
 *   names give the colours red, green and blue.
 * - Player indicator: the LEDs whose names end in player-1, player-2 and so
 *   on, by the kernel's LED naming (devicename:color:function). Player n
 *   lights the LED named player-n alone; the greatest n is the last of an
 *   unbroken run from player-1.
 *
 * LED files are root's to write unless a udev rule says otherwise, so
 * setting a light usually fails with CR_RESULT_INPUT_ACCESS_DENIED.
 *
 * Not written: motion sensors and touch surfaces, which the kernel gives as
 * sibling event nodes of the same device; no gamepad here has them. The
 * kernel has no labels.
 */

#include "../../crinputshared.h"

#include <linux/input.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Where the event nodes are. */
#define CRINPUT_EVDEV_DIRECTORY "/dev/input"

/* Where sysfs has each event node's devices: /sys/class/input/eventN. */
#define CRINPUT_EVDEV_SYSFS_INPUT "/sys/class/input"

/* The most player LEDs looked for: player-1 to player-8. */
#define CRINPUT_EVDEV_PLAYER_LED_MAX 8

/* How long one play of a rumble effect lasts, in milliseconds: long enough
 * that a poll at most CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS after the last
 * play is always in time to play it again. */
#define CRINPUT_EVDEV_RUMBLE_LENGTH_MS (2 * CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS)

/* Bits of a capability bitmap from EVIOCGBIT. */
#define CRINPUT_EVDEV_LONG_BITS (sizeof(unsigned long) * CHAR_BIT)
#define CRINPUT_EVDEV_LONGS(bits) (((bits) + CRINPUT_EVDEV_LONG_BITS - 1) / CRINPUT_EVDEV_LONG_BITS)

static bool crinput_evdev_test_bit(
    const unsigned long *bits,
    unsigned int bit) {
	return (bits[bit / CRINPUT_EVDEV_LONG_BITS] >> (bit % CRINPUT_EVDEV_LONG_BITS)) & 1ul;
}

/* The kernel's button for each of crinput.h's, those that are one key
 * code. The d-pad's may also come from a hat; the triggers are axes. */
static const struct {
	unsigned int code;
	CRINPUT_GAMEPAD_BUTTONS button;
} crinput_evdev_buttons[] = {
    {BTN_SOUTH, CRINPUT_GAMEPAD_BUTTON_SOUTH},
    {BTN_EAST, CRINPUT_GAMEPAD_BUTTON_EAST},
    {BTN_WEST, CRINPUT_GAMEPAD_BUTTON_WEST},
    {BTN_NORTH, CRINPUT_GAMEPAD_BUTTON_NORTH},
    {BTN_TL, CRINPUT_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {BTN_TR, CRINPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {BTN_THUMBL, CRINPUT_GAMEPAD_BUTTON_LEFT_STICK},
    {BTN_THUMBR, CRINPUT_GAMEPAD_BUTTON_RIGHT_STICK},
    {BTN_SELECT, CRINPUT_GAMEPAD_BUTTON_BACK},
    {BTN_START, CRINPUT_GAMEPAD_BUTTON_START},
    {BTN_MODE, CRINPUT_GAMEPAD_BUTTON_GUIDE},
    {BTN_DPAD_UP, CRINPUT_GAMEPAD_BUTTON_DPAD_UP},
    {BTN_DPAD_DOWN, CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN},
    {BTN_DPAD_LEFT, CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT},
    {BTN_DPAD_RIGHT, CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT},
};
#define CRINPUT_EVDEV_BUTTON_COUNT (sizeof(crinput_evdev_buttons) / sizeof(crinput_evdev_buttons[0]))

/* How one of crinput.h's axes is read from a device: from an absolute
 * axis, from a button (a trigger with no axis), or not at all. */
typedef struct crinput_evdev_axis_source {
	/* An ABS_ code, or -1. */
	int abs_code;
	/* A BTN_ code, read as 0 or 1, where abs_code is -1; else -1. */
	int key_code;
	/* The range the kernel gives for the absolute axis. */
	int32_t minimum;
	int32_t maximum;
} crinput_evdev_axis_source;

/* This backend's object for one device: crinput_gamepad_record's
 * `native`. */
typedef struct crinput_evdev_pad {
	/* The instance's allocator: the pad and its strings are freed through
	 * it. */
	const CR_ALLOCATOR *allocator;
	int fd;
	/* Opened for writing as well, which playing an effect needs. */
	bool writable;
	/* The node's device number: what tells it from the other nodes. */
	dev_t rdev;
	/* Reading it failed with ENODEV, or otherwise: it has gone. */
	bool gone;
	/* Events are being skipped up to the next SYN_REPORT after a
	 * SYN_DROPPED, after which the whole state is read again. */
	bool dropping;

	/* What the device has, found when it is opened. */
	CRINPUT_GAMEPAD_BUTTONS buttons_present;
	crinput_evdev_axis_source axes[CRINPUT_GAMEPAD_AXIS_COUNT];
	bool has_hat;
	bool has_rumble;

	/* The state as of the last event read, and what has gone down and come
	 * up since the last poll. */
	CRINPUT_GAMEPAD_BUTTONS buttons;
	CRINPUT_GAMEPAD_BUTTONS went_down;
	CRINPUT_GAMEPAD_BUTTONS went_up;
	/* The d-pad as its hat holds it, kept apart from BTN_DPAD_*: a
	 * direction is down while either holds it. */
	CRINPUT_GAMEPAD_BUTTONS hat_buttons;
	CRINPUT_GAMEPAD_BUTTONS key_buttons;
	/* The hat's two axes as last reported: an event changes only one. */
	int32_t hat_x;
	int32_t hat_y;
	int32_t abs_values[CRINPUT_GAMEPAD_AXIS_COUNT];
	bool key_axis_down[CRINPUT_GAMEPAD_AXIS_COUNT];

	/* The rumble effect: its id once uploaded, else -1; whether it is
	 * playing, and when it was last played. */
	int16_t effect_id;
	bool rumble_playing;
	uint64_t rumble_played_ns;

	/* The battery's power_supply files, kept open and read again at each
	 * poll; -1 for each it lacks. */
	bool has_battery;
	int battery_status_fd;
	int battery_capacity_fd;
	int battery_capacity_level_fd;
	int battery_present_fd;

	/* A multicolor LED's directory in sysfs (owned), or null; how many
	 * channels its multi_intensity has, where red, green and blue are in
	 * it, and its max_brightness. */
	char *rgb_led;
	uint32_t rgb_channel_count;
	uint32_t rgb_channels[3];
	uint32_t rgb_max_brightness;
	/* Else a red, a green and a blue LED (owned): all three, or none. */
	char *color_leds[3];
	uint32_t color_max_brightness[3];
	/* The LEDs named player-1 onwards (owned), and how many of them there
	 * are in an unbroken run from player-1. */
	char *player_leds[CRINPUT_EVDEV_PLAYER_LED_MAX];
	uint32_t player_max_brightness[CRINPUT_EVDEV_PLAYER_LED_MAX];
	uint32_t player_led_count;
} crinput_evdev_pad;

/* This backend's state for an instance: crinput_instance_t's `backend`. */
typedef struct crinput_evdev_instance {
	/* inotify on the directory, or -1 where there is no directory to
	 * watch, as in a container with no input devices. */
	int inotify_fd;
	/* The directory is to be looked through at the next poll: at the
	 * first, after a node arrived or changed, and again after a look
	 * that failed. */
	bool rescan;
} crinput_evdev_instance;

static uint64_t crinput_evdev_now_ns(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* ---- Reading a device ---- */

/* The buttons the hat holds down, from its two axes. */
static CRINPUT_GAMEPAD_BUTTONS crinput_evdev_hat_buttons(
    int32_t x,
    int32_t y) {
	CRINPUT_GAMEPAD_BUTTONS buttons = 0;
	if (x < 0)
		buttons |= CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT;
	if (x > 0)
		buttons |= CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT;
	/* The hat's y grows towards the user, as the sticks' does. */
	if (y < 0)
		buttons |= CRINPUT_GAMEPAD_BUTTON_DPAD_UP;
	if (y > 0)
		buttons |= CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN;
	return buttons;
}

/* Brings a pad's buttons to what its keys and its hat now hold, counting
 * what went down and came up. */
static void crinput_evdev_update_buttons(crinput_evdev_pad *pad) {
	const CRINPUT_GAMEPAD_BUTTONS now     = pad->key_buttons | pad->hat_buttons;
	const CRINPUT_GAMEPAD_BUTTONS changed = now ^ pad->buttons;
	pad->went_down |= changed & now;
	pad->went_up |= changed & ~now;
	pad->buttons = now;
}

/* Reads every key and axis of a pad as the kernel holds it now, and takes
 * the differences from what the pad had as changes. */
static void crinput_evdev_read_whole(crinput_evdev_pad *pad) {
	unsigned long keys[CRINPUT_EVDEV_LONGS(KEY_CNT)];
	memset(keys, 0, sizeof(keys));
	ioctl(pad->fd, EVIOCGKEY(sizeof(keys)), keys);

	pad->key_buttons = 0;
	for (size_t i = 0; i < CRINPUT_EVDEV_BUTTON_COUNT; ++i) {
		if ((pad->buttons_present & crinput_evdev_buttons[i].button) && crinput_evdev_test_bit(keys, crinput_evdev_buttons[i].code))
			pad->key_buttons |= crinput_evdev_buttons[i].button;
	}

	for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
		const crinput_evdev_axis_source *source = &pad->axes[axis];
		if (source->abs_code >= 0) {
			struct input_absinfo info;
			if (ioctl(pad->fd, EVIOCGABS(source->abs_code), &info) == 0)
				pad->abs_values[axis] = info.value;
		} else if (source->key_code >= 0) {
			pad->key_axis_down[axis] = crinput_evdev_test_bit(keys, (unsigned int)source->key_code);
		}
	}

	if (pad->has_hat) {
		struct input_absinfo info;
		if (ioctl(pad->fd, EVIOCGABS(ABS_HAT0X), &info) == 0)
			pad->hat_x = info.value;
		if (ioctl(pad->fd, EVIOCGABS(ABS_HAT0Y), &info) == 0)
			pad->hat_y = info.value;
		pad->hat_buttons = crinput_evdev_hat_buttons(pad->hat_x, pad->hat_y);
	}

	crinput_evdev_update_buttons(pad);
}

/* Takes one event. */
static void crinput_evdev_take_event(
    crinput_evdev_pad *pad,
    const struct input_event *event) {
	if (event->type == EV_SYN) {
		if (event->code == SYN_DROPPED) {
			pad->dropping = true;
		} else if (event->code == SYN_REPORT && pad->dropping) {
			pad->dropping = false;
			crinput_evdev_read_whole(pad);
		}
		return;
	}
	if (pad->dropping)
		return;

	if (event->type == EV_KEY) {
		/* A value of 2 is the kernel's key repeat: the key is still
		 * down. */
		const bool down = event->value != 0;
		for (size_t i = 0; i < CRINPUT_EVDEV_BUTTON_COUNT; ++i) {
			if (crinput_evdev_buttons[i].code != event->code)
				continue;
			if (down)
				pad->key_buttons |= crinput_evdev_buttons[i].button;
			else
				pad->key_buttons &= ~crinput_evdev_buttons[i].button;
			crinput_evdev_update_buttons(pad);
		}
		for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
			if (pad->axes[axis].abs_code < 0 && pad->axes[axis].key_code == (int)event->code)
				pad->key_axis_down[axis] = down;
		}
		return;
	}

	if (event->type == EV_ABS) {
		if (pad->has_hat && (event->code == ABS_HAT0X || event->code == ABS_HAT0Y)) {
			if (event->code == ABS_HAT0X)
				pad->hat_x = event->value;
			else
				pad->hat_y = event->value;
			pad->hat_buttons = crinput_evdev_hat_buttons(pad->hat_x, pad->hat_y);
			crinput_evdev_update_buttons(pad);
			return;
		}
		for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
			if (pad->axes[axis].abs_code == (int)event->code)
				pad->abs_values[axis] = event->value;
		}
	}
}

/* Reads every event the kernel has for a pad, without waiting. A pad whose
 * device has gone is marked so. */
static void crinput_evdev_read_events(crinput_evdev_pad *pad) {
	struct input_event events[64];
	for (;;) {
		const ssize_t got = read(pad->fd, events, sizeof(events));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			/* EAGAIN: nothing more. Anything else, ENODEV above all:
			 * the device can no longer be read. */
			if (errno != EAGAIN)
				pad->gone = true;
			return;
		}
		if (got == 0) {
			pad->gone = true;
			return;
		}

		const size_t count = (size_t)got / sizeof(events[0]);
		for (size_t i = 0; i < count; ++i)
			crinput_evdev_take_event(pad, &events[i]);
		if (count < sizeof(events) / sizeof(events[0]))
			return;
	}
}

/* One axis of crinput.h, from what the device last reported for it. */
static float crinput_evdev_axis_value(
    const crinput_evdev_pad *pad,
    uint32_t axis) {
	const crinput_evdev_axis_source *source = &pad->axes[axis];
	const bool is_trigger                     = axis == CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER || axis == CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER;

	if (source->abs_code < 0)
		return (source->key_code >= 0 && pad->key_axis_down[axis]) ? 1.0f : 0.0f;
	if (source->maximum <= source->minimum)
		return 0.0f;

	/* 0 at the kernel's minimum to 1 at its maximum. */
	float value = (float)((double)(pad->abs_values[axis] - source->minimum) / (double)(source->maximum - source->minimum));
	if (value < 0.0f)
		value = 0.0f;
	if (value > 1.0f)
		value = 1.0f;
	if (is_trigger)
		return value;

	/* -1 to 1; the kernel's y grows towards the user, crinput.h's
	 * away. */
	value = value * 2.0f - 1.0f;
	if (axis == CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y || axis == CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y)
		value = -value;
	return value;
}

/* Lets go of a pad: its effect, if it uploaded one, its node, and its
 * battery files and light names. */
static void crinput_evdev_close(crinput_evdev_pad *pad) {
	if (pad->effect_id >= 0) {
		/* Removing an effect stops it. A device that has gone takes
		 * neither call and has nothing left to stop. */
		ioctl(pad->fd, EVIOCRMFF, (int)pad->effect_id);
	}
	close(pad->fd);

	const int battery_fds[] = {pad->battery_status_fd, pad->battery_capacity_fd, pad->battery_capacity_level_fd, pad->battery_present_fd};
	for (size_t i = 0; i < sizeof(battery_fds) / sizeof(battery_fds[0]); ++i) {
		if (battery_fds[i] >= 0)
			close(battery_fds[i]);
	}
	const CR_ALLOCATOR *allocator = pad->allocator;
	cr_string_free(allocator, pad->rgb_led);
	for (int i = 0; i < 3; ++i)
		cr_string_free(allocator, pad->color_leds[i]);
	for (int i = 0; i < CRINPUT_EVDEV_PLAYER_LED_MAX; ++i)
		cr_string_free(allocator, pad->player_leds[i]);
	CR_DELETE(allocator, crinput_evdev_pad, pad);
}

/* ---- sysfs ---- */

/* Reads a sysfs file from its start, through a descriptor kept open, as a
 * string with its trailing newline cut. Reading from the start makes the
 * kernel write it afresh. */
static bool crinput_evdev_pread_text(
    int fd,
    char *buffer,
    size_t size) {
	if (fd < 0)
		return false;
	ssize_t got = pread(fd, buffer, size - 1, 0);
	if (got < 0)
		return false;
	while (got > 0 && (buffer[got - 1] == '\n' || buffer[got - 1] == ' '))
		--got;
	buffer[got] = '\0';
	return true;
}

/* Opens `name` in a sysfs directory to read, or returns -1. */
static int crinput_evdev_open_attribute(
    const char *directory,
    const char *name) {
	char path[PATH_MAX];
	const int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
	if (length < 0 || (size_t)length >= sizeof(path))
		return -1;
	return open(path, O_RDONLY | O_CLOEXEC);
}

/* Reads `name` in a sysfs directory, as crinput_evdev_pread_text does. */
static bool crinput_evdev_read_attribute(
    const char *directory,
    const char *name,
    char *buffer,
    size_t size) {
	const int fd  = crinput_evdev_open_attribute(directory, name);
	const bool ok = crinput_evdev_pread_text(fd, buffer, size);
	if (fd >= 0)
		close(fd);
	return ok;
}

/* Writes `text` to `name` in a sysfs directory. */
static CR_RESULT crinput_evdev_write_attribute(
    const char *directory,
    const char *name,
    const char *text) {
	char path[PATH_MAX];
	const int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
	if (length < 0 || (size_t)length >= sizeof(path))
		return CR_RESULT_API_SPECIFIC_FAILURE;

	const int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return (errno == EACCES || errno == EPERM || errno == EROFS) ? CR_RESULT_INPUT_ACCESS_DENIED : CR_RESULT_API_SPECIFIC_FAILURE;
	const size_t size   = strlen(text);
	const ssize_t wrote = write(fd, text, size);
	const int error     = errno;
	close(fd);
	if (wrote == (ssize_t)size)
		return CR_SUCCESS;
	return (error == EACCES || error == EPERM) ? CR_RESULT_INPUT_ACCESS_DENIED : CR_RESULT_API_SPECIFIC_FAILURE;
}

/* A whole number of no more than UINT32_MAX, all digits, or false. */
static bool crinput_evdev_parse_u32(
    const char *text,
    uint32_t *out_value) {
	if (!text[0])
		return false;
	uint64_t value = 0;
	for (const char *c = text; *c; ++c) {
		if (*c < '0' || *c > '9')
			return false;
		value = value * 10 + (uint64_t)(*c - '0');
		if (value > UINT32_MAX)
			return false;
	}
	*out_value = (uint32_t)value;
	return true;
}

/* ---- Battery ---- */

/* Finds the first battery among a device directory's power supplies and
 * opens its files. Does nothing once the pad has one. */
static void crinput_evdev_find_battery(
    crinput_evdev_pad *pad,
    const char *device_directory) {
	if (pad->has_battery)
		return;

	char supplies[PATH_MAX];
	int length = snprintf(supplies, sizeof(supplies), "%s/power_supply", device_directory);
	if (length < 0 || (size_t)length >= sizeof(supplies))
		return;
	DIR *directory = opendir(supplies);
	if (!directory)
		return;

	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		char supply[PATH_MAX];
		length = snprintf(supply, sizeof(supply), "%s/%s", supplies, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(supply))
			continue;
		char type[32];
		if (!crinput_evdev_read_attribute(supply, "type", type, sizeof(type)) || strcmp(type, "Battery") != 0)
			continue;

		pad->battery_status_fd         = crinput_evdev_open_attribute(supply, "status");
		pad->battery_capacity_fd       = crinput_evdev_open_attribute(supply, "capacity");
		pad->battery_capacity_level_fd = crinput_evdev_open_attribute(supply, "capacity_level");
		pad->battery_present_fd        = crinput_evdev_open_attribute(supply, "present");
		pad->has_battery = pad->battery_status_fd >= 0 || pad->battery_capacity_fd >= 0 || pad->battery_capacity_level_fd >= 0;
		if (pad->has_battery)
			break;
		if (pad->battery_present_fd >= 0)
			close(pad->battery_present_fd);
		pad->battery_present_fd = -1;
	}
	closedir(directory);
}

/* The battery's power as the kernel reports it now. */
static void crinput_evdev_read_battery(
    const crinput_evdev_pad *pad,
    CRINPUT_GAMEPAD_BATTERY *battery) {
	memset(battery, 0, sizeof(*battery));
	battery->state = CRINPUT_BATTERY_STATE_UNKNOWN;
	char text[64];

	/* A supply that says its battery is not there runs from its cable. */
	if (crinput_evdev_pread_text(pad->battery_present_fd, text, sizeof(text)) && strcmp(text, "0") == 0) {
		battery->state = CRINPUT_BATTERY_STATE_NONE;
		return;
	}

	/* "Not charging" (on power and not filling, which drivers report for
	 * a fault) and "Unknown" have no state of their own. */
	if (crinput_evdev_pread_text(pad->battery_status_fd, text, sizeof(text))) {
		if (strcmp(text, "Discharging") == 0)
			battery->state = CRINPUT_BATTERY_STATE_DISCHARGING;
		else if (strcmp(text, "Charging") == 0)
			battery->state = CRINPUT_BATTERY_STATE_CHARGING;
		else if (strcmp(text, "Full") == 0)
			battery->state = CRINPUT_BATTERY_STATE_FULL;
	}

	/* A percentage, so no steps. */
	uint32_t percent;
	if (crinput_evdev_pread_text(pad->battery_capacity_fd, text, sizeof(text)) && crinput_evdev_parse_u32(text, &percent) && percent <= 100) {
		battery->level_known = true;
		battery->level       = (float)percent / 100.0f;
		return;
	}

	/* Else the kernel's five levels, evenly from 0 to 1. */
	static const char *const levels[] = {"Critical", "Low", "Normal", "High", "Full"};
	if (crinput_evdev_pread_text(pad->battery_capacity_level_fd, text, sizeof(text))) {
		for (uint32_t i = 0; i < 5; ++i) {
			if (strcmp(text, levels[i]) != 0)
				continue;
			battery->level_known = true;
			battery->level       = (float)i / 4.0f;
			battery->level_steps = 5;
			return;
		}
	}
}

/* ---- Lights ---- */

/* Which of red, green and blue a name is: 0, 1 or 2, else -1. */
static int crinput_evdev_color_index(const char *name) {
	if (strcmp(name, "red") == 0)
		return 0;
	if (strcmp(name, "green") == 0)
		return 1;
	if (strcmp(name, "blue") == 0)
		return 2;
	return -1;
}

/* Takes a multicolor LED whose channels include red, green and blue as the
 * pad's light. Returns CR_RESULT_OUT_OF_MEMORY, or CR_SUCCESS whether
 * or not it was one. */
static CR_RESULT crinput_evdev_take_multicolor(
    crinput_evdev_pad *pad,
    const char *led,
    uint32_t max_brightness,
    char *index) {
	uint32_t channels[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
	uint32_t count       = 0;
	char *save           = NULL;
	for (char *token = strtok_r(index, " ", &save); token; token = strtok_r(NULL, " ", &save), ++count) {
		const int color = crinput_evdev_color_index(token);
		if (color >= 0 && channels[color] == UINT32_MAX)
			channels[color] = count;
	}
	if (channels[0] == UINT32_MAX || channels[1] == UINT32_MAX || channels[2] == UINT32_MAX)
		return CR_SUCCESS;

	pad->rgb_led = cr_string_copy(pad->allocator, led);
	if (!pad->rgb_led)
		return CR_RESULT_OUT_OF_MEMORY;
	pad->rgb_channel_count  = count;
	pad->rgb_max_brightness = max_brightness;
	memcpy(pad->rgb_channels, channels, sizeof(channels));
	return CR_SUCCESS;
}

/*
 * Looks through a device directory's LEDs for the pad's lights: a
 * multicolor LED, single-colour red, green and blue ones, and player ones,
 * keeping the first of each it finds. Returns CR_RESULT_OUT_OF_MEMORY, or
 * CR_SUCCESS whatever it found.
 */
static CR_RESULT crinput_evdev_find_lights(
    crinput_evdev_pad *pad,
    const char *device_directory) {
	char leds[PATH_MAX];
	int length = snprintf(leds, sizeof(leds), "%s/leds", device_directory);
	if (length < 0 || (size_t)length >= sizeof(leds))
		return CR_SUCCESS;
	DIR *directory = opendir(leds);
	if (!directory)
		return CR_SUCCESS;

	CR_RESULT result = CR_SUCCESS;
	struct dirent *entry;
	while (result == CR_SUCCESS && (entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		char led[PATH_MAX];
		length = snprintf(led, sizeof(led), "%s/%s", leds, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(led))
			continue;

		char text[256];
		uint32_t max_brightness;
		if (!crinput_evdev_read_attribute(led, "max_brightness", text, sizeof(text)) || !crinput_evdev_parse_u32(text, &max_brightness) ||
		    max_brightness == 0)
			continue;

		/* devicename:color:function, where the device name may itself
		 * hold colons; the last part, and the one before it. */
		const char *last = strrchr(entry->d_name, ':');
		if (!last)
			continue;
		char before[64] = "";
		const char *start = last;
		while (start > entry->d_name && start[-1] != ':')
			--start;
		if (start > entry->d_name && (size_t)(last - start) < sizeof(before)) {
			memcpy(before, start, (size_t)(last - start));
			before[last - start] = '\0';
		}
		++last;

		uint32_t player;
		if (strncmp(last, "player-", 7) == 0 && crinput_evdev_parse_u32(last + 7, &player) && player >= 1 &&
		    player <= CRINPUT_EVDEV_PLAYER_LED_MAX) {
			if (!pad->player_leds[player - 1]) {
				pad->player_leds[player - 1] = cr_string_copy(pad->allocator, led);
				if (!pad->player_leds[player - 1])
					result = CR_RESULT_OUT_OF_MEMORY;
				pad->player_max_brightness[player - 1] = max_brightness;
			}
			continue;
		}

		if (crinput_evdev_read_attribute(led, "multi_index", text, sizeof(text))) {
			if (!pad->rgb_led)
				result = crinput_evdev_take_multicolor(pad, led, max_brightness, text);
			continue;
		}

		/* devicename:color, or devicename:color:function. */
		int color = crinput_evdev_color_index(last);
		if (color < 0)
			color = crinput_evdev_color_index(before);
		if (color >= 0 && !pad->color_leds[color]) {
			pad->color_leds[color] = cr_string_copy(pad->allocator, led);
			if (!pad->color_leds[color])
				result = CR_RESULT_OUT_OF_MEMORY;
			pad->color_max_brightness[color] = max_brightness;
		}
	}
	closedir(directory);
	return result;
}

/* What is found once every directory has been looked through: the player
 * LEDs in an unbroken run from player-1, and the three colour LEDs only if
 * all three are there. */
static void crinput_evdev_settle_lights(crinput_evdev_pad *pad) {
	pad->player_led_count = 0;
	while (pad->player_led_count < CRINPUT_EVDEV_PLAYER_LED_MAX && pad->player_leds[pad->player_led_count])
		++pad->player_led_count;

	if (!pad->color_leds[0] || !pad->color_leds[1] || !pad->color_leds[2]) {
		for (int i = 0; i < 3; ++i) {
			cr_string_free(pad->allocator, pad->color_leds[i]);
			pad->color_leds[i] = NULL;
		}
	}
}

/* Looks for a gamepad's battery and lights under the devices its event
 * node belongs to: its input device, and the device above that. */
static CR_RESULT crinput_evdev_find_sysfs(
    crinput_evdev_pad *pad,
    const char *node_name) {
	static const char *const links[] = {"device/device", "device"};
	for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
		char link[PATH_MAX];
		const int length = snprintf(link, sizeof(link), "%s/%s/%s", CRINPUT_EVDEV_SYSFS_INPUT, node_name, links[i]);
		if (length < 0 || (size_t)length >= sizeof(link))
			continue;
		char *device_directory = realpath(link, NULL);
		if (!device_directory) {
			if (errno == ENOMEM)
				return CR_RESULT_OUT_OF_MEMORY;
			continue;
		}

		crinput_evdev_find_battery(pad, device_directory);
		const CR_RESULT result = crinput_evdev_find_lights(pad, device_directory);
		/* realpath's, from the C library. */
		free(device_directory);
		if (result != CR_SUCCESS)
			return result;
	}
	crinput_evdev_settle_lights(pad);
	return CR_SUCCESS;
}

/* ---- Opening a device ---- */

/* Sets up one axis of crinput.h from the first of `abs_codes` the device
 * has (each -1 for none), else from `key_code` if it has that button. */
static void crinput_evdev_find_axis(
    crinput_evdev_pad *pad,
    uint32_t axis,
    const unsigned long *abs_bits,
    const unsigned long *key_bits,
    int first_abs_code,
    int second_abs_code,
    int key_code) {
	crinput_evdev_axis_source *source = &pad->axes[axis];
	source->abs_code                    = -1;
	source->key_code                    = -1;

	const int abs_codes[2] = {first_abs_code, second_abs_code};
	for (int i = 0; i < 2; ++i) {
		if (abs_codes[i] < 0 || !crinput_evdev_test_bit(abs_bits, (unsigned int)abs_codes[i]))
			continue;
		struct input_absinfo info;
		if (ioctl(pad->fd, EVIOCGABS(abs_codes[i]), &info) != 0)
			continue;
		source->abs_code = abs_codes[i];
		source->minimum  = info.minimum;
		source->maximum  = info.maximum;
		return;
	}
	if (key_code >= 0 && crinput_evdev_test_bit(key_bits, (unsigned int)key_code))
		source->key_code = key_code;
}

/*
 * Opens a node and keeps it if the kernel calls it a gamepad. Returns the
 * pad, or NULL for a node that is not one, cannot be opened, or for want of
 * memory (then *out_of_memory is set).
 */
static crinput_evdev_pad *crinput_evdev_open(
    const CR_ALLOCATOR *allocator,
    const char *path,
    const char *node_name,
    dev_t rdev,
    bool *out_of_memory) {
	/* Writing is for rumble; a node the user may only read is still read. */
	bool writable = true;
	int fd        = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
		writable = false;
		fd       = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0)
		return NULL;

	unsigned long ev_bits[CRINPUT_EVDEV_LONGS(EV_CNT)];
	unsigned long key_bits[CRINPUT_EVDEV_LONGS(KEY_CNT)];
	unsigned long abs_bits[CRINPUT_EVDEV_LONGS(ABS_CNT)];
	unsigned long ff_bits[CRINPUT_EVDEV_LONGS(FF_CNT)];
	memset(ev_bits, 0, sizeof(ev_bits));
	memset(key_bits, 0, sizeof(key_bits));
	memset(abs_bits, 0, sizeof(abs_bits));
	memset(ff_bits, 0, sizeof(ff_bits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !crinput_evdev_test_bit(ev_bits, EV_KEY) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 || !crinput_evdev_test_bit(key_bits, BTN_GAMEPAD)) {
		close(fd);
		return NULL;
	}
	if (crinput_evdev_test_bit(ev_bits, EV_ABS))
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
	if (crinput_evdev_test_bit(ev_bits, EV_FF))
		ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits);

	crinput_evdev_pad *pad = CR_NEW_ZEROED(allocator, crinput_evdev_pad);
	if (!pad) {
		close(fd);
		*out_of_memory = true;
		return NULL;
	}
	pad->allocator                 = allocator;
	pad->fd                        = fd;
	pad->writable                  = writable;
	pad->rdev                      = rdev;
	pad->effect_id                 = -1;
	pad->battery_status_fd         = -1;
	pad->battery_capacity_fd       = -1;
	pad->battery_capacity_level_fd = -1;
	pad->battery_present_fd        = -1;

	for (size_t i = 0; i < CRINPUT_EVDEV_BUTTON_COUNT; ++i) {
		if (crinput_evdev_test_bit(key_bits, crinput_evdev_buttons[i].code))
			pad->buttons_present |= crinput_evdev_buttons[i].button;
	}
	pad->has_hat = crinput_evdev_test_bit(abs_bits, ABS_HAT0X) && crinput_evdev_test_bit(abs_bits, ABS_HAT0Y);
	if (pad->has_hat)
		pad->buttons_present |= CRINPUT_GAMEPAD_BUTTON_DPAD_UP | CRINPUT_GAMEPAD_BUTTON_DPAD_DOWN | CRINPUT_GAMEPAD_BUTTON_DPAD_LEFT |
		                        CRINPUT_GAMEPAD_BUTTON_DPAD_RIGHT;

	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_LEFT_STICK_X, abs_bits, key_bits, ABS_X, -1, -1);
	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_LEFT_STICK_Y, abs_bits, key_bits, ABS_Y, -1, -1);
	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_X, abs_bits, key_bits, ABS_RX, -1, -1);
	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_RIGHT_STICK_Y, abs_bits, key_bits, ABS_RY, -1, -1);
	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER, abs_bits, key_bits, ABS_HAT2Y, ABS_Z, BTN_TL2);
	crinput_evdev_find_axis(pad, CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER, abs_bits, key_bits, ABS_HAT2X, ABS_RZ, BTN_TR2);

	pad->has_rumble = crinput_evdev_test_bit(ev_bits, EV_FF) && crinput_evdev_test_bit(ff_bits, FF_RUMBLE);

	if (crinput_evdev_find_sysfs(pad, node_name) != CR_SUCCESS) {
		crinput_evdev_close(pad);
		*out_of_memory = true;
		return NULL;
	}
	return pad;
}


/* What a gamepad is: written once, when it is first seen. */
static void crinput_evdev_fill_properties(
    CRINPUT_GAMEPAD_PROPERTIES *properties,
    const crinput_evdev_pad *pad) {
	char name[CRINPUT_GAMEPAD_NAME_SIZE];
	memset(name, 0, sizeof(name));
	if (ioctl(pad->fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
		name[0] = '\0';
	crinput_shared_copy_utf8(properties->name, CRINPUT_GAMEPAD_NAME_SIZE, name);

	struct input_id id;
	if (ioctl(pad->fd, EVIOCGID, &id) == 0) {
		properties->vendor_id  = id.vendor;
		properties->product_id = id.product;
	}

	properties->buttons         = pad->buttons_present;
	properties->axes            = 0;
	properties->analog_triggers = true;
	for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis) {
		const crinput_evdev_axis_source *source = &pad->axes[axis];
		if (source->abs_code >= 0 || source->key_code >= 0)
			properties->axes |= 1u << axis;
	}
	if (pad->axes[CRINPUT_GAMEPAD_AXIS_LEFT_TRIGGER].abs_code < 0 || pad->axes[CRINPUT_GAMEPAD_AXIS_RIGHT_TRIGGER].abs_code < 0)
		properties->analog_triggers = false;

	/* FF_RUMBLE has a strong and a weak magnitude, which drivers give to
	 * the heavy and the light motor; one that has a single motor mixes
	 * them. */
	if (pad->has_rumble)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_RUMBLE_LOW_FREQUENCY | CRINPUT_GAMEPAD_FEATURE_RUMBLE_HIGH_FREQUENCY;
	if (pad->has_battery)
		properties->features |= CRINPUT_GAMEPAD_FEATURE_BATTERY;
	if (pad->rgb_led || pad->color_leds[0])
		properties->features |= CRINPUT_GAMEPAD_FEATURE_LIGHT_COLOR;
	if (pad->player_led_count > 0) {
		properties->features |= CRINPUT_GAMEPAD_FEATURE_PLAYER_INDICATOR;
		properties->player_indicator_max = pad->player_led_count;
	}
}

/* Writes what a poll reports about a pad into its record, and starts the
 * next count of changes. */
static void crinput_evdev_fill_record(
    crinput_gamepad_record *record,
    crinput_evdev_pad *pad) {
	CRINPUT_GAMEPAD_STATE *state = &record->state;
	state->buttons                 = pad->buttons;
	state->buttons_went_down       = pad->went_down;
	state->buttons_went_up         = pad->went_up;
	for (uint32_t axis = 0; axis < CRINPUT_GAMEPAD_AXIS_COUNT; ++axis)
		state->axes[axis] = crinput_evdev_axis_value(pad, axis);

	if (pad->has_battery)
		crinput_evdev_read_battery(pad, &record->battery);

	pad->went_down = 0;
	pad->went_up   = 0;
}

/* ---- Rumble ---- */

/* Plays the effect, or stops it. */
static bool crinput_evdev_play(
    crinput_evdev_pad *pad,
    bool play) {
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type  = EV_FF;
	event.code  = (uint16_t)pad->effect_id;
	event.value = play ? 1 : 0;
	if (write(pad->fd, &event, sizeof(event)) != (ssize_t)sizeof(event))
		return false;

	pad->rumble_playing = play;
	if (play)
		pad->rumble_played_ns = crinput_evdev_now_ns();
	return true;
}

/* ---- The directory ---- */

/* The record of the device with this node, or NULL. A record whose device
 * has gone doesn't count: the node may already be a new device's. */
static crinput_gamepad_record *crinput_evdev_find(
    crinput_instance instance,
    dev_t rdev) {
	for (uint32_t i = 0; i < instance->record_count; ++i) {
		const crinput_evdev_pad *pad = (const crinput_evdev_pad *)instance->records[i].native;
		if (pad->rdev == rdev && !pad->gone)
			return &instance->records[i];
	}
	return NULL;
}

/*
 * Opens every gamepad node in the directory that no record has, into a new
 * array of `*out_count` pads (NULL for none), `*out_capacity` long, the count
 * it is freed with. Returns CR_RESULT_OUT_OF_MEMORY
 * with nothing opened, or CR_SUCCESS.
 */
static CR_RESULT crinput_evdev_scan(
    crinput_instance instance,
    crinput_evdev_pad ***out_pads,
    uint32_t *out_count,
    uint32_t *out_capacity) {
	*out_pads     = NULL;
	*out_count    = 0;
	*out_capacity = 0;

	/* No directory: no devices. */
	DIR *directory = opendir(CRINPUT_EVDEV_DIRECTORY);
	if (!directory)
		return CR_SUCCESS;

	crinput_evdev_pad **pads = NULL;
	uint32_t count             = 0;
	uint32_t capacity          = 0;
	CR_RESULT result          = CR_SUCCESS;

	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		char path[sizeof(CRINPUT_EVDEV_DIRECTORY) + 1 + sizeof(entry->d_name)];
		snprintf(path, sizeof(path), "%s/%s", CRINPUT_EVDEV_DIRECTORY, entry->d_name);
		struct stat info;
		if (stat(path, &info) != 0 || !S_ISCHR(info.st_mode))
			continue;
		if (crinput_evdev_find(instance, info.st_rdev))
			continue;

		bool out_of_memory       = false;
		crinput_evdev_pad *pad = crinput_evdev_open(&instance->head.allocator, path, entry->d_name, info.st_rdev, &out_of_memory);
		if (out_of_memory) {
			result = CR_RESULT_OUT_OF_MEMORY;
			break;
		}
		if (!pad)
			continue;

		if (count == capacity) {
			const uint32_t grown       = capacity ? capacity * 2 : 4;
			crinput_evdev_pad **more = CR_RESIZE_ARRAY(&instance->head.allocator, crinput_evdev_pad *, pads, capacity, grown);
			if (!more) {
				crinput_evdev_close(pad);
				result = CR_RESULT_OUT_OF_MEMORY;
				break;
			}
			pads     = more;
			capacity = grown;
		}
		pads[count++] = pad;
	}
	closedir(directory);

	if (result != CR_SUCCESS) {
		for (uint32_t i = 0; i < count; ++i)
			crinput_evdev_close(pads[i]);
		CR_DELETE_ARRAY(&instance->head.allocator, crinput_evdev_pad *, pads, capacity);
		return result;
	}

	*out_pads     = pads;
	*out_count    = count;
	*out_capacity = capacity;
	return CR_SUCCESS;
}

/* Reads what inotify has, without waiting. Returns whether a node arrived
 * or changed. */
static bool crinput_evdev_read_inotify(int inotify_fd) {
	bool changed = false;
	/* Aligned as the events in it are. */
	char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for (;;) {
		const ssize_t got = read(inotify_fd, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return changed;

		/* Any event means a look: a node arrived (IN_CREATE), or its
		 * permissions changed (IN_ATTRIB), or the queue overflowed. */
		changed = true;
	}
}

/* ---- The backend ---- */

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_create(crinput_instance instance) {
	/* The kernel lets any process that can open a node read it, whatever
	 * the user is working in. */
	if (!instance->desc.read_in_background)
		return CR_RESULT_INPUT_UNSUPPORTED;

	crinput_evdev_instance *backend = CR_NEW_ZEROED(&instance->head.allocator, crinput_evdev_instance);
	if (!backend)
		return CR_RESULT_OUT_OF_MEMORY;
	backend->rescan = true;

	backend->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (backend->inotify_fd < 0) {
		CR_DELETE(&instance->head.allocator, crinput_evdev_instance, backend);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	if (inotify_add_watch(backend->inotify_fd, CRINPUT_EVDEV_DIRECTORY, IN_CREATE | IN_ATTRIB) < 0) {
		/* No directory: nothing to watch, and nothing to find. */
		if (errno != ENOENT) {
			close(backend->inotify_fd);
			CR_DELETE(&instance->head.allocator, crinput_evdev_instance, backend);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
		close(backend->inotify_fd);
		backend->inotify_fd = -1;
	}

	instance->backend = backend;
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_evdev_destroy(crinput_instance instance) {
	crinput_evdev_instance *backend = (crinput_evdev_instance *)instance->backend;
	if (backend->inotify_fd >= 0)
		close(backend->inotify_fd);
	CR_DELETE(&instance->head.allocator, crinput_evdev_instance, backend);
	instance->backend = NULL;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_poll(crinput_instance instance) {
	crinput_evdev_instance *backend = (crinput_evdev_instance *)instance->backend;

	if (backend->inotify_fd >= 0 && crinput_evdev_read_inotify(backend->inotify_fd))
		backend->rescan = true;

	/* What every known device did since the last poll, into its pad: this
	 * also finds the ones that have gone, before the directory is looked
	 * through, so that a node a new device has taken over is opened
	 * again. No record is written yet. */
	const uint32_t known = instance->record_count;
	for (uint32_t i = 0; i < known; ++i) {
		crinput_evdev_pad *pad = (crinput_evdev_pad *)instance->records[i].native;
		if (!pad->gone)
			crinput_evdev_read_events(pad);
	}

	/* Everything that can fail for want of memory, before any record is
	 * written to: the new devices, then room for them in the table. A
	 * look that fails is made again at the next poll. */
	crinput_evdev_pad **arrived = NULL;
	uint32_t arrived_count        = 0;
	uint32_t arrived_capacity     = 0;
	if (backend->rescan) {
		CR_RESULT result = crinput_evdev_scan(instance, &arrived, &arrived_count, &arrived_capacity);
		if (result != CR_SUCCESS)
			return result;
		result = crinput_shared_reserve(instance, arrived_count);
		if (result != CR_SUCCESS) {
			for (uint32_t i = 0; i < arrived_count; ++i)
				crinput_evdev_close(arrived[i]);
			CR_DELETE_ARRAY(&instance->head.allocator, crinput_evdev_pad *, arrived, arrived_capacity);
			return result;
		}
		backend->rescan = false;
	}

	const uint64_t now_ns = crinput_evdev_now_ns();
	for (uint32_t i = 0; i < known; ++i) {
		crinput_gamepad_record *record = &instance->records[i];
		crinput_evdev_pad *pad         = (crinput_evdev_pad *)record->native;
		if (pad->gone)
			continue;

		crinput_evdev_fill_record(record, pad);
		record->seen = true;

		/* A running motor is played again before its play runs out:
		 * this is the keeping going that crinput.h asks polls for. */
		if (pad->rumble_playing && now_ns - pad->rumble_played_ns >= (uint64_t)CRINPUT_RUMBLE_POLL_INTERVAL_MAX_MS * 1000000ull)
			crinput_evdev_play(pad, true);
	}

	for (uint32_t i = 0; i < arrived_count; ++i) {
		crinput_evdev_pad *pad         = arrived[i];
		crinput_gamepad_record *record = crinput_shared_add(instance, pad);
		crinput_evdev_fill_properties(&record->properties, pad);

		/* On the first poll that sees it, what is down has gone down. */
		crinput_evdev_read_whole(pad);
		crinput_evdev_fill_record(record, pad);
	}
	CR_DELETE_ARRAY(&instance->head.allocator, crinput_evdev_pad *, arrived, arrived_capacity);

	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION void crinput_evdev_release_gamepad(
    crinput_instance instance,
    crinput_gamepad_record *record) {
	(void)instance;
	crinput_evdev_pad *pad = (crinput_evdev_pad *)record->native;
	if (pad->rumble_playing && !pad->gone)
		crinput_evdev_play(pad, false);
	crinput_evdev_close(pad);
	record->native = NULL;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_set_rumble(
    crinput_instance instance,
    crinput_gamepad_record *record,
    const CRINPUT_GAMEPAD_RUMBLE *rumble) {
	(void)instance;
	crinput_evdev_pad *pad = (crinput_evdev_pad *)record->native;

	/* The trigger motors are not in FF_RUMBLE; the front end has refused a
	 * level above 0 for them, since this gamepad doesn't have them. */
	const bool stop = rumble->low_frequency <= 0.0f && rumble->high_frequency <= 0.0f;
	if (stop && pad->effect_id < 0)
		return CR_SUCCESS;
	if (!pad->writable)
		return CR_RESULT_INPUT_ACCESS_DENIED;
	if (stop)
		return crinput_evdev_play(pad, false) ? CR_SUCCESS : CR_RESULT_API_SPECIFIC_FAILURE;

	/* Uploading an effect with the id of one already uploaded changes it,
	 * playing or not. */
	struct ff_effect effect;
	memset(&effect, 0, sizeof(effect));
	effect.type                      = FF_RUMBLE;
	effect.id                        = pad->effect_id;
	effect.u.rumble.strong_magnitude = (uint16_t)(rumble->low_frequency * 65535.0f + 0.5f);
	effect.u.rumble.weak_magnitude   = (uint16_t)(rumble->high_frequency * 65535.0f + 0.5f);
	effect.replay.length             = CRINPUT_EVDEV_RUMBLE_LENGTH_MS;
	effect.replay.delay              = 0;
	if (ioctl(pad->fd, EVIOCSFF, &effect) < 0)
		return errno == EACCES || errno == EPERM ? CR_RESULT_INPUT_ACCESS_DENIED : CR_RESULT_API_SPECIFIC_FAILURE;
	pad->effect_id = effect.id;

	return crinput_evdev_play(pad, true) ? CR_SUCCESS : CR_RESULT_API_SPECIFIC_FAILURE;
}

/* The front end asks a backend for this only for a gamepad with motion
 * sensors, and no gamepad here is given them. */
CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_set_motion_enabled(
    crinput_instance instance,
    crinput_gamepad_record *record,
    bool enabled) {
	(void)instance;
	(void)record;
	(void)enabled;
	return CR_RESULT_INPUT_FEATURE_NOT_PRESENT;
}

/* A level from 0 to 1 as an LED brightness from 0 to `max_brightness`. */
static uint32_t crinput_evdev_brightness(
    float level,
    uint32_t max_brightness) {
	return (uint32_t)((double)level * (double)max_brightness + 0.5);
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_set_light_color(
    crinput_instance instance,
    crinput_gamepad_record *record,
    float red,
    float green,
    float blue) {
	(void)instance;
	const crinput_evdev_pad *pad = (const crinput_evdev_pad *)record->native;
	const float levels[3]          = {red, green, blue};
	char text[32];

	if (pad->rgb_led) {
		/* Each channel's share in multi_intensity, with the LED at its
		 * brightest: a channel the light has beyond these three is off. */
		char intensity[256];
		size_t used = 0;
		for (uint32_t channel = 0; channel < pad->rgb_channel_count; ++channel) {
			uint32_t value = 0;
			for (int color = 0; color < 3; ++color) {
				if (pad->rgb_channels[color] == channel)
					value = crinput_evdev_brightness(levels[color], pad->rgb_max_brightness);
			}
			const int length = snprintf(intensity + used, sizeof(intensity) - used, channel ? " %u" : "%u", value);
			if (length < 0 || (size_t)length >= sizeof(intensity) - used)
				return CR_RESULT_API_SPECIFIC_FAILURE;
			used += (size_t)length;
		}
		const CR_RESULT result = crinput_evdev_write_attribute(pad->rgb_led, "multi_intensity", intensity);
		if (result != CR_SUCCESS)
			return result;
		snprintf(text, sizeof(text), "%u", pad->rgb_max_brightness);
		return crinput_evdev_write_attribute(pad->rgb_led, "brightness", text);
	}

	for (int color = 0; color < 3; ++color) {
		snprintf(text, sizeof(text), "%u", crinput_evdev_brightness(levels[color], pad->color_max_brightness[color]));
		const CR_RESULT result = crinput_evdev_write_attribute(pad->color_leds[color], "brightness", text);
		if (result != CR_SUCCESS)
			return result;
	}
	return CR_SUCCESS;
}

CRINPUT_BACKEND_FUNCTION CR_RESULT crinput_evdev_set_player_indicator(
    crinput_instance instance,
    crinput_gamepad_record *record,
    uint32_t player) {
	(void)instance;
	const crinput_evdev_pad *pad = (const crinput_evdev_pad *)record->native;
	char text[32];

	/* The others off before the one on, so that two are never lit. */
	for (uint32_t i = 0; i < pad->player_led_count; ++i) {
		if (i + 1 == player)
			continue;
		const CR_RESULT result = crinput_evdev_write_attribute(pad->player_leds[i], "brightness", "0");
		if (result != CR_SUCCESS)
			return result;
	}
	if (player == 0)
		return CR_SUCCESS;
	snprintf(text, sizeof(text), "%u", pad->player_max_brightness[player - 1]);
	return crinput_evdev_write_attribute(pad->player_leds[player - 1], "brightness", text);
}

CRINPUT_BACKEND_FUNCTION void crinput_evdev_get_button_label(
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

CRINPUT_BACKEND_FUNCTION void crinput_evdev_get_axis_label(
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

#endif // CRINPUT_COMPILE_EVDEV

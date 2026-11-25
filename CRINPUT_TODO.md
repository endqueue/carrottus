# Carrottus Input TODO

What Carrottus Input is to gain beyond `crinput.h` as built. The header's
rule stands: Carrottus Input lists what the platform itself presents, and
keeps no table of other devices' layouts (`crinput.h`, its overview).

## A raw HID device (card S68, decided 2026-11-24 with the user: a)

A translation and no more, beside the gamepads the platform presents:
- **Listed by usage:** each HID device the platform reports, with its
  usage page and usage, vendor and product numbers and the platform's
  name, hot-plugged as gamepads are, through the poll.
- **Read and written as reports:** its input reports as the device sends
  them, its output and feature reports written as the caller gives them.
  Nothing in Carrottus says what a report's bytes mean.
- **Per platform:** Windows' HID API or GameInput's raw device (which,
  is the draft's to settle against what GameInput lists), `hidraw` on
  Linux, IOHIDManager on Apple.
- **Rules as every module's:** no global state, polled, no threads of its
  own beyond what a platform imposes (stated per call), the caller's
  allocator, each platform failure to the message callback once S10
  reaches Carrottus Input.

**First, before the draft:** on Windows, what GameInput lists for a
DualSense (Edge) over USB and over Bluetooth, and an 8BitDo-style pad in
DirectInput mode if one is to hand (card S73's Windows item). The answer
goes into the draft's comparison.

**Then:** a draft header for the user's approval (root `CLAUDE.md`, "A
public API change is designed before it is built"), drafted with card
S76's device id (one id for any device, its kinds as bits) and with card
S43 (GameInput loaded by hand) taken in the same build. No SDL3, here or
below (S68's option d).

## Paddles and other extra buttons (card S69, decided 2026-11-24 with the user: a)

`CRINPUT_GAMEPAD_BUTTONS` gains named bits, appended after today's 17:
`PADDLE_1` to `PADDLE_4` (upper right, upper left, lower right, lower
left, SDL's order) and a MISC bit for each extra button some platform
names (Share/Create, the mic mute, the capture button), each named for
what it is. A pad's `buttons` mask in its properties says which it has;
no index-only extras. By a draft header for approval; built when a
backend can report one, the DualSense Edge on the Mac the first device.
What each platform gives is checked on a device first: GameController's
`GCXboxGamepad` paddles and the Edge's, GameInput's paddle bits, xpad's
Elite mapping, and whether `hid-playstation` maps the Edge's. The values
freeze at 1.0.

## What kind of pad it is, and the same one coming back (card S70, decided 2026-11-24 with the user: a)

`CRINPUT_GAMEPAD_PROPERTIES` passes through the platform's own words and
numbers, nothing turned into a family here (that is a table):
- the platform's category string where it has one (GameController's
  `productCategory`: "DualSense", "Xbox One"), empty where it has none;
- vendor and product numbers wherever they can be read (GameInput's,
  evdev's), 0 where they can't;
- a stable identifier, bytes that stay the same when the same device
  reconnects (GameInput's device id, evdev's `uniq` or `phys`), empty
  where the platform has none, said so per native API.

By a draft header for approval; the struct is based, so the fields come
by a chained struct with no break. Drafted with S68's and S76's.

## One id for any device, kinds as bits (card S76, decided 2026-11-24 with the user: a)

Room for wheels, pedals and force feedback before Carrottus Input
freezes, with nothing of wheels written yet:
- **The rename:** `crinput_gamepad_id` becomes `crinput_device_id`,
  `CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED` a device's "not connected", and
  devices are listed by kind, each a bit, so a device that is a wheel
  and a gamepad has one id (GameInput's own model, `GameInputKind`). A
  break at 0.x, listed against `abi.txt` in the commit; none when a new
  kind arrives.
- **The raw layer reads evdev too:** on Linux, S68's raw device covers
  the evdev node the kernel's driver owns (its axes and its
  force-feedback effects, `EVIOCSFF`), not only `hidraw`.
- **Force feedback** gets its own feature bits and calls when a device
  kind needs them; `CRINPUT_GAMEPAD_RUMBLE` stays as it is.
- **Not here:** which evdev axis is a wheel and which a pedal, and a
  vendor's mode switch: those are tables and device knowledge.

The draft covers only the rename and listing by kind, written with S68's
and S70's so the header is redrafted once. Test devices when wheels are
built: a plain-HID wheel over USB and an Xbox-mode one.

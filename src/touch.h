// The touchscreen, shared with a paused Nickel.
//
// The launcher pauses Nickel rather than stopping it, so Nickel keeps the touch
// device open the whole time this app runs. Anything the kernel queues for it
// in that time, it acts on when it resumes. These keep that queue empty.
//
// Kept apart from main.c so it builds on the host, where a uinput device can
// stand in for the panel.

#ifndef KOBOARD_TOUCH_H
#define KOBOARD_TOUCH_H

#include "libevdev/libevdev.h"

// Take the device exclusively, so touches reach this process alone. Returns 0,
// or a negative errno (and says so on stderr): the app still works without the
// grab, but Nickel will then replay every touch when it resumes.
int touch_grab(struct libevdev *evdev);

// Read until the finger is off the glass, or `timeout_ms` passes. `down` is
// whether it was down when called. Everything read here — the lift included —
// is gone for good, so call this before releasing the grab.
void touch_wait_for_lift(struct libevdev *evdev, int fd, int down, int timeout_ms);

// Give the device back. Closing the fd does this too (even in a crash); this
// just makes the moment explicit.
void touch_release(struct libevdev *evdev);

#endif

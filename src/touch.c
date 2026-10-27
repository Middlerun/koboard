#include "touch.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int touch_grab(struct libevdev *evdev)
{
	// Without this, the kernel queues a copy of every touch for Nickel,
	// which then acts on all of them, at once, the moment it resumes: taps on
	// its own UI, as stale as the session is long. The kernel drops the grab
	// when the fd closes, so even a crash cannot leave the device held.
	int rc = libevdev_grab(evdev, LIBEVDEV_GRAB);
	if (rc < 0)
		fprintf(stderr, "homesrv: could not grab the touchscreen (%s); "
			"Nickel will replay these touches when it resumes\n",
			strerror(-rc));
	else
		fprintf(stderr, "homesrv: touchscreen grabbed\n");
	return rc;
}

static int elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int) ((now.tv_sec - start->tv_sec) * 1000
		      + (now.tv_nsec - start->tv_nsec) / 1000000);
}

void touch_wait_for_lift(struct libevdev *evdev, int fd, int down, int timeout_ms)
{
	// A tap fires on the press, so when it is the Quit tap the finger is
	// usually still on the glass as the app exits. Release the grab then and
	// the lift goes to Nickel — a tap on whatever it has under the Quit
	// button. Bounded, so a missed or stuck release cannot hang the exit.
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		struct input_event ev;
		int rc, flag = LIBEVDEV_READ_FLAG_NORMAL;
		while ((rc = libevdev_next_event(evdev, flag, &ev)) >= 0) {
			// After a dropped-events gap libevdev replays the device's
			// current state; it counts the same as anything else.
			flag = rc == LIBEVDEV_READ_STATUS_SYNC
				? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
			if (ev.type == EV_KEY && (ev.code == BTN_TOUCH ||
						  ev.code == BTN_TOOL_FINGER ||
						  ev.code == BTN_TOOL_PEN))
				down = ev.value > 0;
			else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID
				 && ev.value < 0)
				down = 0;
		}
		if (!down) return;

		int waited = elapsed_ms(&start);
		if (waited >= timeout_ms) {
			fprintf(stderr, "homesrv: finger still down after %d ms; "
				"releasing the touchscreen anyway\n", timeout_ms);
			return;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		poll(&pfd, 1, timeout_ms - waited);
	}
}

void touch_release(struct libevdev *evdev)
{
	libevdev_grab(evdev, LIBEVDEV_UNGRAB);
}

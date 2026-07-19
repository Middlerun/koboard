// HomeSrv Kobo client — skeleton.
//
// Draws a hello screen over Nickel via FBInk to validate the
// build → install → launch pipeline, then exits. Nickel keeps running
// underneath; the drawn screen persists until Nickel repaints.

#include <stdio.h>
#include <string.h>

#include "fbink.h"

#ifndef HOMESRV_VERSION
#define HOMESRV_VERSION "dev"
#endif

int main(int argc, char* argv[])
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("homesrv-kobo %s (FBInk %s)\n", HOMESRV_VERSION, fbink_version());
		return 0;
	}

	FBInkConfig cfg = { 0 };
	cfg.is_quiet    = true;

	int fbfd = fbink_open();
	if (fbfd < 0) {
		fprintf(stderr, "homesrv: fbink_open failed\n");
		return 1;
	}
	if (fbink_init(fbfd, &cfg) < 0) {
		fprintf(stderr, "homesrv: fbink_init failed\n");
		fbink_close(fbfd);
		return 1;
	}

	FBInkState state = { 0 };
	fbink_get_state(&cfg, &state);

	char line[128];

	cfg.is_centered = true;
	cfg.is_cleared  = true;
	cfg.is_flashing = true;
	cfg.fontmult    = 3;
	cfg.row         = 4;
	fbink_print(fbfd, "HomeSrv", &cfg);

	cfg.is_cleared  = false;
	cfg.is_flashing = false;
	cfg.fontmult    = 2;

	cfg.row = 8;
	snprintf(line, sizeof(line), "Kobo client skeleton v%s", HOMESRV_VERSION);
	fbink_print(fbfd, line, &cfg);

	cfg.row = 10;
	snprintf(line,
		 sizeof(line),
		 "FBInk %s on a %s (%s)",
		 fbink_version(),
		 state.device_name,
		 state.device_platform);
	fbink_print(fbfd, line, &cfg);

	cfg.row = 12;
	snprintf(line,
		 sizeof(line),
		 "%ux%u @ %hu dpi",
		 (unsigned int) state.view_width,
		 (unsigned int) state.view_height,
		 state.screen_dpi);
	fbink_print(fbfd, line, &cfg);

	cfg.row = -4;
	fbink_print(fbfd, "Skeleton OK - Nickel is still running.", &cfg);
	cfg.row = -2;
	fbink_print(fbfd, "Open any menu to redraw its UI.", &cfg);

	fbink_wait_for_complete(fbfd, LAST_MARKER);
	fbink_close(fbfd);
	return 0;
}

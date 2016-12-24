#include "fluidsim.h"

int main(void)
{
	bool quit = false;
	struct window *win = win_make("fluidsim", 640, 480);

	while (!quit) {
		switch (win_handle_events(win)) {
		case FLUIDSIM_EVENT_QUIT:
			quit = true;
			break;
		default:
			/* TODO: We are busy waiting... */
			continue;
		}
	}

	win_destroy(win);

	return EXIT_SUCCESS;
}

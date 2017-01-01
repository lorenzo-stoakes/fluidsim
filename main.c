#include "fluidsim.h"

int main(void)
{
	bool quit = false;
	struct window *win = win_make("fluidsim", 640, 480);
	struct vulkan *vulkan = vulkan_make(win);

	while (!quit) {
		win_update(win);

		switch (win_handle_events(win)) {
		case FLUIDSIM_EVENT_QUIT:
			quit = true;
			break;
		default:
			/* TODO: We are busy waiting... */
			continue;
		}
	}

	vulkan_destroy(vulkan);
	win_destroy(win);

	return EXIT_SUCCESS;
}

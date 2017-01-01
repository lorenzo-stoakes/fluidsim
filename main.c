#include "vulkan-expers.h"

int main(void)
{
	bool quit = false;
	struct window *win = win_make("vulkan-expers", 640, 480);
	struct vulkan *vulkan = vulkan_make(win);

	while (!quit) {
		win_update(win);

		switch (win_handle_events(win)) {
		case VULKAN_EXPERS_EVENT_QUIT:
			quit = true;
			break;
		default:
			vulkan_render(vulkan);
			break;
		}
	}

	vulkan_destroy(vulkan);
	win_destroy(win);

	return EXIT_SUCCESS;
}

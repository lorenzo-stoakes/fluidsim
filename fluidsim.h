#ifndef __fluidsim_h
#define __fluidsim_h

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* Output specified error message to stderr (suffixing newline.) */
#define err(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)

/* Output specified error message (suffixing newline) to stderr and exit. */
#define fatal(...) do { err(__VA_ARGS__); exit(EXIT_FAILURE); } while (0)

/* Determine size of static array. */
#define ARRAY_SIZE(__arr) (sizeof(__arr)/sizeof(__arr[0]))

#ifdef __linux__
#include <xcb/xcb.h>
struct window {
	xcb_connection_t *conn;
	xcb_window_t win;
	xcb_atom_t event_delete_win;
};
#else
#error Only linux supported.
#endif

enum fluidsim_event {
	FLUIDSIM_EVENT_NONE,
	FLUIDSIM_EVENT_QUIT
};

/* malloc, or if no memory is available raise a fatal error and exit. */
static inline void *must_malloc(size_t size)
{
	void *ret = malloc(size);

	if (ret == NULL)
		fatal("Out of memory");

	return ret;
}

/* win_<target>.c */
void win_destroy(struct window *win);
enum fluidsim_event win_handle_events(struct window *win);
struct window *win_make(char *title, uint16_t width, uint16_t height);
void win_update(struct window *win);

#endif /* __fluidsim_h */

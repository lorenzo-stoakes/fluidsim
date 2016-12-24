#ifdef __linux__
#include "fluidsim.h"

/* XCB keycode for the 'q' key. */
#define KEY_Q 0x18

/* Retrieve an XCB 'atom' code for specified event name. */
static xcb_atom_t get_xcb_atom(xcb_connection_t *conn, const char *name)
{
	xcb_atom_t ret;
	xcb_intern_atom_cookie_t cookie =
		xcb_intern_atom(conn, false, strlen(name), name);
	xcb_intern_atom_reply_t *reply =
		xcb_intern_atom_reply(conn, cookie, NULL);

	ret = reply->atom;
	free(reply);

	return ret;
}

/* Poll and handle events for the specified window. */
enum fluidsim_event win_handle_events(struct window *win)
{
	xcb_generic_event_t *event = xcb_poll_for_event(win->conn);
	enum fluidsim_event ret = FLUIDSIM_EVENT_NONE;

	if (event == NULL)
		return ret;

	switch (event->response_type & 0x7f) {
	case XCB_KEY_RELEASE:
	{
		xcb_key_release_event_t *key_event =
			(xcb_key_release_event_t *)event;

		if (key_event->detail == KEY_Q)
			ret = FLUIDSIM_EVENT_QUIT;

		break;
	}
	case XCB_CLIENT_MESSAGE:
	{
		xcb_client_message_event_t *client_event =
			(xcb_client_message_event_t *)event;

		if (client_event->data.data32[0] == win->event_delete_win)
			ret = FLUIDSIM_EVENT_QUIT;

		break;
	}
	}

	free(event);
	return ret;
}

/* Create X window. */
struct window *win_make(char *title, uint16_t width, uint16_t height)
{
	struct window *ret;
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	xcb_window_t win;
	xcb_atom_t atom_protocols, atom_delete_win;
	const xcb_setup_t *setup;
	xcb_screen_iterator_t iter;
	uint32_t val_mask, val_list[2];

	conn = xcb_connect(NULL, NULL);
	if (conn == NULL)
		fatal("unable to connect to X windows server.");

	/* Create window, add key press (well release :) handler. */
	setup = xcb_get_setup(conn);
	iter = xcb_setup_roots_iterator(setup);
	screen = iter.data;
	win = xcb_generate_id(conn);
	val_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val_list[0] = screen->black_pixel;
	val_list[1] = XCB_EVENT_MASK_KEY_RELEASE;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win,
			screen->root, 0, 0, width, height, 10,
			XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual,
			val_mask, val_list);

	/* Set window title. */
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
			XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(title),
			title);

	/* Handle window close. */
	atom_protocols = get_xcb_atom(conn, "WM_PROTOCOLS");
	atom_delete_win = get_xcb_atom(conn, "WM_DELETE_WINDOW");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
			atom_protocols, XCB_ATOM_ATOM, 32, 1,
			&atom_delete_win);

	/* Show our window. */
	xcb_map_window(conn, win);

	/* Store data about the window to return to the caller. */
	ret = must_malloc(sizeof(struct window));
	ret->conn = conn;
	ret->win = win;
	ret->event_delete_win = atom_delete_win;

	win_update(ret);

	return ret;
}

/* Force update of window contents. */
void win_update(struct window *win)
{
	xcb_flush(win->conn);
}

/* Destroy specified window, freeing memory as required. */
void win_destroy(struct window *win)
{
	if (win == NULL)
		return;

	xcb_destroy_window(win->conn, win->win);
	xcb_disconnect(win->conn);

	free(win);
}
#endif /* __linux__ */

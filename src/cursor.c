/*
 * Cursor initialisation routines
 */

#include <stdlib.h>

#include "server.h"
#include "wayland.h"

static void
button_press(struct wl_listener *listener, void *data)
{
	/*
	uint32_t mods;
	struct wlr_keyboard *keyboard;
	*/
	struct wlr_pointer_button_event *event;

	event = data;

	switch(event->state) {
	case WLR_BUTTON_PRESSED:
		printf("Button pressed!\n");
		break;
	case WLR_BUTTON_RELEASED:
		printf("Button released!\n");
		break;
	}
}

void
cursor_init(Server *server)
{
	struct wlr_cursor *cursor;

	/* Initialize wlroots cursor, which is an image provided by wlroots to track the..
	 * y'know... cursor. Hook up a few events as well. */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, server->output_layout);

	wlr_cursor_attach_output_layout(cursor, server->output_layout);

	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	server->cursor_button.notify = button_press;
	wl_signal_add(&cursor->events.button, &server->cursor_button);
}
	

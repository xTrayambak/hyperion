#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "server.h"
#include "xwayland.h"

static void focus_toplevel(struct Toplevel *toplevel, struct wlr_surface *surface) 
{
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL) 
	{
		return;
	}

	struct Server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) 
	{
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) 
	{
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (keyboard != NULL) 
	{
		wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct Output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct Output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	wlr_log(WLR_INFO, "Output destroyed!");
	struct Output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	struct Server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	wlr_log(WLR_INFO, "New output attached.");

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL)
	{
		wlr_log(WLR_INFO, "Output mode: %s", (char *)mode);
		wlr_output_state_set_mode(&state, mode);
	} else {
		wlr_log(WLR_INFO, "This output does not have a particular mode.");
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct Output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	
	wlr_log(WLR_INFO, "Setting up event triggers for output");
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) 
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

	focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
}

static void reset_cursor_mode(struct Server *server) 
{
	/* Reset the cursor mode to passthrough. */
	server->cursor_mode = SCOWL_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) 
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}

	wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) 
{
	/* Called when the xdg_toplevel is destroyed. */
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	free(toplevel);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) 
{
	/* Just as with request_maximize, we must send a configure here. */
	struct Toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) 
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on client-side
	 * decorations. tinywl doesn't support maximization, but to conform to
	 * xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
	 * However, if the request was sent before an initial commit, we don't do
	 * anything and let the client finish the initial surface setup. */
	struct Toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) 
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct Keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) 
{
	/* Called when a new surface state is committed. */
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. tinywl
		 * configures the xdg_toplevel with 0,0 size to let the client pick the
		 * dimensions itself. */
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) 
{
	/* Called when the xdg_popup is destroyed. */
	struct Popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void begin_interactive(struct Toplevel *toplevel, enum CursorMode mode, uint32_t edges)
{
	struct Server *server = toplevel->server;
	struct wlr_surface *focused_surface = 
		server->seat->pointer_state.focused_surface;
	
	if (toplevel->xdg_toplevel->base->surface != wlr_surface_get_root_surface(focused_surface))
	{
		wlr_log(WLR_ERROR, "Unfocused surface sent a move/resize request; ignoring.");
		return;
	}

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == SCOWL_CURSOR_MOVE)
	{
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else
	{
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo_box);

		double border_x = (toplevel->scene_tree->node.x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) 
{
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, SCOWL_CURSOR_MOVE, 0);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) 
{
	/* Called when a new surface state is committed. */
	struct Popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * tinywl sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) 
{
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct Toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, SCOWL_CURSOR_RESIZE, event->edges);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) 
{
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;

	struct Popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) 
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	struct Server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) 
{
	struct Server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void server_cursor_frame(struct wl_listener *listener, void *data) 
{
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct Server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) 
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct Server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) 
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	//process_cursor_motion(server, event->time_msec);
}

static void process_cursor_resize(struct Server *server, uint32_t time) 
{
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct Toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geo_box);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box.x, new_top - geo_box.y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_move(struct Server *server, uint32_t time) 
{
	/* Move the grabbed toplevel to the new position. */
	struct Toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static struct Toplevel *desktop_toplevel_at(
		struct Server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) 
{
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a tinywl_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the tinywl_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree->node.data;
}

static void process_cursor_motion(struct Server *server, uint32_t time) 
{
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == SCOWL_CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == SCOWL_CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct Toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) 
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) 
{
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct Server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct Toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		reset_cursor_mode(server);
	} else {
		/* Focus that client if the button was _pressed_ */
		focus_toplevel(toplevel, surface);
	}
}

static void server_new_pointer(struct Server *server,
		struct wlr_input_device *device) 
{
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) 
{
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct Keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static bool handle_keybinding(struct Server *server, xkb_keysym_t sym) 
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the next toplevel */
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
		struct Toplevel *next_toplevel =
			wl_container_of(server->toplevels.prev, next_toplevel, link);
		focus_toplevel(next_toplevel, next_toplevel->xdg_toplevel->base->surface);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) 
{
	/* This event is raised when a key is pressed or released. */
	struct Keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct Server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void server_new_keyboard(struct Server *server,
		struct wlr_input_device *device) 
{
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct Keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_input(struct wl_listener *listener, void *data) 
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct Server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		wlr_log(WLR_INFO, "New keyboard device attached");
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_log(WLR_INFO, "New pointer device attached");
		server_new_pointer(server, device);
		break;
	default:
		break;
	}

	wlr_log(WLR_INFO, "New input device attached");
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct Server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct Client *client = NULL;

	client = xdg_toplevel->base->data = calloc(1, sizeof(*client));
	client->surface.xdg = xdg_toplevel->base;
	client->bw = 4;

	wlr_log(WLR_DEBUG, "Allocate Toplevel for new surface");

	struct Toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

void xwayland_ready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;
	struct Server *server = wl_container_of(listener, server, xwayland_ready);

	wlr_log(WLR_INFO, "XWayland is now ready - connecting to rootless X server: %s", server->xwayland->display_name);
	xcb_connection_t *xc = xcb_connect(server->xwayland->display_name, NULL);
	int err = xcb_connection_has_error(xc);

	if (err)
	{
		wlr_log(WLR_ERROR, "xcb_connect() failed; XWayland compatibility will not be enabled. (%i)", err);
		return;
	}

	server->netatom[NetWMWindowTypeDialog] = get_x11_atom(xc, "_NET_WM_WINDOW_TYPE_DIALOG");
	server->netatom[NetWMWindowTypeSplash] = get_x11_atom(xc, "_NET_WM_WINDOW_TYPE_SPLASH");
	server->netatom[NetWMWindowTypeToolbar] = get_x11_atom(xc, "_NET_WM_WINDOW_TYPE_TOOLBAR");
	server->netatom[NetWMWindowTypeUtility] = get_x11_atom(xc, "_NET_WM_WINDOW_TYPE_UTILITY");

	wlr_log(WLR_INFO, "Assigning seat to XWayland server");
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	if ((xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(server->xwayland,
			xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
			xcursor->images[0]->width, xcursor->images[0]->height,
			xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
	
	wlr_log(WLR_INFO, "Disconnecting from X server.");
	xcb_disconnect(xc);
}

void xwayland_surface_destroy(struct wl_listener *listener, void *data)
{
	struct Client *client = wl_container_of(listener, client, destroy);
	wlr_log(WLR_INFO, "Destroying XWayland surface");

	if (client->surface.xwayland)
	{
		wl_list_remove(&client->commit.link);
	}

	client->surface.xwayland = NULL;
	
	wl_list_remove(&client->destroy.link);
	wl_list_remove(&client->configure.link);
	wl_list_remove(&client->fullscreen.link);
	wl_list_remove(&client->maximize.link);
	wl_list_remove(&client->activate.link);
	wl_list_remove(&client->set_title.link);
	wl_list_remove(&client->set_hints.link);
	wl_list_remove(&client->set_decoration_mode.link);
	wl_list_remove(&client->associate.link);
	wl_list_remove(&client->dissociate.link);
}

void xwayland_surface_commit(struct wl_listener *listener, void *data)
{
	wlr_log(WLR_DEBUG, "Commit XWayland surface");

	struct Client *client = wl_container_of(listener, client, commit);
	assert(client->kind == X11);
	struct wlr_xwayland_surface *xsurface = client->surface.xwayland;
	struct wlr_surface_state *state = &xsurface->surface->current;

	struct wlr_box new_geo = {0};
	new_geo.width = state->width;
	new_geo.height = state->height;

	wlr_log(WLR_INFO, "XWayland surface dimensions: %ix%i", new_geo.width, new_geo.height);

	bool new_size = new_geo.width != client->geom.width ||
		new_geo.height != client->geom.height;

	if (new_size)
	{
		memcpy(&client->geom, &new_geo, sizeof(struct wlr_box));
	}
}

void xwayland_surface_associate(struct wl_listener *listener, void *data)
{
	struct Client *client = wl_container_of(listener, client, associate);
}

void xwayland_new_surface(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	struct Client *client;

	client = xsurface->data = calloc(1, sizeof(*client));
	client->surface.xwayland = xsurface;
	client->kind = X11;
	client->bw = 0;

	wl_signal_add(&xsurface->events.associate, &client->associate);
	client->associate.notify = xwayland_surface_associate;

	wl_signal_add(&xsurface->events.destroy, &client->destroy);

	client->destroy.notify = xwayland_surface_destroy;
	
	wl_signal_add(&xsurface->surface->events.commit, &client->commit); // segfaults

	client->commit.notify = xwayland_surface_commit;
}

void layer_shell_commit(struct wl_listener *listener, void *data)
{
	struct LayerSurface *lsrf = wl_container_of(listener, lsrf, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = lsrf->layer_surface;
	struct wlr_layer_surface_v1_state old_state;
	
	if (lsrf->layer_surface->initial_commit)
	{
		// arrangement logic
	}

	if (layer_surface->current.committed == 0 && lsrf->mapped == layer_surface->surface->mapped)
		return;

	lsrf->mapped = layer_surface->surface->mapped;

	// arrangement logic
}

void layer_shell_unmap(struct wl_listener *listener, void *data)
{
	wlr_log(WLR_INFO, "Layer-shell surface is being unmapped");
	struct LayerSurface *lsrf = wl_container_of(listener, lsrf, unmap);

	lsrf->mapped = 0;
}

void layer_shell_destroy(struct wl_listener *listener, void *data)
{
	wlr_log(WLR_INFO, "Layer-shell surface is being destroyed");
	struct LayerSurface *lsrf = wl_container_of(listener, lsrf, destroy);

	wl_list_remove(&lsrf->link);
	wl_list_remove(&lsrf->destroy.link);
	wl_list_remove(&lsrf->unmap.link);
	wl_list_remove(&lsrf->surface_commit.link);
	wlr_scene_node_destroy(&lsrf->scene->node);
	wlr_scene_node_destroy(&lsrf->popups->node);
	free(lsrf);
}

void server_new_layer_surface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	struct Server *server = wl_container_of(listener, server, new_layer_surface);
	struct LayerSurface *lsrf;
	struct wlr_surface *surface = layer_surface->surface;

	wlr_log(WLR_INFO, "New layer-shell surface has been instantiated.");

	lsrf = layer_surface->data = calloc(1, sizeof(*lsrf));
	lsrf->kind = LayerShell;
	
	wl_signal_add(&surface->events.commit, &lsrf->surface_commit);
	lsrf->surface_commit.notify = layer_shell_commit;

	wl_signal_add(&surface->events.unmap, &lsrf->unmap);
	lsrf->unmap.notify = layer_shell_unmap;

	wl_signal_add(&surface->events.destroy, &lsrf->destroy);
	lsrf->destroy.notify = layer_shell_destroy;

	lsrf->layer_surface = layer_surface;
	wlr_surface_send_enter(surface, layer_surface->output);
}

void server_init(struct Server server)
{
	server.display = wl_display_create();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), NULL);

	if (server.backend == NULL) 
	{
		wlr_log(WLR_ERROR, "Failed to instantiate backend");
		return;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) 
	{
		wlr_log(WLR_ERROR, "Failed to instantiate renderer");
		return;
	}

	wlr_renderer_init_wl_display(server.renderer, server.display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) 
	{
		wlr_log(WLR_ERROR, "Failed to instantiate allocator");
		return;
	}

	wlr_log(WLR_INFO, "Creating wlroots compositor");
	server.compositor = wlr_compositor_create(server.display, 5, server.renderer);

	wlr_log(WLR_INFO, "Creating wlroots subcompositor");
	server.subcompositor = wlr_subcompositor_create(server.display);

	wlr_log(WLR_INFO, "Creating data-device manager");
	wlr_data_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	
	wlr_log(WLR_INFO, "Creating scene");
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	wlr_log(WLR_INFO, "Setting up xdg-shell V3");
	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	wlr_log(WLR_INFO, "Setting up wlr-layer-shell");
	server.layer_shell = wlr_layer_shell_v1_create(server.display, 3);
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);
	server.new_layer_surface.notify = server_new_layer_surface;
	
	wlr_log(WLR_INFO, "Initializing cursor");
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = SCOWL_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);
	
	const char *sock = wl_display_add_socket_auto(server.display);
	if (!sock) 
	{
		wlr_log(WLR_ERROR, "Failed to initialize display socket!");
		wlr_backend_destroy(server.backend);
		return;
	}

	wlr_log(WLR_INFO, "Starting backend");
	if (!wlr_backend_start(server.backend)) 
	{
		wlr_log(WLR_ERROR, "Failed to start backend!");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return;
	}
	
	wlr_log(WLR_INFO, "Initializing XWayland layer");

	/* Make sure that XWayland clients don't connect to the parent X server when running in a nested compositor */
	unsetenv("DISPLAY");
	server.xwayland = wlr_xwayland_create(server.display, server.compositor, true);

	if (server.xwayland == NULL)
	{
		wlr_log(WLR_ERROR, "Failed to initialize XWayland!");
	} else
	{
		wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
		server.xwayland_ready.notify = xwayland_ready;

		wl_signal_add(&server.xwayland->events.new_surface, &server.xwayland_surface);
		server.xwayland_surface.notify = xwayland_new_surface;
	}
	
	setenv("WAYLAND_DISPLAY", sock, true);
	setenv("XDG_CURRENT_DESKTOP", "scowl", true);
	setenv("DISPLAY", server.xwayland->display_name, true);

	if (fork() == 0)
	{
		execl("/bin/sh", "/bin/sh", "-c", "foot", (void *)NULL);
	}

	wlr_log(WLR_INFO, "Wayland backend starting on socket path: %s", sock);
	wl_display_run(server.display);

	wlr_log(WLR_INFO, "Cleaning up and exiting.");
	wl_display_destroy_clients(server.display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
}

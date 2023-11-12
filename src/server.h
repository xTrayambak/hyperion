/*
 * Types and functions centered around
 * core operation of the compositor.
 */

#include "util.h"
#include "wayland.h"

#ifndef _SERVER_H_
#define _SERVER_H_

/* this is needed because of cyclic struct references */
typedef struct _View View;

typedef struct _Server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;

	struct wlr_xdg_shell *xdg_shell;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	CursorMode cursor_mode;
	View *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	struct wlr_scene_tree *view_menu;
	View *opened_menu_view;
	struct wlr_scene_rect *selected_menu_item;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
} Server;

typedef struct {
	struct wl_list link;
	Server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
} Keyboard;

typedef struct {
	NodeType type;
	void *owner;
	View *view;
	int index;
	struct wl_listener destroy;
} NodeInfo;

typedef struct _View {
	Server *server;
	Title title;
	struct wl_list link;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_node *scene_node;
	struct wlr_scene_rect *border;
	struct wlr_scene_rect *titlebar;
	struct wlr_scene_rect *close_button;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener set_title;
	Rectangle saved_geometry;
	int x, y;
} View;

typedef struct {
	Server *server;
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wlr_scene_rect *background;
} Output;

void server_init(Server *);
void server_new_output(struct wl_listener *, void *);

#endif

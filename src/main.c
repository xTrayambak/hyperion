#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/util/log.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <pango/pangocairo.h>
#include <drm_fourcc.h>

#include "xdg-shell-protocol.h"

/* For brevity's sake, struct members are annotated where they are used. */
typedef enum {
	HYPERION_CURSOR_PASSTHROUGH,
	HYPERION_CURSOR_MOVE,
	HYPERION_CURSOR_RESIZE,
	HYPERION_CURSOR_PRESSED,
} CursorMode;

typedef struct {
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
	struct hyperion_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	struct wlr_scene_tree *view_menu;
	struct hyperion_view *opened_menu_view;
	struct wlr_scene_rect *selected_menu_item;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
} Server;

typedef struct {
	Server *server;
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wlr_scene_rect *background;
} Output;

typedef struct {
	int x, y, width, height;
} Rectangle;

typedef struct {
	struct wlr_scene_buffer *buffer;
	int original_width, current_width;
} Title;

typedef struct {
	struct wl_list link;
	struct hyperion_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_node *scene_node;
	struct wlr_scene_rect *border;
	struct wlr_scene_rect *titlebar;
	struct wlr_scene_rect *close_button;
	Title title;
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
	struct wl_list link;
	Server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
} Keyboard;

typedef enum {
	NONE,
	TITLEBAR,
	BORDER,
	CLOSE_BUTTON,
	MENU,
} NodeType;

typedef struct {
	NodeType type;
	void *owner;
	View *view;
	int index;
	struct wl_listener destroy;
} NodeInfo;

void
panic(char *msg) {
	printf("Hyperion Panic: %s\n", msg);
	abort();
}

void
button_press(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;

	uint32_t mods;

	switch(event->state) {
	case WLR_BUTTON_PRESSED:
		printf("Button pressed!\n");
	case WLR_BUTTON_RELEASED:
		printf("Button released!\n");
	}
}

/*
 * This function is triggered whenever the compositor is ready to draw a frame
 * (generally at a rate of 60Hz)
*/
static void
output_frame(struct wl_listener *listener, void *data)
{
	Output *output = wl_container_of(listener, output, frame);
	Server *server = output->server;
	struct wlr_scene *scene = output->server->scene;
	struct wlr_renderer *renderer = output->server->renderer;
	
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	if (!wlr_output_attach_render(output->wlr_output, NULL)) {
		return;
	}

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	
	// Do some OpenGL stuff
	wlr_renderer_begin(renderer, width, height);
	
	// Clear the screen before doing anything
	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

	wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "fleur", server->cursor);

	wlr_renderer_end(renderer);
	
	// Let all clients know that a frame got rendered.
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
	wlr_output_commit(output->wlr_output); 
}

static void
server_new_output(struct wl_listener *listener, void *data)
{
	printf("New output attached!\n");

	Server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	
	printf("Initializing renderer on new output.\n");
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	
	if (!wl_list_empty(&wlr_output->modes)) {
		printf("Enabling output and setting up modes.\n");
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output, mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}
	
	printf("Allocating hyperion_output\n");
	Output *output = calloc(1, sizeof(Output));

	output->wlr_output = wlr_output;
	output->server = server;

	printf("Attaching listener to the frame notify event.\n");
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

	/* printf("Creating background for wlroots scene.\n");
	output->background = wlr_scene_rect_create(
		&server->scene->tree,
		output->wlr_output->width, output->wlr_output->height,
		NULL); */
	
	printf("Setting wlroots output layout to auto.\n");
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

int main() {
	if (!getenv("XDG_RUNTIME_DIR")) {
		panic("XDG_RUNTIME_DIR not set!");
	}

	wlr_log_init(WLR_DEBUG, NULL);
	Server server;
	
	wlr_log(WLR_INFO, "Creating Wayland display\n");
	server.wl_display = wl_display_create();

	server.backend = wlr_backend_autocreate(server.wl_display);
	
	server.renderer = wlr_renderer_autocreate(server.backend);

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	
	printf("Creating wlroots allocator\n");
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	
	printf("Creating wlroots compositor and data device manager\n");
	wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 0);
	
	printf("Creating wlroots output layout\n");
	server.output_layout = wlr_output_layout_create();
	
	printf("Initializing inputs list\n");
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	printf("Initializing mouse/cursor\n");
	/* Initialize wlroots cursor, which is an image provided by wlroots to track the..
	 * y'know... cursor. Hook up a few events as well. 
	*/
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	server.cursor_button.notify = button_press;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	
	printf("Creating wlroots scene\n");
	server.scene = wlr_scene_create();
	wlr_scene_attach_output_layout(server.scene, server.output_layout);
	
	printf("Adding socket to Wayland display\n");
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		panic("Failed to add socket to Wayland display!");
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	printf("Starting the wlroots backend\n");
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		panic("Failed to start wlroots backend!");
	}

	printf("Running compositor on %s\n", socket);
	
	printf("Running Wayland display\n");
	wl_display_run(server.wl_display);
	
	printf("Initializing SHM\n");
	wl_display_init_shm(server.wl_display);

	printf("Creating wlroots idle manager\n");
	wlr_idle_create(server.wl_display);

	printf("Destroying Wayland display\n");
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}

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
enum hyperion_cursor_mode {
	HYPERION_CURSOR_PASSTHROUGH,
	HYPERION_CURSOR_MOVE,
	HYPERION_CURSOR_RESIZE,
	HYPERION_CURSOR_PRESSED,
};

struct hyperion_server {
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
	enum hyperion_cursor_mode cursor_mode;
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
};

struct hyperion_output {
	struct wl_list link;
	struct hyperion_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wlr_scene_rect *background;
};

struct previous_geo {
	int x, y, width, height;
};

struct title {
	struct wlr_scene_buffer *buffer;
	int original_width, current_width;
};

struct hyperion_view {
	struct wl_list link;
	struct hyperion_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_node *scene_node;
	struct wlr_scene_rect *border;
	struct wlr_scene_rect *titlebar;
	struct wlr_scene_rect *close_button;
	struct title title;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener set_title;
	struct previous_geo saved_geometry;
	int x, y;
};

struct hyperion_keyboard {
	struct wl_list link;
	struct hyperion_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

enum hyperion_node_type {
	NONE,
	TITLEBAR,
	BORDER,
	CLOSE_BUTTON,
	MENU,
};

struct hyperion_node_details {
	enum hyperion_node_type type;
	void *owner;
	struct hyperion_view *view;
	int index;
	struct wl_listener destroy;
};

static void output_frame(struct wl_listener *listener, void *data) {
	printf("Output is ready to commit a frame!\n");
	struct hyperion_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;
	struct wlr_renderer *renderer = output->server->renderer;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	
	// Do some OpenGL stuff
	wlr_renderer_begin(renderer, width, height);
	
	// Clear the screen before doing anything
	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

	struct wl_resource *_surface;

	wl_resource_for_each(_surface, &server->compositor->surfaces) {
		struct wlr_surface *surface = wlr_surface_from_resource(_surface);
		if (!wlr_surface_has_buffer(surface)) {
			continue;
		}

		struct wlr_box render_box = {
			.x = 20, .y = 20,
			.width = surface->current->width,
			.height = surface->current->height
		};

		float matrix[16];

		wlr_matrix_project_box(&matrix, &render_box,
				surface->current->transform,
				0, &output->transform_matrix);

	}

	// wlr_output_swap_buffers(wlr_output, NULL, NULL);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	printf("New output attached!\n");

	struct hyperion_server *server = wl_container_of(listener, server, new_output);
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
	struct hyperion_output *output = calloc(1, sizeof(struct hyperion_output));

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
	struct hyperion_server server;
	
	printf("Creating Wayland display\n");
	server.wl_display = wl_display_create();

	printf("Creating wlroots backend\n");
	server.backend = wlr_backend_autocreate(server.wl_display);
	
	printf("Creating wlroots renderer\n");
	server.renderer = wlr_renderer_autocreate(server.backend);

	printf("Initializing wlroots renderer\n");
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
	
	printf("Creating wlroots scene\n");
	server.scene = wlr_scene_create();
	wlr_scene_attach_output_layout(server.scene, server.output_layout);
	
	printf("Adding socket to Wayland display\n");
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	printf("Starting the wlroots backend\n");
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	printf("Running compositor on %s\n", socket);
	
	printf("Running Wayland display\n");
	wl_display_run(server.wl_display);
	
	printf("Initializing shm\n");
	wl_display_init_shm(server.wl_display);

	printf("Creating wlroots idle manager\n");
	wlr_idle_create(server.wl_display);

	printf("Destroying Wayland display\n");
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}

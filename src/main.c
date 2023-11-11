#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <err.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <pango/pangocairo.h>
#include <drm_fourcc.h>

#include "cursor.h"
#include "server.h"
#include "util.h"
#include "wayland.h"

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
	/* new output has been attached */

	Server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	
	/* initialize renderer on new output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	
	if (!wl_list_empty(&wlr_output->modes)) {
		/* enable output and set up modes */
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output, mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}
	
	Output *output = calloc(1, sizeof(Output));

	output->wlr_output = wlr_output;
	output->server = server;

	/* attach listener to the frame notify event */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

	/* create background for wlroots scene */
	/*
	output->background = wlr_scene_rect_create(
		&server->scene->tree,
		output->wlr_output->width, output->wlr_output->height,
		NULL);
	*/
	
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

int
main(int argc, char *argv[]) {
	Server server;
	const char *socket;

	if (!getenv("XDG_RUNTIME_DIR")) {
		errx(1, "XDG_RUNTIME_DIR not set");
	}

	wlr_log_init(WLR_DEBUG, NULL);
	
	wlr_log(WLR_INFO, "Creating Wayland display\n");
	server.wl_display = wl_display_create();

	server.backend = wlr_backend_autocreate(server.wl_display);
	
	server.renderer = wlr_renderer_autocreate(server.backend);

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	
	/* create wlroots allocator */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	
	/* create wlroots compositor and data device manager */
	wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 0);

	/* create wlroots output layout */
	server.output_layout = wlr_output_layout_create();
	
	/* initialize inputs list */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* initialize mouse/cursor */
	cursor_init(&server);

	/* create wlroots scene */
	server.scene = wlr_scene_create();
	wlr_scene_attach_output_layout(server.scene, server.output_layout);
	
	/* add socket to Wayland display */
	socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		errx(1, "failed to add socket to Wayland display");
	}

	/* Start the backend. This will enumerate outputs and inputs,
	 * become the DRM master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		errx(1, "failed to start wlroots backend");
	}

	wl_display_run(server.wl_display);
	wl_display_init_shm(server.wl_display);
	wlr_idle_create(server.wl_display);
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}

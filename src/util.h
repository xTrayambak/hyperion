/*
 * Utility types and functions
 */

#include <stddef.h>

#include "wayland.h"

#ifndef _UTIL_H_
#define _UTIL_H_

typedef enum {
	CURSOR_PASSTHROUGH,
	CURSOR_MOVE,
	CURSOR_RESIZE,
	CURSOR_PRESSED,
} CursorMode;

typedef enum {
	NODETYPE_NONE,
	NODETYPE_TITLEBAR,
	NODETYPE_BORDER,
	NODETYPE_CLOSE_BUTTON,
	NODETYPE_MENU,
} NodeType;

typedef struct {
	int x, y, width, height;
} Rectangle;

typedef struct {
	struct wlr_scene_buffer *buffer;
	int original_width, current_width;
} Title;

void mzero(void *, size_t);

#endif

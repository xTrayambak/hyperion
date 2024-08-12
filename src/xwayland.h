#ifndef XWAYLAND_H_
#define XWAYLAND_H_

#include <stdlib.h>
#include "wayland.h"

enum { NetWMWindowTypeDialog, NetWMWindowTypeSplash, NetWMWindowTypeToolbar, NetWMWindowTypeUtility, NetLast };
enum { X11, Wayland, LayerShell };

xcb_atom_t get_x11_atom(xcb_connection_t *xc, const char *name);

#endif

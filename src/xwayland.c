#include "xwayland.h"

xcb_atom_t get_x11_atom(xcb_connection_t *xc, const char *name)
{
	xcb_atom_t atom = 0;
	xcb_intern_atom_reply_t *reply;
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(xc, 0, strlen(name), name);
	if ((reply = xcb_intern_atom_reply(xc, cookie, NULL)))
		atom = reply->atom;

	free(reply);

	return atom;
}

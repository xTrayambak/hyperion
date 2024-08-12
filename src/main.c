#include "wayland.h"
#include "server.h"

int main()
{
	wlr_log_init(WLR_DEBUG, NULL);

	struct Server server;
	server_init(server);
}

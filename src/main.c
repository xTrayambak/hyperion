#define _POSIX_C_SOURCE 200112L

#include "server.h"

int
main(int argc, char *argv[])
{
	Server server;

	server_init(&server);

	return 0;
}

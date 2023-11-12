#define _POSIX_C_SOURCE 200112L

#include <err.h>
#include <stdio.h>
#include <unistd.h>
#include "server.h"
#include "util.h"

int
main(int argc, char *argv[])
{
	if (getuid() == 0) {
		fprintf(
				stderr,
				"* You are attempting to run scowl as root!\n"
				"* This might cause weird bugs.\n"
		);
		errx(1, "attempt to run scowl as root!");
	}

	if (using_proprietary_drivers()) {
		fprintf(
				stderr,
				"* You are using proprietary GPU drivers\n"
				"* Scowl may or may not work properly if your drivers do not support Wayland.\n"
				"* They most likely have a FOSS counterpart, so you're better off using those instead.\n"
				"\t(fglrx -> radeon)\n\t(nvidia -> nouveau)\n"
				"* Do NOT report issues if you persist using these drivers. We cannot help you.\n\n"
		);
	}

	Server server;

	server_init(&server);

	return 0;
}

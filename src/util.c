#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

void
mzero(void *ptr, size_t sz)
{
	memset(ptr, 0, sz);
}

/*
 * Detect whether proprietary NVIDIA/AMD drivers are being used.
 * Mostly taken from sway, but this function only reports whether proprietary drivers
 * are being used. It's up to scowl to decide what to do next.
 *
*/
bool 
using_proprietary_drivers() 
{
	FILE *mods = fopen("/proc/modules", "r");

	if (!mods) {
		return false;
	}

	char *line = NULL;
	size_t line_size = 0;

	while (getline(&line, &line_size, mods) != -1) {
		if (strncmp(line, "nvidia ", 7) == 0) 
			return true;

		if (strstr(line, "fglrx"))
			return true;
	}
	
	free(line);
	fclose(mods);

	return false;
}

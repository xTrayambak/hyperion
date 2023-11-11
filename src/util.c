#include <string.h>

void
mzero(void *ptr, size_t sz)
{
	memset(ptr, 0, sz);
}

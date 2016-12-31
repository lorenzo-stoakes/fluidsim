#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include "fluidsim.h"

/* Convert degrees to radians. */
float deg_to_rad(float deg)
{
	return deg * M_PI / 180.0f;
}

/* Read specified file into char buffer and also return size. */
unsigned char *read_file(char *path, size_t *psize)
{
	struct stat stats;
	unsigned char *ret;
	FILE *file;
	size_t size;

	if (stat(path, &stats) == -1)
		fatal("Unable to open %s: %s", path, strerror(errno));

	size = stats.st_size;
	if (psize)
		*psize = size;

	ret = malloc(size);
	file = fopen(path, "r");

	if (file == NULL)
		fatal("Unable to open %s: %s", path, strerror(errno));

	if ((unsigned int)fread(ret, 1, stats.st_size, file) != stats.st_size)
		fatal("Unable to read %s: %s", path, strerror(errno));

	fclose(file);

	return ret;
}

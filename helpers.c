#include <errno.h>
#include <libgen.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fluidsim.h"

/* Maximum size of stack-allocated path buffers. */
#define PATH_BUF_SIZE 2048

/* Determine path relative to this binary's path. Very non-portable :) */
static char *rel_path(char *path)
{
	char root_buf[PATH_BUF_SIZE] = {}, buf[PATH_BUF_SIZE] = {};

	if (readlink("/proc/self/exe", root_buf, PATH_BUF_SIZE) == -1)
		fatal("Unable to open /proc/self/exe: %s", strerror(errno));

	if (snprintf(buf, PATH_BUF_SIZE, "%s/%s", dirname(root_buf), path) >=
		PATH_BUF_SIZE)
		fatal("Unable to concatenate paths.");

	return strdup(buf);
}

/* Convert degrees to radians. */
float deg_to_rad(float deg)
{
	return deg * M_PI / 180.0f;
}

/*
 * Read specified file (path relative to binary location) into char buffer and
 * also return size.
 */
unsigned char *read_file(char *path, size_t *psize)
{
	struct stat stats;
	unsigned char *ret;
	FILE *file;
	size_t size;
	/* Get path relative to binary location. */
	char *full_path = rel_path(path);

	if (stat(full_path, &stats) == -1)
		fatal("Unable to open %s: %s", full_path, strerror(errno));

	size = stats.st_size;
	if (psize)
		*psize = size;

	ret = malloc(size);
	file = fopen(full_path, "r");

	if (file == NULL)
		fatal("Unable to open %s: %s", full_path, strerror(errno));

	if ((unsigned int)fread(ret, 1, stats.st_size, file) != stats.st_size)
		fatal("Unable to read %s: %s", full_path, strerror(errno));

	fclose(file);
	free(full_path);

	return ret;
}

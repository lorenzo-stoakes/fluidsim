# TODO: Abstract out linux-specific options.

override CFLAGS += -D_GNU_SOURCE -DVK_USE_PLATFORM_XCB_KHR -g -Wall -Wextra -Wpacked -Wshadow -std=gnu11
LIBFLAGS = -lxcb -lvulkan -lm

SHADER_FILES=cube.vert cube.frag
# Ensure we pick up changes for all relevant files...
CODE_FILES=$(wildcard *.c *.h Makefile)
# ...but are able to filter out ones we don't need to pass to gcc.
FILTER_FILES=Makefile %.h $(SHADER_FILES)

all: cube
clean:
	rm -f cube

cube: $(CODE_FILES) $(SHADER_FILES)
	glslangValidator -V cube.frag -o cube.frag.spv
	glslangValidator -V cube.vert -o cube.vert.spv
	$(CC) $(CFLAGS) $(LIBFLAGS) $(filter-out $(FILTER_FILES), $^) -o $@

.PHONY: all clean

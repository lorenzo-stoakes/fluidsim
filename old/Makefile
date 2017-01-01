# TODO: Abstract out linux-specific options.

override CFLAGS += -D_GNU_SOURCE -DVK_USE_PLATFORM_XCB_KHR -g -Wall -Werror -Wextra -Wpacked -Wshadow -std=gnu11
LIBFLAGS = -lxcb -lvulkan -lm

SHADER_FILES=shaders/1.vert shaders/1.frag
# Ensure we pick up changes for all relevant files...
CODE_FILES=$(wildcard *.c *.h Makefile)
# ...but are able to filter out ones we don't need to pass to gcc.
FILTER_FILES=Makefile %.h $(SHADER_FILES)

all: vulkan-expers
clean:
	rm -f vulkan-expers

vulkan-expers: $(CODE_FILES) $(SHADER_FILES)
	glslangValidator -V shaders/1.frag -o shaders/1.frag.spv
	glslangValidator -V shaders/1.vert -o shaders/1.vert.spv
	$(CC) $(CFLAGS) $(LIBFLAGS) $(filter-out $(FILTER_FILES), $^) -o $@

.PHONY: all clean

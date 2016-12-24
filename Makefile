override CFLAGS += -D_GNU_SOURCE -g -Wall -Werror -Wextra -Wpacked -Wshadow -std=gnu11
LIBFLAGS = -lxcb

# Ensure we pick up changes for all relevant files...
CODE_FILES=$(wildcard *.c *.h Makefile)
# ...but are able to filter out ones we don't need to pass to gcc.
FILTER_FILES=Makefile %.h

all: fluidsim
clean:
	rm -f fluidsim

fluidsim: $(CODE_FILES)
	$(CC) $(CFLAGS) $(LIBFLAGS) $(filter-out $(FILTER_FILES), $^) -o $@

.PHONY: all clean

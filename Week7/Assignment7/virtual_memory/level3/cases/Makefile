CC ?= cc
CFLAGS ?= -O1 -g -std=c11 -Wall -Wextra -Wpedantic -fno-builtin -fno-omit-frame-pointer
CPPFLAGS += -D_GNU_SOURCE
PROGRAMS = case1 case2 case3 case4 case5
.PHONY: all clean smoke
all: $(PROGRAMS)
$(PROGRAMS): %: %.c common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -pthread $(LDLIBS) -o $@
smoke: all
	@set -e; for p in $(PROGRAMS); do ./$$p --help; ./$$p --ticks 2 --interval-ms 50 --threads 2 --mib 1; done
clean:
	rm -f $(PROGRAMS)

CC = gcc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=

PROGRAMS := task_demo context_demo fd_fork_demo syscall_demo
TARGETS := $(addprefix bin/,$(PROGRAMS))

.PHONY: all clean check
all: $(TARGETS)

bin/%: src/%.c src/lab.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $< $(LDFLAGS) $(LDLIBS) -o $@

bin:
	mkdir -p $@

check: all
	bash verify.sh --no-build

clean:
	$(RM) $(TARGETS)

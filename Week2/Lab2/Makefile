CC = gcc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=
PROGRAMS := signal_steps term_demo
TARGETS := $(addprefix bin/,$(PROGRAMS))
.PHONY: all check clean
all: $(TARGETS)
bin/%: src/%.c | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@
bin:
	mkdir -p $@
check: all
	python3 tests/verify.py
clean:
	$(RM) $(TARGETS)

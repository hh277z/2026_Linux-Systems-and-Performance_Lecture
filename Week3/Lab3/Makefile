CC = gcc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=
PROGRAMS := wait_modes fd_scale trigger_steps ipc_server
TARGETS := $(addprefix bin/,$(PROGRAMS))
.PHONY: all prepare answers check check-answer clean
all: $(TARGETS)

# Lab 1-1, lab 2, and lab 3 are complete; lab 1-2 is built after its six blanks.
prepare: bin/wait_modes bin/trigger_steps bin/ipc_server

bin/%: src/%.c src/lab.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

bin/ipc_server: src/ipc_server.c src/log_stats.c src/log_stats.h src/lab.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) src/ipc_server.c src/log_stats.c $(LDFLAGS) $(LDLIBS) -o $@

bin:
	mkdir -p $@

# Instructor-only solution; never copy over the student's source or binary.
answers: prepare bin_answer/wait_modes bin_answer/fd_scale

bin_answer/wait_modes: src_answer/wait_modes.c src/lab.h | bin_answer
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

bin_answer/fd_scale: src_answer/fd_scale.c src/lab.h | bin_answer
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

bin_answer:
	mkdir -p $@

check: all
	python3 tests/verify.py

check-answer: answers
	python3 tests/verify.py --answers

clean:
	$(RM) $(TARGETS) bin_answer/wait_modes bin_answer/fd_scale

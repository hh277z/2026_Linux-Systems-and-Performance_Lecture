CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
PROGRAMS = psi_watch file_reader cpu_busy anon_work

all: $(PROGRAMS:%=bin/%)

bin/%: src/%.c src/common.h
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all

CC = gcc
CFLAGS = -std=c11 -O0 -g -fno-omit-frame-pointer -Wall -Wextra -Werror
PROGRAMS = lab10_a lab10_b lab10_c lab10_view

all: $(PROGRAMS)

lab10_%: src/lab10_%.c src/lab_common.h
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all

CC = gcc
CFLAGS = -std=c11 -O0 -g -Wall -Wextra -Werror -fPIE
LDFLAGS = -pie -Wl,-z,relro,-z,now

.PHONY: all check
all: lab7_memory

lab7_memory: lab7_memory.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

check: lab7_memory
	python3 tests/validate_lab.py

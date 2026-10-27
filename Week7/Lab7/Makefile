CC = gcc
CFLAGS = -std=c11 -O0 -g -Wall -Wextra -Werror -fPIE
LDFLAGS = -pie -Wl,-z,relro,-z,now

.PHONY: all check clean
all: lab7_memory

lab7_memory: lab7_memory.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f lab7_memory
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
BIN     := bin/ws_demo bin/fs_demo bin/mtwork

all: $(BIN)

bin/mtwork: src/mtwork.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/%: src/%.c src/lab.h | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

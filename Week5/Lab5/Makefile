CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
BIN     := bin/mtwork bin/pi_demo

all: $(BIN)

bin/%: src/%.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

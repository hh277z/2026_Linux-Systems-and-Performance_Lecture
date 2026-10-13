CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
BIN     := bin/frameloop bin/svc bin/svcload

all: $(BIN)

bin/%: src/%.c src/a5.h src/svc_proto.h | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

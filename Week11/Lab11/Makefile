# Week 11 lab build
#   bin/emmcsim.so   nbdkit plugin: the eMMC model
#   bin/emmcctl      stat, trace, reset, powercut for a running emmcsim
#   bin/logger       exercise 2: small synchronous log writes
#   bin/otawriter    exercise 3: one large update file
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
EMMC    = src/emmcsim/emmcsim.c src/emmcsim/model.c src/emmcsim/store.c src/emmcsim/ftl.c

all: bin/emmcsim.so bin/emmcctl bin/logger bin/otawriter

bin/emmcsim.so: $(EMMC) src/emmcsim/emmc.h | bin
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(EMMC)

bin/emmcctl: src/emmcsim/emmcctl.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/logger: src/apps/logger.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/otawriter: src/apps/otawriter.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

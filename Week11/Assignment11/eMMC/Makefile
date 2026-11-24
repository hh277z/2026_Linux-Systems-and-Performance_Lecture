# Assignment 11 build
#   bin/emmcsim.so   nbdkit plugin: the eMMC model
#   bin/emmcctl      stat, trace, reset, powercut for a running emmcsim
#   bin/logger       Level 2: small synchronous log writes (-m policy uses src/apps/logpolicy.c)
#   bin/otawriter    Level 3: one large update file (-m policy uses src/apps/otapolicy.c)
#   bin/uiburst      Level 3: app-launch read bursts
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
EMMC    = src/emmcsim/emmcsim.c src/emmcsim/model.c src/emmcsim/store.c src/emmcsim/ftl.c

all: bin/emmcsim.so bin/emmcctl bin/logger bin/otawriter bin/uiburst

bin/emmcsim.so: $(EMMC) src/emmcsim/emmc.h | bin
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(EMMC)

bin/emmcctl: src/emmcsim/emmcctl.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/logger: src/apps/logger.c src/apps/logpolicy.c src/apps/logpolicy.h | bin
	$(CC) $(CFLAGS) -o $@ src/apps/logger.c src/apps/logpolicy.c

bin/otawriter: src/apps/otawriter.c src/apps/otapolicy.c src/apps/otapolicy.h | bin
	$(CC) $(CFLAGS) -o $@ src/apps/otawriter.c src/apps/otapolicy.c

bin/uiburst: src/apps/uiburst.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

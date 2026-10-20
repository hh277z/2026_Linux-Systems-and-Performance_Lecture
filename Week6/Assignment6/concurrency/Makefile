# Assignment 6 build.
#   make              bin/bqtest, bin/bqtest_tsan, bin/albumsvc,
#                     bin/devsvc, bin/devsvc_tsan, bin/devsvc_ref, bin/lockcheck.so
#   make bugif        bin/bqtest_bugif: wait loops become `if` (explanation 1)
#   make albumsvc_ref bin/albumsvc_ref: albumsvc linked with lib/libbq_ref.a instead of src/bq.c
# bin/devsvc_ref is the Level 3 driver linked with lib/libdevsvc_ref.a (the
# reference fixes of the four cases). tools/l3_check.sh compares bin/devsvc with it.
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
LDFLAGS ?= -rdynamic
TSAN    := -O1 -g -std=gnu11 -fsanitize=thread -pthread

L3_CASES := src/sink.c src/bus.c src/catalog.c src/config.c
L3_BASE  := src/devsvc.c src/rcu.c
L3_HDRS  := src/devsvc.h src/rcu.h

all: bin/bqtest bin/bqtest_tsan bin/albumsvc bin/devsvc bin/devsvc_tsan bin/devsvc_ref bin/lockcheck.so

bin/bqtest: src/bqtest.c src/bq.c src/bq.h src/common.h | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/bqtest.c src/bq.c

bin/bqtest_tsan: src/bqtest.c src/bq.c src/bq.h src/common.h | bin
	$(CC) $(TSAN) -o $@ src/bqtest.c src/bq.c

bin/bqtest_bugif: src/bqtest.c src/bq.c src/bq.h src/common.h | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -DBUG_IF -o $@ src/bqtest.c src/bq.c

bin/albumsvc: src/albumsvc.c src/album.c src/album.h src/bq.c src/bq.h src/common.h | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/albumsvc.c src/album.c src/bq.c

bin/albumsvc_ref: src/albumsvc.c src/album.c src/album.h src/bq.h src/common.h lib/libbq_ref.a | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/albumsvc.c src/album.c lib/libbq_ref.a

bin/devsvc: $(L3_BASE) $(L3_CASES) $(L3_HDRS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(L3_BASE) $(L3_CASES)

bin/devsvc_tsan: $(L3_BASE) $(L3_CASES) $(L3_HDRS) | bin
	$(CC) $(TSAN) -o $@ $(L3_BASE) $(L3_CASES)

bin/devsvc_ref: $(L3_BASE) $(L3_HDRS) lib/libdevsvc_ref.a | bin
	$(CC) $(CFLAGS) -o $@ $(L3_BASE) lib/libdevsvc_ref.a

bin/lockcheck.so: tools/lockcheck.c | bin
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

bugif: bin/bqtest_bugif
albumsvc_ref: bin/albumsvc_ref

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all bugif albumsvc_ref clean


# Week 6 실습 빌드
#   bin/counter       실습 1 시간 측정용 (일반 빌드)
#   bin/counter_tsan  실습 1 오류 검사용 (ThreadSanitizer 빌드, 느리므로 시간 측정에 쓰지 않는다)
#   bin/lockbench     실습 2 (1~3단계)
#   bin/cfgread       실습 2 (4단계)
#   bin/cvbug         실습 3
CC     = gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -fno-omit-frame-pointer -pthread
TSAN   := -O1 -g -std=gnu11 -fsanitize=thread -pthread

all: bin/counter bin/counter_tsan bin/lockbench bin/cfgread bin/cvbug

bin/counter: src/counter.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/counter_tsan: src/counter.c | bin
	$(CC) $(TSAN) -o $@ $<

bin/lockbench: src/lockbench.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/cfgread: src/cfgread.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin/cvbug: src/cvbug.c | bin
	$(CC) $(CFLAGS) -o $@ $<

bin:
	mkdir -p bin logs

clean:
	rm -rf bin

.PHONY: all clean

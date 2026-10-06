CC ?= cc
CFLAGS ?= -O2 -g -fno-omit-frame-pointer -Wall -Wextra
LDLIBS += -pthread
PROGRAMS = dengine cpumgr usched

.PHONY: all clean check
all: $(PROGRAMS)

dengine: dengine.c work.h
	$(CC) $(CFLAGS) dengine.c -o $@ $(LDLIBS)
cpumgr: cpumgr.c
	$(CC) $(CFLAGS) cpumgr.c -o $@ $(LDLIBS)
usched: usched.c work.h
	$(CC) $(CFLAGS) usched.c -o $@ $(LDLIBS)

# TODO 구현 후 실행한다. 구현 전에는 nonzero로 실패하는 것이 정상이다.
check: all
	./dengine -n 64 -t 2 -m block -b 256 -r 3
	./dengine -n 64 -t 2 -m stage -b 256 -r 3
	./usched -p rr -q 20 -b 256 -w 1 -j jobs.txt -o check-usched.csv

clean:
	rm -f $(PROGRAMS)

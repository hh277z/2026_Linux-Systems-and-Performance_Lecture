CC ?= cc
CFLAGS ?= -O0 -g3 -Wall -Wextra \
    -Wno-unused-function -fno-omit-frame-pointer -fno-optimize-sibling-calls

all: scenario foreground

scenario: scenario_src/scenario.c scenario_src/common.h
	$(CC) $(CFLAGS) -rdynamic -o $@ scenario_src/scenario.c

foreground: student_support/foreground.c scenario_src/common.h
	$(CC) $(CFLAGS) -o $@ student_support/foreground.c

clean:
	rm -f scenario foreground

.PHONY: all clean


CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra

cpuwork: cpuwork.c
	$(CC) $(CFLAGS) cpuwork.c -o cpuwork -pthread

.PHONY: clean
clean:
	rm -f cpuwork
CC = gcc
CFLAGS = -Wall -Wextra -g

myshell: myshell.c parser.c execute.c jobs.c myshell.h
	$(CC) $(CFLAGS) myshell.c parser.c execute.c jobs.c -o myshell

.PHONY: clean
clean:
	rm -f myshell

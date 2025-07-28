CC=gcc
CFLAGS=-I. \
       -Wextra \
       -Wall \
       -g

main.out: main.c lib/halloc.c
	$(CC) $(CFLAGS) -o $@ $^

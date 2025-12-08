CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra

all: client1

client1: client1.c
	$(CC) $(CFLAGS) -o client1 client1.c

.PHONY: clean
clean:
	rm -f client1

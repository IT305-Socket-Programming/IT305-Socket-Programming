CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?= -pthread

all: server client

server: server.c common.c common.h
	$(CC) $(CFLAGS) -pthread -o $@ server.c common.c $(LDFLAGS)

client: client.c common.c common.h
	$(CC) $(CFLAGS) -o $@ client.c common.c

clean:
	rm -f server client

.PHONY: all clean

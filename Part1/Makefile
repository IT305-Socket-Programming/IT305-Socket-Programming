CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?= -pthread

all: server client

server: server.c common.c common.h
	$(CC) $(CFLAGS) -pthread -o $@ server.c common.c $(LDFLAGS)

client: client.c common.c common.h
	$(CC) $(CFLAGS) -o $@ client.c common.c

test: all
	bash test_part1.sh

clean:
	rm -f server client *.o *.csv
	rm -rf test_out out

.PHONY: all clean test

CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS ?= -pthread

all: server client

server: server.c common.c common.h
	$(CC) $(CFLAGS) -o $@ server.c common.c $(LDFLAGS)

client: client.c common.c common.h
	$(CC) $(CFLAGS) -o $@ client.c common.c $(LDFLAGS)

test: all
	bash tests/test_case2_enhanced.sh

clean:
	rm -f server client *.o *.csv
	rm -rf test_out out .staging_* .session_* .sessions

.PHONY: all clean test

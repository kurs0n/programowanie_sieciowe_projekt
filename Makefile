CC = gcc
CFLAGS = -Wall -Wextra -pthread
TARGETS = client server

all: $(TARGETS)

client: client.c
	$(CC) $(CFLAGS) -o $@ $<

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
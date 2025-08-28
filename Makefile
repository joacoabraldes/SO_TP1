CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic
LDFLAGS = -lpthread -lrt

all: master view player

master: master.c common.h
	$(CC) $(CFLAGS) master.c -o master $(LDFLAGS)

view: view.c common.h
	$(CC) $(CFLAGS) view.c -o view $(LDFLAGS)

player: player.c common.h
	$(CC) $(CFLAGS) player.c -o player $(LDFLAGS)

clean:
	rm -f master view player

.PHONY: all clean
# Makefile

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -pedantic -g -D_XOPEN_SOURCE=700
LDLIBS  := -pthread -lrt -lm

SHM_SRCS := shm_manager.c

MASTER_SRCS := master.c $(SHM_SRCS)
VIEW_SRCS   := view.c $(SHM_SRCS)

PLAYER_SRCS := $(wildcard player*.c)
PLAYER_PROGS := $(PLAYER_SRCS:.c=)

PROGS := master view $(PLAYER_PROGS)

.PHONY: all clean

all: $(PROGS)

master: $(MASTER_SRCS)
	$(CC) $(CFLAGS) $(MASTER_SRCS) -o $@ $(LDLIBS)

view: $(VIEW_SRCS)
	$(CC) $(CFLAGS) $(VIEW_SRCS) -o $@ $(LDLIBS)

%: %.c $(SHM_SRCS)
	$(CC) $(CFLAGS) $< $(SHM_SRCS) -o $@ $(LDLIBS)

clean:
	rm -f $(PROGS) *.o

# Makefile

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -pedantic -g
LDLIBS  := -pthread -lrt

# Shared helper implementation to link into every binary
SHM_SRCS := shm_manager.c

# Explicit sources for master and view (kept for clarity)
MASTER_SRCS := master.c $(SHM_SRCS)
VIEW_SRCS   := view.c $(SHM_SRCS)

# Automatically discover all player*.c sources and corresponding program names
PLAYER_SRCS := $(wildcard player*.c)
PLAYER_PROGS := $(PLAYER_SRCS:.c=)

# All programs to build
PROGS := master view $(PLAYER_PROGS)

.PHONY: all clean

all: $(PROGS)

# explicit master/view rules (linking shm_manager.c)
master: $(MASTER_SRCS)
	$(CC) $(CFLAGS) $(MASTER_SRCS) -o $@ $(LDLIBS)

view: $(VIEW_SRCS)
	$(CC) $(CFLAGS) $(VIEW_SRCS) -o $@ $(LDLIBS)

# Generic rule for any other program that has a single .c source and needs shm_manager.c
# This will build player programs like player.c, player_random.c, player_ai.c, ...
%: %.c $(SHM_SRCS)
	$(CC) $(CFLAGS) $< $(SHM_SRCS) -o $@ $(LDLIBS)

clean:
	rm -f $(PROGS) *.o

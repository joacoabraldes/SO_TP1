#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#define MAX_PLAYERS 9
#define SHM_GAME_STATE "/game_state"
#define SHM_GAME_SYNC "/game_sync"
#define PIPE_READ 0
#define PIPE_WRITE 1

// Estructura para el jugador
typedef struct {
    char name[16];
    unsigned int score;
    unsigned int invalid_moves;
    unsigned int valid_moves;
    unsigned short x, y;
    pid_t pid;
    bool blocked;
} player_t;

// Estructura para el estado del juego
typedef struct {
    unsigned short width;
    unsigned short height;
    unsigned int player_count;
    player_t players[MAX_PLAYERS];
    bool game_over;
    int board[];
} game_state_t;

// Estructura para la sincronización
typedef struct {
    sem_t master_to_view;
    sem_t view_to_master;
    sem_t master_mutex;
    sem_t state_mutex;
    sem_t reader_count_mutex;
    unsigned int reader_count;
    sem_t player_mutex[MAX_PLAYERS];
} game_sync_t;

// Direcciones de movimiento
typedef enum {
    UP = 0,
    UP_RIGHT,
    RIGHT,
    DOWN_RIGHT,
    DOWN,
    DOWN_LEFT,
    LEFT,
    UP_LEFT
} direction_t;

#endif
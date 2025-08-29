#define _XOPEN_SOURCE 700
#include "common.h"
#include "shm_manager.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    size_t state_size = sizeof(game_state_t) + width * height * sizeof(int);

    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, state_size, 0);
    if (!state_mgr) { perror("shm_manager_open state"); exit(EXIT_FAILURE); }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); exit(EXIT_FAILURE); }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);

    srand(getpid() ^ time(NULL));

    while (!game_state->game_over) {
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);

        unsigned char move = rand() % 8;
        ssize_t bytes_written = write(STDOUT_FILENO, &move, 1);
        if (bytes_written != 1) break;
    }
    
    // clean up
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return 0;
}

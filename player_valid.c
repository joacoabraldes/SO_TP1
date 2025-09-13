
#include "common.h"
#include "shm_manager.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

/*
 * Player that waits for master to allow it (per-player semaphore),
 * then grabs state_mutex, inspects the board, chooses a valid move
 * and sends exactly one byte to the master. It will not send again
 * until master posts its semaphore again.
 */

static int find_my_index(game_state_t *gs, game_sync_t *sync) {
    pid_t me = getpid();
    int idx = -1;
    if (sem_wait(&sync->state_mutex) == -1) return -1;
    for (unsigned int i = 0; i < gs->player_count; i++) {
        if ((pid_t)gs->players[i].pid == me) { idx = (int)i; break; }
    }
    sem_post(&sync->state_mutex);
    return idx;
}

static inline void target_from_dir(int gx, int gy, int d, int *tx, int *ty) {
    int nx = gx, ny = gy;
    switch (d) {
        case UP: ny--; break;
        case UP_RIGHT: ny--; nx++; break;
        case RIGHT: nx++; break;
        case DOWN_RIGHT: ny++; nx++; break;
        case DOWN: ny++; break;
        case DOWN_LEFT: ny++; nx--; break;
        case LEFT: nx--; break;
        case UP_LEFT: ny--; nx--; break;
        default: break;
    }
    *tx = nx; *ty = ny;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    size_t state_size = sizeof(game_state_t) + (size_t)width * height * sizeof(int);

    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, state_size, 0);
    if (!state_mgr) {
        perror("shm_manager_open state");
        return EXIT_FAILURE;
    }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);
    if (!game_state) {
        fprintf(stderr, "failed to get game_state pointer\n");
        shm_manager_close(state_mgr);
        return EXIT_FAILURE;
    }

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) {
        perror("shm_manager_open sync");
        shm_manager_close(state_mgr);
        return EXIT_FAILURE;
    }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    (void)game_sync; /* silence unused-variable warning if static analysis/compilation can't detect uses */
    if (!game_sync) {
        fprintf(stderr, "failed to get game_sync pointer\n");
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    /* Find our player index (wait a little if master hasn't written pid yet) */
    int my_index = -1;
    const int max_iters = 500; // ~5s with 10ms sleep
    int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) {
        my_index = find_my_index(game_state, game_sync);
        if (my_index != -1) break;
        struct timespec short_sleep = {0, 10 * 1000 * 1000}; // 10ms
        nanosleep(&short_sleep, NULL);
        it++;
    }
    if (my_index == -1) {
        /* final attempt (without extra wait) */
        my_index = find_my_index(game_state, game_sync);
    }

    if (my_index == -1) {
        fprintf(stderr, "player: couldn't determine my index (pid %d)\n", (int)getpid());
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    /* Seed RNG */
    srand((unsigned int)(getpid() ^ time(NULL)));

    while (1) {
        /* Wait until master allows us to act (this ensures previous move was processed). */
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) {
            if (errno == EINTR) continue;
            break;
        }

        /* If game ended while waiting, quit */
        if (game_state->game_over) break;
        if (game_state->players[my_index].blocked) break;

        /* Now consult state under state_mutex and send exactly one move */
        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
            break;
        }

        if (game_state->game_over) {
            sem_post(&game_sync->state_mutex);
            break;
        }

        int gx = (int)game_state->players[my_index].x;
        int gy = (int)game_state->players[my_index].y;
        int gwidth = game_state->width;
        int gheight = game_state->height;

        /* gather valid moves under mutex */
        int valid_dirs[8];
        int valid_count = 0;
        int pref_dirs[8];
        int pref_count = 0;

        for (int d = 0; d < 8; d++) {
            int tx, ty;
            target_from_dir(gx, gy, d, &tx, &ty);

            /* skip out-of-bounds */
            if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) continue;

            int cell = game_state->board[ty * gwidth + tx];
            if (cell <= 0) continue;

            valid_dirs[valid_count++] = d;

            /* prefer cells not adjacent to other heads */
            bool near_other = false;
            for (unsigned int p = 0; p < game_state->player_count; p++) {
                if ((int)p == my_index) continue;
                int px = (int)game_state->players[p].x;
                int py = (int)game_state->players[p].y;
                if (px == tx && py == ty) { near_other = true; break; }
                if (abs(px - tx) <= 1 && abs(py - ty) <= 1) { near_other = true; break; }
            }
            if (!near_other) pref_dirs[pref_count++] = d;
        }

        if (valid_count == 0) {
            /* nothing to do this turn; release state mutex and continue */
            sem_post(&game_sync->state_mutex);
            continue;
        }

        int *candidates = pref_count > 0 ? pref_dirs : valid_dirs;
        int cand_count = pref_count > 0 ? pref_count : valid_count;

        /* choose the best candidate by reward (board value). If tie, random among ties. */
        int best_value = INT_MIN;
        int bests[8];
        int bests_count = 0;
        for (int i = 0; i < cand_count; i++) {
            int d = candidates[i];
            int tx, ty;
            target_from_dir(gx, gy, d, &tx, &ty);
            int val = game_state->board[ty * gwidth + tx];
            if (val > best_value) {
                best_value = val;
                bests_count = 0;
                bests[bests_count++] = d;
            } else if (val == best_value) {
                bests[bests_count++] = d;
            }
        }

        int pick = bests[rand() % bests_count];
        unsigned char move = (unsigned char)pick;

        /* write exactly one byte while still inside state mutex */
        ssize_t written = write(STDOUT_FILENO, &move, 1);

        /* release state mutex after writing */
        sem_post(&game_sync->state_mutex);

        if (written != 1) {
            if (written == -1 && errno == EPIPE) break; /* master closed -> exit */
            break;
        }

        /* After writing, player must wait again for master to post its semaphore
           before sending another move. */
    }

    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

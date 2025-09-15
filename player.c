#include "common.h"
#include "shm_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <errno.h>
#include <signal.h>

#define MAX_PLAYERS_PROBE 128

static void short_sleep(void) {
    struct timespec ts = {0, 50 * 1000 * 1000}; // 50 ms
    nanosleep(&ts, NULL);
}

static volatile sig_atomic_t stop = 0;
static void handle_sigint(int s) { (void)s; stop = 1; }

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGINT, handle_sigint);

    setvbuf(stdout, NULL, _IONBF, 0);

    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, 0, 0);
    if (!state_mgr) { perror("shm_manager_open state"); exit(EXIT_FAILURE); }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, 0, 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); exit(EXIT_FAILURE); }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);

    pid_t mypid = getpid();
    fprintf(stderr, "[player %d] started\n", (int)mypid);

    unsigned int pc = 0;
    int myid = -1;
    while (!stop) {
        pc = game_state->player_count;
        if (pc > MAX_PLAYERS_PROBE) pc = MAX_PLAYERS_PROBE;

        for (unsigned int i = 0; i < pc; ++i) {
            if (game_state->players[i].pid == mypid) { myid = (int)i; break; }
        }
        if (myid >= 0) break;

        if (game_state->game_over) {
            fprintf(stderr, "[player %d] game already over while waiting for slot\n", (int)mypid);
            shm_manager_close(state_mgr);
            shm_manager_close(sync_mgr);
            return 0;
        }
        short_sleep();
    }

    if (myid < 0) {
        fprintf(stderr, "[player %d] interrupted before assignment, exiting\n", (int)mypid);
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return 1;
    }

    fprintf(stderr, "[player %d] assigned id=%d name='%s'\n", (int)mypid, myid, game_state->players[myid].name);
    srand((unsigned)time(NULL) ^ (unsigned)mypid);

    int dirs[8][2] = { {0,-1}, {1,-1}, {1,0}, {1,1}, {0,1}, {-1,1}, {-1,0}, {-1,-1} };

    while (!game_state->game_over && !stop) {
        if (myid < 0 || (unsigned)myid >= MAX_PLAYERS) break;

        sem_t *my_wake = &game_sync->player_mutex[myid];
        if (!my_wake) {
            fprintf(stderr, "[player %d] ERROR: player_mutex[%d] missing\n", (int)mypid, myid);
            break;
        }

        while (sem_wait(my_wake) == -1) {
            if (errno == EINTR) { if (stop) break; continue; }
            perror("sem_wait player_mutex");
            goto cleanup;
        }
        if (game_state->game_over || stop) break;

        sem_wait(&game_sync->master_mutex);
        sem_post(&game_sync->master_mutex);

        sem_wait(&game_sync->reader_count_mutex);
        game_sync->reader_count++;
        if (game_sync->reader_count == 1) sem_wait(&game_sync->state_mutex);
        sem_post(&game_sync->reader_count_mutex);

        player_t local_player = game_state->players[myid];
        unsigned short width = game_state->width;
        unsigned short height = game_state->height;

        int x = local_player.x;
        int y = local_player.y;

        fprintf(stderr, "[player %d] pos=(%d,%d) score=%u\n", (int)mypid, x, y, local_player.score);

        int valid_dirs[8];
        int valid_count = 0;
        int food_dirs[8];
        int food_count = 0;
        int empty_dirs[8];
        int empty_count = 0;

        for (int d = 0; d < 8; ++d) {
            int nx = x + dirs[d][0];
            int ny = y + dirs[d][1];
            if (nx < 0 || nx >= (int)width || ny < 0 || ny >= (int)height) continue;
            int cell = game_state->board[ny * width + nx];
            
            fprintf(stderr, "[player %d] neighbor d=%d -> (%d,%d) cell=%d\n", (int)mypid, d, nx, ny, cell);
            if (cell >= 0) {
                
                valid_dirs[valid_count++] = d;
                if (cell > 0) food_dirs[food_count++] = d;
                else empty_dirs[empty_count++] = d;
            }
        }

        int best_dir = -1;
        int best_nx = -1, best_ny = -1, best_cell = -999;

        if (food_count > 0) {
            int pick = rand() % food_count;
            best_dir = food_dirs[pick];
        } else if (empty_count > 0) {
            int pick = rand() % empty_count;
            best_dir = empty_dirs[pick];
        } else if (valid_count > 0) {
            int pick = rand() % valid_count;
            best_dir = valid_dirs[pick];
        } else {
            for (int d = 0; d < 8; ++d) {
                int nx = x + dirs[d][0];
                int ny = y + dirs[d][1];
                if (nx < 0 || nx >= (int)width || ny < 0 || ny >= (int)height) continue;
                best_dir = d;
                break;
            }
            if (best_dir == -1) best_dir = 0; 
        }

        best_nx = x + dirs[best_dir][0];
        best_ny = y + dirs[best_dir][1];
        if (best_nx >= 0 && best_nx < (int)width && best_ny >= 0 && best_ny < (int)height)
            best_cell = game_state->board[best_ny * width + best_nx];
        else
            best_cell = -999; 

  
        sem_wait(&game_sync->reader_count_mutex);
        game_sync->reader_count--;
        if (game_sync->reader_count == 0) sem_post(&game_sync->state_mutex);
        sem_post(&game_sync->reader_count_mutex);

        fprintf(stderr, "[player %d] picked dir=%d -> (%d,%d) cell=%d (valid_count=%d food=%d empty=%d)\n",
                (int)mypid, best_dir, best_nx, best_ny, best_cell, valid_count, food_count, empty_count);

        if (best_dir < 0 || best_dir > 7) {
            fprintf(stderr, "[player %d] WARNING: best_dir %d out of range, clamping to 0\n", (int)mypid, best_dir);
            best_dir = 0;
            best_nx = x + dirs[best_dir][0];
            best_ny = y + dirs[best_dir][1];
            if (best_nx >= 0 && best_nx < (int)width && best_ny >= 0 && best_ny < (int)height)
                best_cell = game_state->board[best_ny * width + best_nx];
            else best_cell = -999;
        }

        unsigned char out_byte = (unsigned char)best_dir;
        ssize_t w = write(STDOUT_FILENO, &out_byte, 1);
        if (w == 1) {
            fprintf(stderr, "[player %d] sent BYTE=%u (dir=%d) -> (%d,%d) cell=%d\n",
                    (int)mypid, (unsigned)out_byte, best_dir, best_nx, best_ny, best_cell);
        } else {
            if (w == -1 && errno == EPIPE) {
                fprintf(stderr, "[player %d] master closed pipe (EPIPE). Exiting\n", (int)mypid);
                goto cleanup;
            } else {
                fprintf(stderr, "[player %d] short/failed write (%zd)\n", (int)mypid, w);
            }
        }

        short_sleep();
    }

cleanup:
    fprintf(stderr, "[player %d] exiting (game_over=%d)\n", (int)mypid, (int)game_state->game_over);
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return 0;
}

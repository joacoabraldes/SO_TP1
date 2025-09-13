#define _XOPEN_SOURCE 700
#include "common.h"
#include "shm_manager.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Monte-Carlo / flat-simulation player for the multiplayer capture-points game.
 * Strategy summary:
 *  - On your turn, snapshot the shared state (board + players) under state_mutex.
 *  - Enumerate all valid moves. For each candidate move run many fast playouts
 *    (apply the candidate move, then simulate the rest of the game with a
 *    lightweight greedy+random policy for all players).
 *  - Pick the candidate with the highest average final score (or best score-diff).
 *
 * Implementation notes:
 *  - We copy the board and player array into private buffers and run playouts on
 *    those copies; the real shared state is never modified.
 *  - We reacquire state_mutex only to perform the final write of the chosen
 *    move (write must happen while state_mutex is held, following the project's
 *    convention).
 *  - Number of playouts adapts to board size (so it stays reasonably fast on
 *    large boards while giving more sims for smaller boards).
 *
 * This is *not* a provably-winning algorithm against arbitrary optimal
 * opponents (multiplayer perfect-information games do not generally admit a
 * simple always-win strategy). However, Monte-Carlo evaluation with a sensible
 * playout policy is a strong practical approach and will outperform simple
 * greedy heuristics in almost all boards.
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

/* ----- Simulation helpers (operate on private copies) ----- */

typedef struct {
    int x, y;
    unsigned int score;
    bool blocked;
} sim_player_t;

static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid, int d) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return false;
    int cell = board[ty * width + tx];
    return cell > 0;
}

static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid, int d) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return -1;
    int reward = board[ty * width + tx];
    if (reward <= 0) return -1;
    players[pid].score += (unsigned int)reward;
    board[ty * width + tx] = -(pid + 1); // mark occupied
    players[pid].x = tx;
    players[pid].y = ty;
    players[pid].blocked = false;
    return reward;
}

static bool sim_any_player_has_move(int *board, int width, int height, sim_player_t *players, int player_count) {
    for (int i = 0; i < player_count; i++) {
        if (players[i].blocked) continue;
        for (int d = 0; d < 8; d++) if (sim_is_valid_move(board, width, height, players, player_count, i, d)) return true;
    }
    return false;
}

/* A lightweight playout policy: mostly greedy (pick highest-value move), with a small
   random factor to avoid pathological deterministic ties.  This policy is cheap to
   evaluate and produces reasonably realistic continuations. */
static int sim_pick_policy_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid) {
    int best_dirs[8]; int best_count = 0; int best_val = INT_MIN;
    int valid_dirs[8]; int valid_count = 0;
    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        int cell = board[ty * width + tx];
        if (cell <= 0) continue;
        valid_dirs[valid_count++] = d;
        if (cell > best_val) { best_val = cell; best_count = 0; best_dirs[best_count++] = d; }
        else if (cell == best_val) { best_dirs[best_count++] = d; }
    }
    if (valid_count == 0) return -1;
    /* small noise: with 15% probability pick random valid move, else greedy best */
    if ((rand() & 0xFF) < 38) { // ~15%
        return valid_dirs[rand() % valid_count];
    }
    return best_dirs[rand() % best_count];
}

/* Simulate to the end starting from current simulated state. Returns when terminal.
   start_next_player is the player index who should play next (i.e. the turn order).
*/
static void simulate_playout(int *board, int width, int height, sim_player_t *players, int player_count, int start_next_player) {
    int next = start_next_player;
    while (sim_any_player_has_move(board, width, height, players, player_count)) {
        int p = next;
        next = (next + 1) % player_count;
        if (players[p].blocked) continue;
        int mv = sim_pick_policy_move(board, width, height, players, player_count, p);
        if (mv == -1) { players[p].blocked = true; continue; }
        sim_apply_move(board, width, height, players, player_count, p, mv);
    }
}

/* Copy helpers */
static void copy_board(int *dst, int *src, int n) { memcpy(dst, src, n * sizeof(int)); }
static void copy_players(sim_player_t *dst, player_t *src, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        dst[i].x = (int)src[i].x;
        dst[i].y = (int)src[i].y;
        dst[i].score = src[i].score;
        dst[i].blocked = src[i].blocked;
    }
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
    if (!state_mgr) { perror("shm_manager_open state"); return EXIT_FAILURE; }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);
    if (!game_state) { fprintf(stderr, "failed to get game_state pointer\n"); shm_manager_close(state_mgr); return EXIT_FAILURE; }

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    if (!game_sync) { fprintf(stderr, "failed to get game_sync pointer\n"); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

    int my_index = -1;
    const int max_iters = 500; // small wait loop like the sample
    int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) {
        my_index = find_my_index(game_state, game_sync);
        if (my_index != -1) break;
        struct timespec short_sleep = {0, 10 * 1000 * 1000}; // 10ms
        nanosleep(&short_sleep, NULL);
        it++;
    }
    if (my_index == -1) my_index = find_my_index(game_state, game_sync);
    if (my_index == -1) {
        fprintf(stderr, "player: couldn't determine my index (pid %d)\n", (int)getpid());
        shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    srand((unsigned int)(getpid() ^ time(NULL)));

    /* allocate private buffers used for simulations */
    int *board_snapshot = malloc(width * height * sizeof(int));
    int *board_sim = malloc(width * height * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    sim_player_t *players_sim = malloc(sizeof(sim_player_t) * game_state->player_count);
    if (!board_snapshot || !board_sim || !players_snapshot || !players_sim) {
        fprintf(stderr, "allocation failed\n");
        return EXIT_FAILURE;
    }

    while (1) {
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) {
            if (errno == EINTR) continue; break; }

        if (game_state->game_over) break;
        if (game_state->players[my_index].blocked) break;

        /* Snapshot state under mutex */
        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
            break;
        }

        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }

        int gx = (int)game_state->players[my_index].x;
        int gy = (int)game_state->players[my_index].y;
        int gwidth = game_state->width;
        int gheight = game_state->height;
        unsigned int gplayer_count = game_state->player_count;

        /* copy board and players into private buffers */
        copy_board(board_snapshot, game_state->board, gwidth * gheight);
        copy_players(players_snapshot, game_state->players, gplayer_count);

        sem_post(&game_sync->state_mutex);

        /* Gather valid moves for us from the snapshot (cheap) */
        int valid_dirs[8]; int valid_count = 0;
        for (int d = 0; d < 8; d++) {
            int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
            if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) continue;
            int cell = board_snapshot[ty * gwidth + tx];
            if (cell <= 0) continue;
            valid_dirs[valid_count++] = d;
        }

        if (valid_count == 0) {
            /* nothing to do this turn; release and continue */
            continue;
        }

        /* Choose number of playouts adaptively */
        int board_cells = gwidth * gheight;
        int base = board_cells > 0 ? board_cells : 1;
        int sims_per_candidate = 200; // baseline
        /* increase sims for smaller boards, decrease for huge boards */
        if (board_cells <= 25) sims_per_candidate = 2000;
        else if (board_cells <= 100) sims_per_candidate = 800;
        else if (board_cells <= 400) sims_per_candidate = 300;
        else sims_per_candidate = 150;

        /* Limit total sims to avoid huge CPU use */
        int max_total_sims = 2500;
        long candidate_limit = valid_count;
        long total_sims = (long)sims_per_candidate * candidate_limit;
        if (total_sims > max_total_sims) {
            sims_per_candidate = max_total_sims / (int)candidate_limit;
            if (sims_per_candidate < 10) sims_per_candidate = 10;
        }

        /* For each candidate, run sims and compute average final score */
        double best_avg = -1e300;
        int best_dirs[8]; int best_dirs_count = 0;

        for (int ci = 0; ci < valid_count; ci++) {
            int cand = valid_dirs[ci];
            double sum_score = 0.0;

            /* prepare a base state for this candidate: copy snapshot into sim buffers */
            for (int s = 0; s < sims_per_candidate; s++) {
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);

                /* Apply candidate move for our player */
                int immediate = sim_apply_move(board_sim, gwidth, gheight, players_sim, gplayer_count, my_index, cand);
                if (immediate < 0) {
                    /* shouldn't happen, but treat as 0 and continue */
                    players_sim[my_index].blocked = true;
                }

                /* next player is (my_index + 1) % player_count */
                int next = (my_index + 1) % gplayer_count;

                /* Simulate to the end */
                simulate_playout(board_sim, gwidth, gheight, players_sim, gplayer_count, next);

                sum_score += (double)players_sim[my_index].score;
            }

            double avg = sum_score / (double)sims_per_candidate;
            if (avg > best_avg) {
                best_avg = avg; best_dirs_count = 0; best_dirs[best_dirs_count++] = cand;
            } else if (avg == best_avg) {
                best_dirs[best_dirs_count++] = cand;
            }
        }

        /* tie-break: prefer candidate with largest immediate reward */
        int pick = best_dirs[rand() % best_dirs_count];
        int pick_tx, pick_ty;
        target_from_dir(gx, gy, pick, &pick_tx, &pick_ty);
        int immediate_val = board_snapshot[pick_ty * gwidth + pick_tx];
        for (int i = 0; i < best_dirs_count; i++) {
            int d = best_dirs[i];
            int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
            int v = board_snapshot[ty * gwidth + tx];
            if (v > immediate_val) { immediate_val = v; pick = d; }
        }

        unsigned char move = (unsigned char)pick;

        /* Now acquire state_mutex again and write the chosen move while holding it. */
        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
            break;
        }

        if (game_state->game_over) {
            sem_post(&game_sync->state_mutex);
            break;
        }

        /* final sanity: make sure we still are at the same position and not blocked */
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
            /* state changed unexpectedly; don't write (skip this turn) */
            sem_post(&game_sync->state_mutex);
            continue;
        }

        ssize_t written = write(STDOUT_FILENO, &move, 1);
        sem_post(&game_sync->state_mutex);

        if (written != 1) {
            if (written == -1 && errno == EPIPE) break;
            break;
        }
    }

    free(board_snapshot); free(board_sim); free(players_snapshot); free(players_sim);
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

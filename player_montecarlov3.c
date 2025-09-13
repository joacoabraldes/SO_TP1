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
#include <float.h>
#include <sys/time.h>

/*
 * player_montecarlov3.c
 *
 * Faster, smarter Monte‑Carlo player with an opening fast-eval phase and a
 * time-limited, allocation-free simulation loop. Key improvements:
 *  - Opening-phase fast heuristic: when the board is still "crowded" the
 *    agent uses a cheap static evaluator (immediate reward + local potential)
 *    which is very fast and avoids doing expensive sims while all players are
 *    still active.
 *  - Time-budgeted Monte-Carlo: instead of a fixed simulation count we run
 *    playouts until a small per-move time budget elapses (configurable via
 *    PLAYER_TIME_MS env var). This prevents long blocking at start and adapts
 *    to master delay settings.
 *  - No malloc in the inner loops: all sim buffers are allocated once at
 *    startup and reused to avoid fragmentation and startup pauses.
 *  - Fast xorshift RNG for playouts.
 *  - Opponent-aware lightweight playout policy that prefers reward+liberties.
 *
 * Note: still not a provably-winning algorithm; it trades theoretical
 * guarantees for practical speed and much better early-game responsiveness.
 */

/* small fast RNG (xorshift32) */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x1234567u;
    return *state;
}

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

typedef struct {
    int x, y;
    unsigned int score;
    bool blocked;
} sim_player_t;

/* fast check valid */
static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x, gy = players[pid].y;
    int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return false;
    int cell = board[ty * width + tx];
    return cell > 0;
}

/* apply move (no safety checks beyond bounds) */
static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x, gy = players[pid].y;
    int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
    int idx = ty * width + tx;
    int reward = board[idx];
    if (reward <= 0) return -1;
    players[pid].score += (unsigned int)reward;
    board[idx] = -(pid + 1);
    players[pid].x = tx; players[pid].y = ty; players[pid].blocked = false;
    return reward;
}

/* count free neighbors (liberties) */
static inline int count_liberties_fast(int *board, int width, int height, sim_player_t *players, int pid) {
    int gx = players[pid].x, gy = players[pid].y, c = 0;
    for (int d = 0; d < 8; d++) {
        int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        if (board[ty * width + tx] > 0) c++;
    }
    return c;
}

/* lightweight playout policy used for opponents and sometimes for fast sims.
   score = immediate_reward + lambda * liberties_after_move */
static inline int pick_policy_move_fast(int *board, int width, int height, sim_player_t *players, int player_count, int pid, uint32_t *rng) {
    int best_dirs[8]; int best_count = 0; double best_val = -DBL_MAX;
    for (int d = 0; d < 8; d++) {
        int tx, ty; target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        int idx = ty * width + tx;
        int cell = board[idx]; if (cell <= 0) continue;
        /* temporarily mark and compute liberties */
        board[idx] = -(pid + 1);
        int oldx = players[pid].x, oldy = players[pid].y;
        players[pid].x = tx; players[pid].y = ty;
        int lib = count_liberties_fast(board, width, height, players, pid);
        players[pid].x = oldx; players[pid].y = oldy;
        board[idx] = cell;
        double val = (double)cell + 1.2 * (double)lib;
        if (val > best_val) { best_val = val; best_count = 0; best_dirs[best_count++] = d; }
        else if (val == best_val) best_dirs[best_count++] = d;
    }
    if (best_count == 0) return -1;
    /* small randomness */
    if ((xorshift32(rng) & 0xFF) < 30) return best_dirs[xorshift32(rng) % best_count];
    return best_dirs[xorshift32(rng) % best_count];
}

/* simulate until terminal or until a small depth limit (to keep sims fast) */
static void run_fast_playout(int *board, int width, int height, sim_player_t *players, int player_count, int next_player, uint32_t *rng, int depth_limit) {
    int next = next_player;
    int iter = 0;
    while (1) {
        bool any = false;
        for (int i = 0; i < player_count; i++) {
            if (!players[i].blocked) { any = true; break; }
        }
        if (!any) break;
        if (iter++ >= depth_limit) break; /* limit to keep playouts short */
        int p = next; next = (next + 1) % player_count;
        if (players[p].blocked) continue;
        int mv = pick_policy_move_fast(board, width, height, players, player_count, p, rng);
        if (mv == -1) { players[p].blocked = true; continue; }
        sim_apply_move(board, width, height, players, p, mv);
    }
}

/* small helper to copy board and players into sim buffers (fast memcpy) */
static inline void copy_board_int(int *dst, int *src, int n) { memcpy(dst, src, n * sizeof(int)); }
static inline void copy_players_sim(sim_player_t *dst, player_t *src, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        dst[i].x = (int)src[i].x; dst[i].y = (int)src[i].y; dst[i].score = src[i].score; dst[i].blocked = src[i].blocked;
    }
}

/* quick static evaluator used in opening phase: immediate reward + small neighborhood potential */
static inline double opening_eval(int *board, int width, int height, int gx, int gy, int d) {
    int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return -DBL_MAX;
    int idx = ty * width + tx; int cell = board[idx]; if (cell <= 0) return -DBL_MAX;
    int neigh_sum = 0;
    for (int dd = 0; dd < 8; dd++) {
        int nx, ny; target_from_dir(tx, ty, dd, &nx, &ny);
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
        int v = board[ny * width + nx]; if (v > 0) neigh_sum += v;
    }
    /* immediate reward dominates; neighbor sum is secondary */
    return (double)cell + 0.25 * (double)neigh_sum;
}

int main(int argc, char *argv[]) {
    if (argc != 3) { fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]); return EXIT_FAILURE; }
    int width = atoi(argv[1]), height = atoi(argv[2]);
    size_t state_size = sizeof(game_state_t) + (size_t)width * height * sizeof(int);
    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, state_size, 0);
    if (!state_mgr) { perror("shm_manager_open state"); return EXIT_FAILURE; }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);
    if (!game_state) { fprintf(stderr, "failed to get game_state pointer\n"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    if (!game_sync) { fprintf(stderr, "failed to get game_sync pointer\n"); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

    int my_index = -1; const int max_iters = 500; int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) { my_index = find_my_index(game_state, game_sync); if (my_index != -1) break; struct timespec short_sleep = {0, 10 * 1000 * 1000}; nanosleep(&short_sleep, NULL); it++; }
    if (my_index == -1) my_index = find_my_index(game_state, game_sync);
    if (my_index == -1) { fprintf(stderr, "player: couldn't determine my index (pid %d)\n", (int)getpid()); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

    /* read optional env var for time budget in milliseconds */
    int time_budget_ms = 120; /* default per-move budget */
    char *env = getenv("PLAYER_TIME_MS"); if (env) { int t = atoi(env); if (t > 10) time_budget_ms = t; }

    uint32_t rng_state = (uint32_t)(getpid() ^ (unsigned int)time(NULL));

    /* allocate sim buffers once */
    int cells = width * height;
    int *board_snapshot = malloc(cells * sizeof(int));
    int *board_sim = malloc(cells * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    sim_player_t *players_sim = malloc(sizeof(sim_player_t) * game_state->player_count);
    if (!board_snapshot || !board_sim || !players_snapshot || !players_sim) { fprintf(stderr, "allocation failed\n"); return EXIT_FAILURE; }

    while (1) {
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) { if (errno == EINTR) continue; break; }
        if (game_state->game_over) break; if (game_state->players[my_index].blocked) break;

        if (sem_wait(&game_sync->state_mutex) == -1) { if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; } break; }
        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }

        /* snapshot */
        int gx = (int)game_state->players[my_index].x;
        int gy = (int)game_state->players[my_index].y;
        int gwidth = game_state->width; int gheight = game_state->height; unsigned int gplayer_count = game_state->player_count;
        copy_board_int(board_snapshot, game_state->board, cells);
        copy_players_sim(players_snapshot, game_state->players, gplayer_count);

        /* count free cells to detect opening phase */
        int free_cells = 0; for (int i = 0; i < cells; i++) if (board_snapshot[i] > 0) free_cells++;
        sem_post(&game_sync->state_mutex);

        /* gather valid moves */
        int valid_dirs[8]; int valid_count = 0;
        for (int d = 0; d < 8; d++) { int tx, ty; target_from_dir(gx, gy, d, &tx, &ty); if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) continue; int cell = board_snapshot[ty * gwidth + tx]; if (cell <= 0) continue; valid_dirs[valid_count++] = d; }
        if (valid_count == 0) continue;

        /* Opening phase heuristic: when many free cells remain -> cheap eval */
        int total_cells = gwidth * gheight;
        int opening_threshold = (int)(total_cells * 0.55); /* tune: when >55% free use opening */
        int pick = -1;
        if (free_cells >= opening_threshold) {
            double best_val = -DBL_MAX; int bests[8]; int bestc = 0;
            for (int i = 0; i < valid_count; i++) {
                int d = valid_dirs[i]; double v = opening_eval(board_snapshot, gwidth, gheight, gx, gy, d);
                if (v > best_val) { best_val = v; bestc = 0; bests[bestc++] = d; }
                else if (v == best_val) bests[bestc++] = d;
            }
            pick = bests[xorshift32(&rng_state) % bestc];
            /* send move quickly: reacquire state_mutex, sanity-check and write while holding it */
            if (sem_wait(&game_sync->state_mutex) == -1) { if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; } break; }
            if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }
            if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) { sem_post(&game_sync->state_mutex); continue; }
            unsigned char out = (unsigned char)pick; ssize_t w = write(STDOUT_FILENO, &out, 1); sem_post(&game_sync->state_mutex); if (w != 1) { if (w == -1 && errno == EPIPE) break; break; } continue;
        }

        /* Monte-Carlo phase: time-limited sims, no per-sim allocations */
        struct timespec tstart, tnow; clock_gettime(CLOCK_MONOTONIC, &tstart);
        double time_budget = (double)time_budget_ms / 1000.0; /* seconds */
        double elapsed = 0.0; int sims = 0;

        double best_avg = -DBL_MAX; int bests[8]; int bestc = 0;
        double sums[8]; for (int i = 0; i < valid_count; i++) sums[i] = 0.0;

        /* run sims until budget exhausted */
        while (1) {
            /* iterate candidates and run one sim per candidate in round-robin to keep progress even */
            for (int ci = 0; ci < valid_count; ci++) {
                copy_board_int(board_sim, board_snapshot, cells);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                int cand = valid_dirs[ci]; sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                int next = (my_index + 1) % gplayer_count;
                /* depth limit shorter when many players to keep sims fast */
                int depth_limit = 20; if (gplayer_count >= 6) depth_limit = 12; if (gplayer_count >= 8) depth_limit = 8;
                run_fast_playout(board_sim, gwidth, gheight, players_sim, gplayer_count, next, &rng_state, depth_limit);
                sums[ci] += (double)players_sim[my_index].score;
                sims++;
                /* check time */
                clock_gettime(CLOCK_MONOTONIC, &tnow);
                elapsed = (tnow.tv_sec - tstart.tv_sec) + (tnow.tv_nsec - tstart.tv_nsec) / 1e9;
                if (elapsed >= time_budget) break;
            }
            if (elapsed >= time_budget) break;
            /* safety cap */
            if (sims > 20000) break;
        }

        for (int ci = 0; ci < valid_count; ci++) {
            double avg = sums[ci] / (double)(sims / valid_count + 1e-9);
            if (avg > best_avg) { best_avg = avg; bestc = 0; bests[bestc++] = valid_dirs[ci]; }
            else if (avg == best_avg) bests[bestc++] = valid_dirs[ci];
        }
        pick = bests[xorshift32(&rng_state) % bestc];

        /* final write under state mutex */
        if (sem_wait(&game_sync->state_mutex) == -1) { if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; } break; }
        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) { sem_post(&game_sync->state_mutex); continue; }
        unsigned char out = (unsigned char)pick; ssize_t w = write(STDOUT_FILENO, &out, 1); sem_post(&game_sync->state_mutex); if (w != 1) { if (w == -1 && errno == EPIPE) break; break; }
    }

    free(board_snapshot); free(board_sim); free(players_snapshot); free(players_sim);
    shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

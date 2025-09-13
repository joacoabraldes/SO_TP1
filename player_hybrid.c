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
#include <math.h>

/*
 * player_hybrid.c
 *
 * Hybrid MonteCarlo + heuristic player (improved).
 * - realistic opponent model in playouts (sim_opponent_policy)
 * - simulate_playout accepts my_index so my policy differs from opponents
 * - Voronoi tie-break considers (my_vor - max_other_vor)
 * - adaptive simulation budget increases on dense boards
 *
 * Compile:
 *   gcc -Wall -Wextra -std=c11 -pedantic -g -D_XOPEN_SOURCE=700 player_hybrid.c shm_manager.c -o player_hybrid -pthread -lrt -lm
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
        case UP:        ny--; break;
        case UP_RIGHT:  ny--; nx++; break;
        case RIGHT:     nx++; break;
        case DOWN_RIGHT:ny++; nx++; break;
        case DOWN:      ny++; break;
        case DOWN_LEFT: ny++; nx--; break;
        case LEFT:      nx--; break;
        case UP_LEFT:   ny--; nx--; break;
        default: break;
    }
    *tx = nx; *ty = ny;
}

typedef struct { int x,y; unsigned int score; bool blocked; } sim_player_t;

/* ---------- basic sim helpers ---------- */

static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid, int d) {
    (void)player_count;
    int gx = players[pid].x, gy = players[pid].y, tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return false;
    return board[ty*width + tx] > 0;
}

static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid, int d) {
    (void)player_count;
    int gx = players[pid].x, gy = players[pid].y, tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return -1;
    int idx = ty*width + tx;
    int reward = board[idx];
    if (reward <= 0) return -1;
    players[pid].score += (unsigned int)reward;
    board[idx] = -(pid+1);
    players[pid].x = tx;
    players[pid].y = ty;
    players[pid].blocked = false;
    return reward;
}

static bool sim_any_player_has_move(int *board, int width, int height, sim_player_t *players, int player_count) {
    for (int i = 0; i < player_count; i++) {
        if (players[i].blocked) continue;
        for (int d = 0; d < 8; d++) {
            if (sim_is_valid_move(board, width, height, players, player_count, i, d)) return true;
        }
    }
    return false;
}

static int sim_count_liberties(int *board, int width, int height, sim_player_t *players, int pid) {
    int gx = players[pid].x, gy = players[pid].y, c = 0;
    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(gx, gy, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        if (board[ty*width + tx] > 0) c++;
    }
    return c;
}

/* playout policy for "me" used in simulate_playout */
static int sim_pick_policy_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid) {
    (void)player_count;
    int valid_dirs[8], valid_count = 0;
    int best_dirs[8], best_count = 0;
    double best_score = -DBL_MAX;

    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        int cell = board[ty*width + tx];
        if (cell <= 0) continue;
        valid_dirs[valid_count++] = d;
    }
    if (valid_count == 0) return -1;
    if ((rand() & 0xFF) < 40) return valid_dirs[rand() % valid_count];

    for (int i = 0; i < valid_count; i++) {
        int d = valid_dirs[i];
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        int saved = board[ty*width + tx];
        /* mark temporarily */
        board[ty*width + tx] = -(pid+1);
        int oldx = players[pid].x, oldy = players[pid].y;
        players[pid].x = tx; players[pid].y = ty;
        int lib = sim_count_liberties(board, width, height, players, pid);
        players[pid].x = oldx; players[pid].y = oldy;
        board[ty*width + tx] = saved;
        double score = (double)saved + 1.2 * (double)lib;
        if (lib == 0) score -= 2000.0;
        if (score > best_score) { best_score = score; best_count = 0; best_dirs[best_count++] = d; }
        else if (score == best_score) best_dirs[best_count++] = d;
    }
    return best_dirs[rand() % best_count];
}

/* Voronoi routine (buffers external) */
static void compute_voronoi_potential_buf(int *board, int width, int height, sim_player_t *players, int player_count,
                                          unsigned int *vor_out, int *dist, int *owner, int *qx, int *qy, int *qo) {
    int n = width * height;
    for (int i = 0; i < n; i++) { dist[i] = INT_MAX; owner[i] = -1; }
    int qh = 0, qt = 0;
    for (int p = 0; p < player_count; p++) {
        if (players[p].blocked) continue;
        int x = players[p].x, y = players[p].y;
        int idx = y * width + x;
        dist[idx] = 0; owner[idx] = p; qx[qt] = x; qy[qt] = y; qo[qt] = p; qt++;
    }
    while (qh < qt) {
        int x = qx[qh], y = qy[qh], p = qo[qh]; qh++;
        int base = y * width + x;
        int dcur = dist[base];
        for (int dir = 0; dir < 8; dir++) {
            int nx, ny;
            target_from_dir(x, y, dir, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int nidx = ny * width + nx;
            if (board[nidx] <= 0) continue;
            int nd = dcur + 1;
            if (nd < dist[nidx]) {
                dist[nidx] = nd; owner[nidx] = p; qx[qt] = nx; qy[qt] = ny; qo[qt] = p; qt++;
            } else if (nd == dist[nidx] && owner[nidx] != p) {
                owner[nidx] = -2;
            }
        }
    }
    for (int p = 0; p < player_count; p++) vor_out[p] = 0u;
    for (int i = 0; i < n; i++) {
        if (board[i] <= 0) continue;
        int o = owner[i];
        if (o >= 0) vor_out[o] += (unsigned int)board[i];
    }
}

static void copy_board(int *dst, int *src, int n) { memcpy(dst, src, n * sizeof(int)); }
static void copy_players_sim(sim_player_t *dst, player_t *src, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        dst[i].x = (int)src[i].x;
        dst[i].y = (int)src[i].y;
        dst[i].score = src[i].score;
        dst[i].blocked = src[i].blocked;
    }
}

/* ---------- opponent model for playouts ---------- */
static int sim_opponent_policy(int *board, int width, int height,
                               sim_player_t *players, int player_count, int pid) {
    if (players[pid].blocked) return -1;
    int best_dirs[8], best_count = 0;
    double best_score = -DBL_MAX;
    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        int cell = board[ty * width + tx];
        if (cell <= 0) continue;
        int neigh_sum = 0;
        for (int dd = 0; dd < 8; dd++) {
            int nx, ny;
            target_from_dir(tx, ty, dd, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int v = board[ny * width + nx];
            if (v > 0) neigh_sum += v;
        }
        int saved = board[ty * width + tx];
        board[ty * width + tx] = -(pid + 1);
        int oldx = players[pid].x, oldy = players[pid].y;
        players[pid].x = tx; players[pid].y = ty;
        int lib = sim_count_liberties(board, width, height, players, pid);
        players[pid].x = oldx; players[pid].y = oldy;
        board[ty * width + tx] = saved;
        double score = (double)cell + 0.5 * (double)neigh_sum + 1.2 * (double)lib;
        if (lib == 0) score -= 1000.0;
        if (score > best_score) { best_score = score; best_count = 0; best_dirs[best_count++] = d; }
        else if (score == best_score) best_dirs[best_count++] = d;
    }
    if (best_count == 0) return -1;
    return best_dirs[rand() % best_count];
}

/* simulate_playout now differentiates my policy and opponents */
static void simulate_playout(int *board, int width, int height,
                             sim_player_t *players, int player_count,
                             int start_next_player, int my_index) {
    int next = start_next_player;
    while (sim_any_player_has_move(board, width, height, players, player_count)) {
        int p = next; next = (next + 1) % player_count;
        if (players[p].blocked) continue;
        int mv;
        if (p == my_index) mv = sim_pick_policy_move(board, width, height, players, player_count, p);
        else mv = sim_opponent_policy(board, width, height, players, player_count, p);
        if (mv == -1) { players[p].blocked = true; continue; }
        sim_apply_move(board, width, height, players, player_count, p, mv);
    }
}

/* small expansion heuristic used earlier */
static double calculate_expansion_potential(int *board, int width, int height, sim_player_t *players, int pid, int direction) {
    int gx = players[pid].x, gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, direction, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return -1.0;
    int cell_value = board[ty * width + tx];
    if (cell_value <= 0) return -1.0;
    int accessible_cells = 0;
    for (int d = 0; d < 8; d++) {
        int nx, ny;
        target_from_dir(tx, ty, d, &nx, &ny);
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
        if (board[ny * width + nx] > 0) accessible_cells++;
    }
    return (double)cell_value + 0.3 * (double)accessible_cells;
}

/* ---------- main ---------- */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <width> <height>\n", argv[0]);
        return EXIT_FAILURE;
    }
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

    int my_index = -1;
    const int max_iters = 500;
    int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) {
        my_index = find_my_index(game_state, game_sync);
        if (my_index != -1) break;
        struct timespec short_sleep = {0, 10 * 1000 * 1000};
        nanosleep(&short_sleep, NULL);
        it++;
    }
    if (my_index == -1) my_index = find_my_index(game_state, game_sync);
    if (my_index == -1) {
        fprintf(stderr, "player: couldn't determine my index (pid %d)\n", (int)getpid());
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    srand((unsigned int)(getpid() ^ time(NULL)));

    int cells = width * height;
    int *board_snapshot = malloc(cells * sizeof(int));
    int *board_sim = malloc(cells * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    sim_player_t *players_sim = malloc(sizeof(sim_player_t) * game_state->player_count);
    unsigned int *vor_tmp = malloc(sizeof(unsigned int) * game_state->player_count);
    int *dist = malloc(sizeof(int) * cells);
    int *owner = malloc(sizeof(int) * cells);
    int *qx = malloc(sizeof(int) * cells);
    int *qy = malloc(sizeof(int) * cells);
    int *qo = malloc(sizeof(int) * cells);

    if (!board_snapshot || !board_sim || !players_snapshot || !players_sim || !vor_tmp || !dist || !owner || !qx || !qy || !qo) {
        fprintf(stderr, "allocation failed\n");
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    /* base MonteCarlo budget per decision (can tune) */
    const int sims_budget = 400;

    while (1) {
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (game_state->game_over) break;
        if (game_state->players[my_index].blocked) break;

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

        copy_board(board_snapshot, game_state->board, gwidth * gheight);
        copy_players_sim(players_snapshot, game_state->players, gplayer_count);

        sem_post(&game_sync->state_mutex);

        int valid_dirs[8], valid_count = 0;
        int immediate_vals[8];
        double expansion_potential[8];
        for (int d = 0; d < 8; d++) {
            int tx, ty;
            target_from_dir(gx, gy, d, &tx, &ty);
            if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) continue;
            int cell = board_snapshot[ty * gwidth + tx];
            if (cell <= 0) continue;
            valid_dirs[valid_count] = d;
            immediate_vals[valid_count] = cell;
            expansion_potential[valid_count] = calculate_expansion_potential(board_snapshot, gwidth, gheight, players_snapshot, my_index, d);
            valid_count++;
        }
        if (valid_count == 0) {
            continue;
        }

        /* opening-phase cheap evaluator */
        int free_cells = 0;
        for (int i = 0; i < cells; i++) if (board_snapshot[i] > 0) free_cells++;
        int opening_threshold = (int)(cells * 0.55);
        if (free_cells >= opening_threshold) {
            double bestv = -DBL_MAX; int bests[8], bc = 0;
            for (int i = 0; i < valid_count; i++) {
                int d = valid_dirs[i];
                int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
                int neigh_sum = 0;
                for (int dd = 0; dd < 8; dd++) {
                    int nx, ny; target_from_dir(tx, ty, dd, &nx, &ny);
                    if (nx < 0 || nx >= gwidth || ny < 0 || ny >= gheight) continue;
                    int v = board_snapshot[ny * gwidth + nx];
                    if (v > 0) neigh_sum += v;
                }
                int saved = board_snapshot[ty * gwidth + tx];
                int oldx = players_snapshot[my_index].x, oldy = players_snapshot[my_index].y;
                board_snapshot[ty * gwidth + tx] = -(my_index + 1);
                players_snapshot[my_index].x = tx; players_snapshot[my_index].y = ty;
                int lib = sim_count_liberties(board_snapshot, gwidth, gheight, players_snapshot, my_index);
                players_snapshot[my_index].x = oldx; players_snapshot[my_index].y = oldy;
                board_snapshot[ty * gwidth + tx] = saved;
                double score = (double)immediate_vals[i] + 0.25 * (double)neigh_sum + 1.5 * (double)lib;
                if (lib == 0) score -= 1000.0;
                if (score > bestv) { bestv = score; bc = 0; bests[bc++] = d; }
                else if (score == bestv) bests[bc++] = d;
            }
            int pick = bests[rand() % bc];
            if (sem_wait(&game_sync->state_mutex) == -1) {
                if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
                break;
            }
            if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }
            if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
                sem_post(&game_sync->state_mutex); continue;
            }
            unsigned char mv = (unsigned char)pick;
            ssize_t w = write(STDOUT_FILENO, &mv, 1);
            sem_post(&game_sync->state_mutex);
            if (w != 1) { if (w == -1 && errno == EPIPE) break; break; }
            continue;
        }

        /* mid/endgame: top-K + MonteCarlo with opponent model + Voronoi delta */
        int K = 4; if (valid_count < K) K = valid_count;
        int idxs[8]; for (int i = 0; i < valid_count; i++) idxs[i] = i;
        for (int i = 0; i < K; i++) {
            int best = i;
            for (int j = i + 1; j < valid_count; j++)
                if (immediate_vals[idxs[j]] > immediate_vals[idxs[best]]) best = j;
            int tmp = idxs[i]; idxs[i] = idxs[best]; idxs[best] = tmp;
        }

        int board_cells = gwidth * gheight;
        int sims_per_candidate;
        if (board_cells <= 25) sims_per_candidate = 600;
        else if (board_cells <= 100) sims_per_candidate = 350;
        else if (board_cells <= 400) sims_per_candidate = 140;
        else sims_per_candidate = 70;

        /* scale sims by free ratio (more sims if dense board) */
        float free_ratio = (float)free_cells / (float)cells;
        /* if board is dense (low free_ratio) increase sims */
        if (free_ratio < 0.4f) sims_per_candidate = sims_per_candidate * 2;
        if (free_ratio < 0.2f) sims_per_candidate = sims_per_candidate * 3;
        if (sims_per_candidate > 2000) sims_per_candidate = 2000;
        if (sims_per_candidate < 10) sims_per_candidate = 10;

        double best_avg = -DBL_MAX;
        int bests2[8]; int bestc2 = 0;
        double candidate_avgs[8];
        for (int i = 0; i < valid_count; i++) candidate_avgs[i] = (double)immediate_vals[i];

        for (int t = 0; t < K; t++) {
            int ci = idxs[t]; int cand = valid_dirs[ci];
            double sum_score = 0.0;
            for (int s = 0; s < sims_per_candidate; s++) {
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                int immediate = sim_apply_move(board_sim, gwidth, gheight, players_sim, gplayer_count, my_index, cand);
                if (immediate < 0) players_sim[my_index].blocked = true;
                int next = (my_index + 1) % gplayer_count;
                simulate_playout(board_sim, gwidth, gheight, players_sim, gplayer_count, next, my_index);
                sum_score += (double)players_sim[my_index].score;
            }
            double avg = sum_score / (double)sims_per_candidate;
            candidate_avgs[ci] = avg;
            if (avg > best_avg) { best_avg = avg; bestc2 = 0; bests2[bestc2++] = cand; }
            else if (avg == best_avg) bests2[bestc2++] = cand;
        }

        int pick = bests2[rand() % bestc2];

        /* Voronoi tie-break among tied candidates: use delta (my_vor - max_other_vor) */
        if (bestc2 > 1) {
            double best_combined = -DBL_MAX;
            int topk = bestc2; if (topk > 4) topk = 4;
            for (int t = 0; t < topk; t++) {
                int cand = bests2[t];
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                sim_apply_move(board_sim, gwidth, gheight, players_sim, gplayer_count, my_index, cand);
                compute_voronoi_potential_buf(board_sim, gwidth, gheight, players_sim, gplayer_count, vor_tmp, dist, owner, qx, qy, qo);
                double my_vor = (double)vor_tmp[my_index];
                unsigned int max_other = 0;
                for (unsigned int p = 0; p < gplayer_count; p++) if ((int)p != my_index && vor_tmp[p] > max_other) max_other = vor_tmp[p];
                double gamma = 0.035;
                /* find index of candidate to lookup avg: find ci */
                double avg_for_candidate = -DBL_MAX;
                for (int i = 0; i < valid_count; i++) if (valid_dirs[i] == cand) { avg_for_candidate = candidate_avgs[i]; break; }
                double combined = avg_for_candidate + gamma * (my_vor - (double)max_other);
                if (combined > best_combined) { best_combined = combined; pick = cand; }
            }
        }

        /* final anti-suicide check and fallback */
        int tx, ty;
        target_from_dir(gx, gy, pick, &tx, &ty);
        int saved = board_snapshot[ty * gwidth + tx];
        int oldx = players_snapshot[my_index].x, oldy = players_snapshot[my_index].y;
        board_snapshot[ty * gwidth + tx] = -(my_index + 1);
        players_snapshot[my_index].x = tx; players_snapshot[my_index].y = ty;
        int lib = sim_count_liberties(board_snapshot, gwidth, gheight, players_snapshot, my_index);
        players_snapshot[my_index].x = oldx; players_snapshot[my_index].y = oldy;
        board_snapshot[ty * gwidth + tx] = saved;
        if (lib == 0) {
            int best_alt = -1; double best_alt_score = -DBL_MAX;
            for (int i = 0; i < valid_count; i++) {
                int d = valid_dirs[i];
                int tx2, ty2; target_from_dir(gx, gy, d, &tx2, &ty2);
                int saved2 = board_snapshot[ty2 * gwidth + tx2];
                int oldx2 = players_snapshot[my_index].x, oldy2 = players_snapshot[my_index].y;
                board_snapshot[ty2 * gwidth + tx2] = -(my_index + 1);
                players_snapshot[my_index].x = tx2; players_snapshot[my_index].y = ty2;
                int lib2 = sim_count_liberties(board_snapshot, gwidth, gheight, players_snapshot, my_index);
                players_snapshot[my_index].x = oldx2; players_snapshot[my_index].y = oldy2;
                board_snapshot[ty2 * gwidth + tx2] = saved2;
                if (lib2 > 0) {
                    double score = candidate_avgs[i];
                    if (score > best_alt_score) { best_alt_score = score; best_alt = d; }
                }
            }
            if (best_alt != -1) pick = best_alt;
        }

        unsigned char move = (unsigned char)pick;
        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
            break;
        }
        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
            sem_post(&game_sync->state_mutex); continue;
        }
        ssize_t written = write(STDOUT_FILENO, &move, 1);
        sem_post(&game_sync->state_mutex);
        if (written != 1) { if (written == -1 && errno == EPIPE) break; break; }
    }

    free(board_snapshot); free(board_sim); free(players_snapshot); free(players_sim);
    free(vor_tmp); free(dist); free(owner); free(qx); free(qy); free(qo);
    shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

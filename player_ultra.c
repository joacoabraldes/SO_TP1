#define _XOPEN_SOURCE 700
#include "common.h"
#include "shm_manager.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>

/* Player "ULTRA" - heuristic + short playouts, robust vs trapper/montecarlo */

/* ---------- helpers ---------- */

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

/* BFS flood-fill bounded: devuelve suma y cantidad de celdas alcanzables desde sx,sy
   Considera ocupadas (<=0) como bloqueadas. max_nodes limita la exploración. */
static void flood_bounded_sum(int *board, int width, int height, int sx, int sy, int max_nodes,
                              int *out_sum, int *out_count) {
    int n = width * height;
    *out_sum = 0; *out_count = 0;
    if (sx < 0 || sx >= width || sy < 0 || sy >= height) return;
    int start = sy * width + sx;
    if (board[start] <= 0) return;

    int *vis = calloc(n, sizeof(int));
    if (!vis) return;
    int *q = malloc(sizeof(int) * max_nodes);
    if (!q) { free(vis); return; }

    int head = 0, tail = 0;
    q[tail++] = start; vis[start] = 1;
    int sum = 0, cnt = 0;
    while (head < tail && cnt < max_nodes) {
        int idx = q[head++];
        sum += board[idx];
        cnt++;
        int r = idx / width, c = idx % width;
        for (int d = 0; d < 8; d++) {
            int nx, ny; target_from_dir(c, r, d, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int nidx = ny * width + nx;
            if (vis[nidx]) continue;
            if (board[nidx] <= 0) continue;
            vis[nidx] = 1;
            if (tail < max_nodes) q[tail++] = nidx;
        }
    }
    free(q);
    free(vis);
    *out_sum = sum;
    *out_count = cnt;
}

/* distancia mínima desde cualquier cabeza enemiga a cualquier celda del conjunto
   cells_mask = array de n ints donde 1 indica celda de interés.
   Si cells_mask es NULL, calcula dist mínima desde enemigos a coordenada (sx,sy).
*/
static int min_dist_from_opponents(int *board, int width, int height,
                                   int *cells_mask, int sx, int sy,
                                   player_t *players, unsigned int pcount, int my_index,
                                   int max_limit) {
    int n = width * height;
    /* BFS desde todos los enemigos (multi-source) */
    int *dist = malloc(sizeof(int) * n);
    if (!dist) return max_limit;
    for (int i = 0; i < n; i++) dist[i] = INT_MAX;
    int *q = malloc(sizeof(int) * n);
    if (!q) { free(dist); return max_limit; }
    int head = 0, tail = 0;
    for (unsigned int p = 0; p < pcount; p++) {
        if ((int)p == my_index) continue;
        if (players[p].blocked) continue;
        int sx_p = players[p].x, sy_p = players[p].y;
        int sidx = sy_p * width + sx_p;
        dist[sidx] = 0;
        q[tail++] = sidx;
    }
    int best = INT_MAX;
    while (head < tail) {
        int idx = q[head++]; int dcur = dist[idx];
        if (dcur >= best) continue;
        if (cells_mask) {
            if (cells_mask[idx]) { best = dcur; break; }
        } else {
            int r = idx / width, c = idx % width;
            if (c == sx && r == sy) { best = dcur; break; }
        }
        int r = idx / width, c = idx % width;
        for (int dir = 0; dir < 8; dir++) {
            int nx, ny; target_from_dir(c, r, dir, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int nidx = ny * width + nx;
            if (board[nidx] <= 0) continue;
            if (dist[nidx] > dcur + 1) {
                dist[nidx] = dcur + 1;
                if (dist[nidx] <= max_limit) q[tail++] = nidx;
            }
        }
    }
    free(q);
    free(dist);
    if (best == INT_MAX) return max_limit;
    return best;
}

/* ---------- simulation helpers (short playouts) ---------- */

typedef struct { int x,y; unsigned int score; bool blocked; } sim_player_t;

static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x, gy = players[pid].y, tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return false;
    int cell = board[ty * width + tx];
    return cell > 0;
}

static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x, gy = players[pid].y, tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return -1;
    int idx = ty * width + tx;
    int reward = board[idx];
    if (reward <= 0) return -1;
    players[pid].score += (unsigned int)reward;
    board[idx] = -(pid + 1);
    players[pid].x = tx;
    players[pid].y = ty;
    players[pid].blocked = false;
    return reward;
}

/* --- CORRECTED: sim_any_player_has_move (missing before) --- */
static bool sim_any_player_has_move(int *board, int width, int height, sim_player_t *players, int player_count) {
    for (int i = 0; i < player_count; i++) {
        if (players[i].blocked) continue;
        for (int d = 0; d < 8; d++) {
            if (sim_is_valid_move(board, width, height, players, i, d)) return true;
        }
    }
    return false;
}

/* opponent realistic greedy: maximize (value + 0.3 * liberties) */
static int opponent_greedy_choose(int *board, int width, int height, sim_player_t *players, int pid) {
    int bestd = -1; double bestscore = -1e300;
    for (int d = 0; d < 8; d++) {
        if (!sim_is_valid_move(board, width, height, players, pid, d)) continue;
        int gx = players[pid].x, gy = players[pid].y, tx, ty;
        target_from_dir(gx, gy, d, &tx, &ty);
        int val = board[ty * width + tx];
        int lib = 0;
        for (int dd = 0; dd < 8; dd++) {
            int nx, ny; target_from_dir(tx, ty, dd, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            if (board[ny * width + nx] > 0) lib++;
        }
        double sc = (double)val + 0.3 * (double)lib;
        if (sc > bestscore) { bestscore = sc; bestd = d; }
    }
    return bestd;
}

/* my greedy policy in playouts: prefer value + mobility */
static int my_biased_choose(int *board, int width, int height, sim_player_t *players, int pid) {
    int bestd = -1; double bestscore = -1e300;
    for (int d = 0; d < 8; d++) {
        if (!sim_is_valid_move(board, width, height, players, pid, d)) continue;
        int gx = players[pid].x, gy = players[pid].y, tx, ty;
        target_from_dir(gx, gy, d, &tx, &ty);
        int val = board[ty * width + tx];
        int lib = 0;
        for (int dd = 0; dd < 8; dd++) {
            int nx, ny; target_from_dir(tx, ty, dd, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            if (board[ny * width + nx] > 0) lib++;
        }
        double sc = (double)val + 0.5 * (double)lib;
        if (sc > bestscore) { bestscore = sc; bestd = d; }
    }
    return bestd;
}

/* run a short playout from snapshot; opponents use opponent_greedy_choose */
static unsigned int run_short_playout(int *board_snap, int width, int height,
                                      sim_player_t *players_snap, int player_count,
                                      int next_player, int my_index, int max_steps) {
    int cells = width * height;
    int *board = malloc(cells * sizeof(int));
    sim_player_t *players = malloc(sizeof(sim_player_t) * player_count);
    if (!board || !players) { free(board); free(players); return players_snap[my_index].score; }
    memcpy(board, board_snap, cells * sizeof(int));
    memcpy(players, players_snap, sizeof(sim_player_t) * player_count);

    int steps = 0;
    int p = next_player;
    while (sim_any_player_has_move(board, width, height, players, player_count) && steps < max_steps) {
        if (!players[p].blocked) {
            int mv;
            if (p == my_index) mv = my_biased_choose(board, width, height, players, p);
            else mv = opponent_greedy_choose(board, width, height, players, p);
            if (mv == -1) players[p].blocked = true;
            else sim_apply_move(board, width, height, players, p, mv);
        }
        p = (p + 1) % player_count;
        steps++;
    }
    unsigned int res = players[my_index].score;
    free(board);
    free(players);
    return res;
}

/* ---------- main ---------- */

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

    /* find my index */
    int my_index = -1;
    const int max_iters = 500;
    int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) {
        my_index = find_my_index(game_state, game_sync);
        if (my_index != -1) break;
        struct timespec st = {0, 10 * 1000 * 1000}; nanosleep(&st, NULL);
        it++;
    }
    if (my_index == -1) my_index = find_my_index(game_state, game_sync);
    if (my_index == -1) {
        fprintf(stderr, "player: couldn't determine my index (pid %d)\n", (int)getpid());
        shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    srand((unsigned int)(getpid() ^ time(NULL)));

    int cells = width * height;
    int *board_snapshot = malloc(cells * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    if (!board_snapshot || !players_snapshot) {
        fprintf(stderr, "allocation failed\n");
        free(board_snapshot); free(players_snapshot);
        shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    /* parameters - you can tune these (trade speed vs strength) */
    const int MAX_BFS = 250;        /* max nodes for flood-fill */
    const int TOPK = 2;             /* top candidates to simulate */
    const int SIMS_PER_CAND = 40;   /* playouts per candidate (short) */
    const int PLAYOUT_STEPS = 120;  /* playout step cap */

    while (1) {
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (game_state->game_over) break;
        if (game_state->players[my_index].blocked) break;

        /* snapshot under mutex */
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

        /* copy board + players */
        memcpy(board_snapshot, game_state->board, cells * sizeof(int));
        for (unsigned int p = 0; p < gplayer_count; p++) {
            players_snapshot[p].x = (int)game_state->players[p].x;
            players_snapshot[p].y = (int)game_state->players[p].y;
            players_snapshot[p].score = game_state->players[p].score;
            players_snapshot[p].blocked = game_state->players[p].blocked;
        }

        /* gather valid moves */
        int valid_dirs[8]; int valid_count = 0;
        int immediate[8];
        for (int d = 0; d < 8; d++) {
            int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
            if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) continue;
            int cell = board_snapshot[ty * gwidth + tx];
            if (cell <= 0) continue;
            valid_dirs[valid_count] = d;
            immediate[valid_count] = cell;
            valid_count++;
        }
        if (valid_count == 0) { sem_post(&game_sync->state_mutex); continue; }

        /* quick heuristic score for each candidate */
        double heur[8];
        for (int i = 0; i < valid_count; i++) heur[i] = 0.0;

        for (int i = 0; i < valid_count; i++) {
            int d = valid_dirs[i];
            int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
            int rsum = 0, rcount = 0;
            flood_bounded_sum(board_snapshot, gwidth, gheight, tx, ty, MAX_BFS, &rsum, &rcount);
            /* liberties = neighbors free */
            int lib = 0;
            for (int dd = 0; dd < 8; dd++) {
                int nx, ny; target_from_dir(tx, ty, dd, &nx, &ny);
                if (nx < 0 || nx >= gwidth || ny < 0 || ny >= gheight) continue;
                if (board_snapshot[ny * gwidth + nx] > 0) lib++;
            }
            /* compute min dist from opponents to this target area (approx by checking target cell) */
            int mind = min_dist_from_opponents(board_snapshot, gwidth, gheight, NULL, tx, ty, game_state->players, gplayer_count, my_index, 9999);
            /* combine */
            double w_im = 1.0, w_reach = 0.95, w_lib = 0.6, w_dist = 2.0;
            heur[i] = w_im * (double)immediate[i] + w_reach * (double)rsum + w_lib * (double)lib - w_dist / (1.0 + (double)mind);
        }

        /* pick top-K by heur */
        int idxs[8]; for (int i = 0; i < valid_count; i++) idxs[i] = i;
        int K = TOPK; if (K > valid_count) K = valid_count;
        for (int i = 0; i < K; i++) {
            int best = i;
            for (int j = i+1; j < valid_count; j++) {
                if (heur[idxs[j]] > heur[idxs[best]]) best = j;
            }
            int tmp = idxs[i]; idxs[i] = idxs[best]; idxs[best] = tmp;
        }

        /* run short playouts for top K */
        double best_comb = -1e300;
        int best_move = valid_dirs[idxs[0]]; /* fallback */
        for (int t = 0; t < K; t++) {
            int ci = idxs[t];
            int cand = valid_dirs[ci];
            double sum = 0.0;
            /* prepare base snapshot arrays to reuse in playouts (but allocate inside each sim for safety) */
            for (int s = 0; s < SIMS_PER_CAND; s++) {
                int *board_sim = malloc(cells * sizeof(int));
                sim_player_t *players_sim = malloc(sizeof(sim_player_t) * gplayer_count);
                if (!board_sim || !players_sim) { free(board_sim); free(players_sim); continue; }
                memcpy(board_sim, board_snapshot, cells * sizeof(int));
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                /* apply candidate */
                sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                int nextp = (my_index + 1) % gplayer_count;
                unsigned int final_my = run_short_playout(board_sim, gwidth, gheight, players_sim, (int)gplayer_count, nextp, my_index, PLAYOUT_STEPS);
                sum += (double)final_my;
                free(board_sim);
                free(players_sim);
            }
            double avg = sum / (double)SIMS_PER_CAND;
            /* combine heuristic and sim avg */
            double combined = 0.55 * heur[ci] + 0.45 * avg;
            if (combined > best_comb) { best_comb = combined; best_move = cand; }
        }

        unsigned char mv = (unsigned char)best_move;

        /* final sanity: still at same pos and not blocked */
        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
            sem_post(&game_sync->state_mutex);
            continue;
        }

        /* write move while holding state_mutex (same pattern as your players) */
        ssize_t written = write(STDOUT_FILENO, &mv, 1);
        sem_post(&game_sync->state_mutex);
        if (written != 1) {
            if (written == -1 && errno == EPIPE) break;
            break;
        }
    }

    free(board_snapshot);
    free(players_snapshot);
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

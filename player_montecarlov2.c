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

/*
 * Improved Monte-Carlo player that is opponent-aware and trap-aware.
 *
 * Key improvements over the flat-MonteCarlo version:
 *  1) Opponent-aware playout policy: opponents choose moves that balance
 *     immediate reward and local 'liberties' (number of adjacent free cells),
 *     which encourages moves that avoid getting trapped and that cut others off.
 *  2) Trap/territory heuristic (Voronoi-like): for the best candidate moves we
 *     compute a quick multi-source distance flood-fill to estimate which empty
 *     cells each player can *uniquely* reach earlier than others. Cells that
 *     only our player can reach are treated as potential future captures.
 *  3) Final candidate ranking mixes average simulated final score with the
 *     territory estimate to prefer moves that not only score well in playouts
 *     but also trap opponents or secure contested regions.
 *
 * These changes make the player consider other players' reactions and the
 * consequences of giving up or securing regions. This isn't guaranteed to
 * always win (no simple always-win strategy exists in general multiplayer
 * perfect-information games), but it is substantially stronger at trapping and
 * region control than a pure greedy or vanilla-MC approach.
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

/* ----- Simulation types & helpers ----- */

typedef struct {
    int x, y;
    unsigned int score;
    bool blocked;
} sim_player_t;

static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) return false;
    int cell = board[ty * width + tx];
    return cell > 0;
}

static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
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
        for (int d = 0; d < 8; d++) if (sim_is_valid_move(board, width, height, players, i, d)) return true;
    }
    return false;
}

/* Count free-adjacent cells (local liberties) for a player's head in a simulated board */
static int sim_count_liberties(int *board, int width, int height, sim_player_t *players, int pid) {
    int gx = players[pid].x, gy = players[pid].y; int count = 0;
    for (int d = 0; d < 8; d++) {
        int tx, ty; target_from_dir(gx, gy, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        if (board[ty * width + tx] > 0) count++;
    }
    return count;
}

/* Playout policy: for our player we simulated earlier by Monte-Carlo. For opponents,
   use a policy that prefers moves with good immediate reward but also high liberties
   (to avoid being trivially trapped). Add small randomness to avoid deterministic loops. */
static int sim_pick_policy_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid) {
    (void)player_count; /* unused parameter in current policy implementation */
    int valid_dirs[8]; int valid_count = 0;
    int best_dirs[8]; int best_count = 0;
    double best_score = -DBL_MAX;

    for (int d = 0; d < 8; d++) {
        int tx, ty; target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) continue;
        int cell = board[ty * width + tx];
        if (cell <= 0) continue;
        valid_dirs[valid_count++] = d;
    }
    if (valid_count == 0) return -1;

    /* With small probability choose a pure random move to diversify playouts */
    if ((rand() & 0xFF) < 30) return valid_dirs[rand() % valid_count];

    for (int i = 0; i < valid_count; i++) {
        int d = valid_dirs[i];
        int tx, ty; target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        int cell = board[ty * width + tx];
        /* evaluate as immediate reward + lambda * liberties after moving there */
        /* we'll simulate liberties by temporarily marking the cell occupied and counting */
        int saved = board[ty * width + tx];
        board[ty * width + tx] = -(pid + 1);
        int oldx = players[pid].x, oldy = players[pid].y;
        players[pid].x = tx; players[pid].y = ty;
        int liberties = sim_count_liberties(board, width, height, players, pid);
        /* restore */
        players[pid].x = oldx; players[pid].y = oldy;
        board[ty * width + tx] = saved;

        double score = (double)cell + 1.5 * (double)liberties; /* weight chosen experimentally */
        if (score > best_score) { best_score = score; best_count = 0; best_dirs[best_count++] = d; }
        else if (score == best_score) best_dirs[best_count++] = d;
    }
    return best_dirs[rand() % best_count];
}

/* Fast multi-source BFS to compute a Voronoi-like potential score: which empty cells
   are uniquely closest to each player? We return an array 'vor' of length player_count
   where vor[i] is the sum of values of cells that are strictly closer to i than to
   any other player (ties -> contested -> ignored). Distances are in Chebyshev metric
   because players can move in 8 directions (diagonal cost = 1). */
static void compute_voronoi_potential(int *board, int width, int height, sim_player_t *players, int player_count, unsigned int *vor_out) {
    int n = width * height;
    int *dist = malloc(sizeof(int) * n);
    int *owner = malloc(sizeof(int) * n);
    if (!dist || !owner) {
        if (dist) free(dist);
        if (owner) free(owner);
        return;
    }
    for (int i = 0; i < n; i++) { dist[i] = INT_MAX; owner[i] = -1; }

    /* simple BFS queue (we'll use arrays for speed) */
    int *qx = malloc(sizeof(int) * n);
    int *qy = malloc(sizeof(int) * n);
    int *qo = malloc(sizeof(int) * n);
    int qh = 0, qt = 0;
    /* initialize with player heads */
    for (int p = 0; p < player_count; p++) {
        if (players[p].blocked) continue;
        int x = players[p].x, y = players[p].y;
        int idx = y * width + x;
        dist[idx] = 0;
        owner[idx] = p;
        qx[qt] = x; qy[qt] = y; qo[qt] = p; qt++;
    }

    while (qh < qt) {
        int x = qx[qh]; int y = qy[qh]; int p = qo[qh]; qh++;
        int base_idx = y * width + x;
        int dcur = dist[base_idx];
        for (int dir = 0; dir < 8; dir++) {
            int nx, ny; target_from_dir(x, y, dir, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int nidx = ny * width + nx;
            if (board[nidx] <= 0) continue; /* occupied */
            int nd = dcur + 1;
            if (nd < dist[nidx]) {
                dist[nidx] = nd;
                owner[nidx] = p;
                qx[qt] = nx; qy[qt] = ny; qo[qt] = p; qt++;
            } else if (nd == dist[nidx] && owner[nidx] != p) {
                /* tie -> mark contested */
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

    free(dist); free(owner); free(qx); free(qy); free(qo);
}

/* Copy helpers */
static void copy_board(int *dst, int *src, int n) { memcpy(dst, src, n * sizeof(int)); }
static void copy_players_sim(sim_player_t *dst, player_t *src, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        dst[i].x = (int)src[i].x;
        dst[i].y = (int)src[i].y;
        dst[i].score = src[i].score;
        dst[i].blocked = src[i].blocked;
    }
}

/* Full playout: starting from the sim state given, play until terminal using the
   sim_pick_policy_move for all players. start_next_player is the player index to
   play next. */
static void simulate_playout(int *board, int width, int height, sim_player_t *players, int player_count, int start_next_player) {
    int next = start_next_player;
    while (sim_any_player_has_move(board, width, height, players, player_count)) {
        int p = next;
        next = (next + 1) % player_count;
        if (players[p].blocked) continue;
        int mv = sim_pick_policy_move(board, width, height, players, player_count, p);
        if (mv == -1) { players[p].blocked = true; continue; }
        sim_apply_move(board, width, height, players, p, mv);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\\n", argv[0]);
        return EXIT_FAILURE;
    }

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    size_t state_size = sizeof(game_state_t) + (size_t)width * height * sizeof(int);

    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, state_size, 0);
    if (!state_mgr) { perror("shm_manager_open state"); return EXIT_FAILURE; }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);
    if (!game_state) { fprintf(stderr, "failed to get game_state pointer\\n"); shm_manager_close(state_mgr); return EXIT_FAILURE; }

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    if (!game_sync) { fprintf(stderr, "failed to get game_sync pointer\\n"); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

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
        fprintf(stderr, "player: couldn't determine my index (pid %d)\\n", (int)getpid());
        shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    srand((unsigned int)(getpid() ^ time(NULL)));

    /* allocate private buffers used for simulations */
    int *board_snapshot = malloc(width * height * sizeof(int));
    int *board_sim = malloc(width * height * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    sim_player_t *players_sim = malloc(sizeof(sim_player_t) * game_state->player_count);
    unsigned int *vor_tmp = malloc(sizeof(unsigned int) * game_state->player_count);
    if (!board_snapshot || !board_sim || !players_snapshot || !players_sim || !vor_tmp) {
        fprintf(stderr, "allocation failed\\n");
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
        copy_players_sim(players_snapshot, game_state->players, gplayer_count);

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

        /* Adaptive sims per candidate */
        int board_cells = gwidth * gheight;
        int sims_per_candidate = 400;
        if (board_cells <= 25) sims_per_candidate = 2500;
        else if (board_cells <= 100) sims_per_candidate = 1200;
        else if (board_cells <= 400) sims_per_candidate = 500;
        else sims_per_candidate = 200;
        int max_total_sims = 4000;
        long total_sims = (long)sims_per_candidate * valid_count;
        if (total_sims > max_total_sims) {
            sims_per_candidate = max_total_sims / valid_count; if (sims_per_candidate < 10) sims_per_candidate = 10;
        }

        /* For each candidate, run sims and compute average final score */
        double best_avg = -1e300;
        int best_candidates[8]; int best_candidates_count = 0;
        double candidate_avgs[8];

        for (int ci = 0; ci < valid_count; ci++) {
            int cand = valid_dirs[ci];
            double sum_score = 0.0;

            for (int s = 0; s < sims_per_candidate; s++) {
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);

                /* Apply candidate move for our player */
                int immediate = sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                if (immediate < 0) { players_sim[my_index].blocked = true; }

                int next = (my_index + 1) % gplayer_count;
                simulate_playout(board_sim, gwidth, gheight, players_sim, gplayer_count, next);
                sum_score += (double)players_sim[my_index].score;
            }

            double avg = sum_score / (double)sims_per_candidate;
            candidate_avgs[ci] = avg;
            if (avg > best_avg) { best_avg = avg; best_candidates_count = 0; best_candidates[best_candidates_count++] = cand; }
            else if (avg == best_avg) { best_candidates[best_candidates_count++] = cand; }
        }

        /* If multiple top candidates, compute Voronoi-like potential for the top-K and pick the best
           according to a combined metric: avg_sim_score + gamma * voronoi_potential. */
        int pick = best_candidates[rand() % best_candidates_count];
        if (best_candidates_count > 1) {
            double best_combined = -DBL_MAX;
            int topk = best_candidates_count;
            if (topk > 4) topk = 4; /* limit how many we evaluate with Voronoi for performance */
            for (int t = 0; t < topk; t++) {
                int cand = best_candidates[t];
                /* build the board and players after applying cand */
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                unsigned int *vor = (unsigned int *)malloc(sizeof(unsigned int) * gplayer_count);
                if (!vor) continue;
                compute_voronoi_potential(board_sim, gwidth, gheight, players_sim, gplayer_count, vor);
                double my_vor = (double)vor[my_index];
                free(vor);
                /* gamma weights the territory estimate; chosen experimentally */
                double gamma = 0.03; /* small because vor sums are raw board values */
                /* find index in candidate_avgs for this cand */
                double avg = -DBL_MAX;
                for (int ci = 0; ci < valid_count; ci++) if (valid_dirs[ci] == cand) { avg = candidate_avgs[ci]; break; }
                double combined = avg + gamma * my_vor;
                if (combined > best_combined) { best_combined = combined; pick = cand; }
            }
        }

        unsigned char move = (unsigned char)pick;

        /* Now acquire state_mutex again and write the chosen move while holding it. */
        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) { sem_post(&game_sync->player_mutex[my_index]); continue; }
            break;
        }

        if (game_state->game_over) { sem_post(&game_sync->state_mutex); break; }

        /* final sanity: make sure we still are at the same position and not blocked */
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
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

    free(board_snapshot); free(board_sim); free(players_snapshot); free(players_sim); free(vor_tmp);
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

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

#define MAX_PLAYERS_PROBE 128


static inline void reader_enter(game_sync_t *sync) {
    sem_wait(&sync->master_mutex);
    sem_post(&sync->master_mutex);

    sem_wait(&sync->reader_count_mutex);
    sync->reader_count++;
    if (sync->reader_count == 1) sem_wait(&sync->state_mutex);
    sem_post(&sync->reader_count_mutex);
}
static inline void reader_exit(game_sync_t *sync) {
    sem_wait(&sync->reader_count_mutex);
    sync->reader_count--;
    if (sync->reader_count == 0) sem_post(&sync->state_mutex);
    sem_post(&sync->reader_count_mutex);
}

static int find_my_index(game_state_t *gs, game_sync_t *sync) {
    pid_t me = getpid();
    int idx = -1;

    reader_enter(sync);
    for (unsigned int i = 0; i < gs->player_count; i++) {
        if ((pid_t)gs->players[i].pid == me) {
            idx = (int)i;
            break;
        }
    }
    reader_exit(sync);
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
    *tx = nx;
    *ty = ny;
}

typedef struct { int x,y; unsigned int score; bool blocked; } sim_player_t;

static inline bool sim_is_valid_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) {
        return false;
    }
    return board[ty * width + tx] > 0;
}

static inline int sim_apply_move(int *board, int width, int height, sim_player_t *players, int pid, int d) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int tx, ty;
    target_from_dir(gx, gy, d, &tx, &ty);
    if (tx < 0 || tx >= width || ty < 0 || ty >= height) {
        return -1;
    }
    int idx = ty * width + tx;
    int reward = board[idx];
    if (reward <= 0) {
        return -1;
    }
    players[pid].score += (unsigned int)reward;
    board[idx] = -(pid + 1);
    players[pid].x = tx;
    players[pid].y = ty;
    players[pid].blocked = false;
    return reward;
}

static bool sim_any_player_has_move(int *board, int width, int height, sim_player_t *players, int player_count) {
    for (int i = 0; i < player_count; i++) {
        if (players[i].blocked) {
            continue;
        }
        for (int d = 0; d < 8; d++) {
            if (sim_is_valid_move(board, width, height, players, i, d)) {
                return true;
            }
        }
    }
    return false;
}

static int sim_count_liberties(int *board, int width, int height, sim_player_t *players, int pid) {
    int gx = players[pid].x;
    int gy = players[pid].y;
    int c = 0;
    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(gx, gy, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) {
            continue;
        }
        if (board[ty * width + tx] > 0) {
            c++;
        }
    }
    return c;
}

static int sim_pick_policy_move(int *board, int width, int height, sim_player_t *players, int player_count, int pid) {
    (void)player_count;
    int valid_dirs[8];
    int valid_count = 0;
    int best_dirs[8];
    int best_count = 0;
    double best_score = -DBL_MAX;

    for (int d = 0; d < 8; d++) {
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        if (tx < 0 || tx >= width || ty < 0 || ty >= height) {
            continue;
        }
        int cell = board[ty * width + tx];
        if (cell <= 0) {
            continue;
        }
        valid_dirs[valid_count++] = d;
    }

    if (valid_count == 0) {
        return -1;
    }

    if ((rand() & 0xFF) < 30) {
        return valid_dirs[rand() % valid_count];
    }

    for (int i = 0; i < valid_count; i++) {
        int d = valid_dirs[i];
        int tx, ty;
        target_from_dir(players[pid].x, players[pid].y, d, &tx, &ty);
        int saved = board[ty * width + tx];
        board[ty * width + tx] = -(pid + 1);
        int oldx = players[pid].x;
        int oldy = players[pid].y;
        players[pid].x = tx;
        players[pid].y = ty;
        int lib = sim_count_liberties(board, width, height, players, pid);
        players[pid].x = oldx;
        players[pid].y = oldy;
        board[ty * width + tx] = saved;

        double score = (double)saved + 1.5 * (double)lib;
        if (score > best_score) {
            best_score = score;
            best_count = 0;
            best_dirs[best_count++] = d;
        } else if (score == best_score) {
            best_dirs[best_count++] = d;
        }
    }

    return best_dirs[rand() % best_count];
}

static void compute_voronoi_potential_buf(int *board, int width, int height, sim_player_t *players, int player_count, unsigned int *vor_out, int *dist, int *owner, int *qx, int *qy, int *qo) {
    int n = width * height;
    for (int i = 0; i < n; i++) {
        dist[i] = INT_MAX;
        owner[i] = -1;
    }

    int qh = 0;
    int qt = 0;

    for (int p = 0; p < player_count; p++) {
        if (players[p].blocked) {
            continue;
        }
        int x = players[p].x;
        int y = players[p].y;
        int idx = y * width + x;
        dist[idx] = 0;
        owner[idx] = p;
        qx[qt] = x;
        qy[qt] = y;
        qo[qt] = p;
        qt++;
    }

    while (qh < qt) {
        int x = qx[qh];
        int y = qy[qh];
        int p = qo[qh];
        qh++;
        int base = y * width + x;
        int dcur = dist[base];
        for (int dir = 0; dir < 8; dir++) {
            int nx, ny;
            target_from_dir(x, y, dir, &nx, &ny);
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            int nidx = ny * width + nx;
            if (board[nidx] <= 0) {
                continue;
            }
            int nd = dcur + 1;
            if (nd < dist[nidx]) {
                dist[nidx] = nd;
                owner[nidx] = p;
                qx[qt] = nx;
                qy[qt] = ny;
                qo[qt] = p;
                qt++;
            } else if (nd == dist[nidx] && owner[nidx] != p) {
                owner[nidx] = -2;
            }
        }
    }

    for (int p = 0; p < player_count; p++) {
        vor_out[p] = 0u;
    }
    for (int i = 0; i < n; i++) {
        if (board[i] <= 0) {
            continue;
        }
        int o = owner[i];
        if (o >= 0) {
            vor_out[o] += (unsigned int)board[i];
        }
    }
}

static void copy_board(int *dst, int *src, int n) {
    memcpy(dst, src, n * sizeof(int));
}
static void copy_players_sim(sim_player_t *dst, player_t *src, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        dst[i].x = (int)src[i].x;
        dst[i].y = (int)src[i].y;
        dst[i].score = src[i].score;
        dst[i].blocked = src[i].blocked;
    }
}

static void simulate_playout(int *board, int width, int height, sim_player_t *players, int player_count, int start_next_player) {
    int next = start_next_player;
    while (sim_any_player_has_move(board, width, height, players, player_count)) {
        int p = next;
        next = (next + 1) % player_count;
        if (players[p].blocked) {
            continue;
        }
        int mv = sim_pick_policy_move(board, width, height, players, player_count, p);
        if (mv == -1) {
            players[p].blocked = true;
            continue;
        }
        sim_apply_move(board, width, height, players, p, mv);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, 0, 0);
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

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, 0, 0);
    if (!sync_mgr) {
        perror("shm_manager_open sync");
        shm_manager_close(state_mgr);
        return EXIT_FAILURE;
    }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    if (!game_sync) {
        fprintf(stderr, "failed to get game_sync pointer\n");
        shm_manager_close(state_mgr);
        shm_manager_close(sync_mgr);
        return EXIT_FAILURE;
    }

    int my_index = -1;
    const int max_iters = 500;
    int it = 0;
    while (my_index == -1 && !game_state->game_over && it < max_iters) {
        my_index = find_my_index(game_state, game_sync);
        if (my_index != -1) {
            break;
        }
        struct timespec short_sleep = {0, 10 * 1000 * 1000};
        nanosleep(&short_sleep, NULL);
        it++;
    }

    if (my_index == -1) {
        my_index = find_my_index(game_state, game_sync);
    }
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
        return EXIT_FAILURE;
    }

    while (1) {
        
        if (sem_wait(&game_sync->player_mutex[my_index]) == -1) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (game_state->game_over) {
            break;
        }

        if (game_state->players[my_index].blocked) {
            break;
        }

        reader_enter(game_sync);
        if (game_state->game_over) {
            reader_exit(game_sync);
            break;
        }

        int gx = (int)game_state->players[my_index].x;
        int gy = (int)game_state->players[my_index].y;
        int gwidth = game_state->width;
        int gheight = game_state->height;
        unsigned int gplayer_count = game_state->player_count;

        copy_board(board_snapshot, game_state->board, gwidth * gheight);
        copy_players_sim(players_snapshot, game_state->players, gplayer_count);
        reader_exit(game_sync);

        int valid_dirs[8];
        int valid_count = 0;
        int immediate_vals[8];
        for (int d = 0; d < 8; d++) {
            int tx, ty;
            target_from_dir(gx, gy, d, &tx, &ty);
            if (tx < 0 || tx >= gwidth || ty < 0 || ty >= gheight) {
                continue;
            }
            int cell = board_snapshot[ty * gwidth + tx];
            if (cell <= 0) {
                continue;
            }
            valid_dirs[valid_count] = d;
            immediate_vals[valid_count] = cell;
            valid_count++;
        }
        if (valid_count == 0) {
            continue;
        }

        int free_cells = 0;
        for (int i = 0; i < cells; i++) {
            if (board_snapshot[i] > 0) {
                free_cells++;
            }
        }
        int opening_threshold = (int)(cells * 0.55);
        if (free_cells >= opening_threshold) {
        
            double bestv = -DBL_MAX;
            int bests[8];
            int bc = 0;
            for (int i = 0; i < valid_count; i++) {
                int d = valid_dirs[i];
                int tx, ty;
                target_from_dir(gx, gy, d, &tx, &ty);
                int neigh_sum = 0;
                for (int dd = 0; dd < 8; dd++) {
                    int nx, ny;
                    target_from_dir(tx, ty, dd, &nx, &ny);
                    if (nx < 0 || nx >= gwidth || ny < 0 || ny >= gheight) {
                        continue;
                    }
                    int v = board_snapshot[ny * gwidth + nx];
                    if (v > 0) {
                        neigh_sum += v;
                    }
                }
                double val = (double)immediate_vals[i] + 0.25 * (double)neigh_sum;
                if (val > bestv) {
                    bestv = val;
                    bc = 0;
                    bests[bc++] = d;
                } else if (val == bestv) {
                    bests[bc++] = d;
                }
            }
            int pick = bests[rand() % bc];

            if (sem_wait(&game_sync->state_mutex) == -1) {
                if (errno == EINTR) {
                    sem_post(&game_sync->player_mutex[my_index]);
                    continue;
                }
                break;
            }
            if (game_state->game_over) {
                sem_post(&game_sync->state_mutex);
                break;
            }
            if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
                sem_post(&game_sync->state_mutex);
                continue;
            }
            unsigned char mv = (unsigned char)pick;
            ssize_t w = write(STDOUT_FILENO, &mv, 1);
            sem_post(&game_sync->state_mutex);
            if (w != 1) {
                if (w == -1 && errno == EPIPE) {
                    break;
                }
                break;
            }
            continue;
        }

        int K = 3;
        if (valid_count < K) {
            K = valid_count;
        }
        int idxs[8];
        for (int i = 0; i < valid_count; i++) idxs[i] = i;
        for (int i = 0; i < K; i++) {
            int best = i;
            for (int j = i + 1; j < valid_count; j++) {
                if (immediate_vals[idxs[j]] > immediate_vals[idxs[best]]) {
                    best = j;
                }
            }
            int tmp = idxs[i];
            idxs[i] = idxs[best];
            idxs[best] = tmp;
        }

        int board_cells = gwidth * gheight;
        int sims_per_candidate = 400;
        if (board_cells <= 25) sims_per_candidate = 500;
        else if (board_cells <= 100) sims_per_candidate = 300;
        else if (board_cells <= 400) sims_per_candidate = 150;
        else sims_per_candidate = 80;
        int max_total_sims = 2000;
        long total_sims = (long)sims_per_candidate * K;
        if (total_sims > max_total_sims) sims_per_candidate = max_total_sims / K;
        if (sims_per_candidate < 5) sims_per_candidate = 5;

        double best_avg = -DBL_MAX;
        int bests2[8];
        int bestc2 = 0;
        double candidate_avgs[8];
        for (int i = 0; i < valid_count; i++) candidate_avgs[i] = (double)immediate_vals[i];

        for (int t = 0; t < K; t++) {
            int ci = idxs[t];
            int cand = valid_dirs[ci];
            double sum_score = 0.0;
            for (int s = 0; s < sims_per_candidate; s++) {
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                int immediate = sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                if (immediate < 0) {
                    players_sim[my_index].blocked = true;
                }
                int next = (my_index + 1) % gplayer_count;
                simulate_playout(board_sim, gwidth, gheight, players_sim, gplayer_count, next);
                sum_score += (double)players_sim[my_index].score;
            }
            double avg = sum_score / (double)sims_per_candidate;
            candidate_avgs[ci] = avg;
            if (avg > best_avg) {
                best_avg = avg;
                bestc2 = 0;
                bests2[bestc2++] = cand;
            } else if (avg == best_avg) {
                bests2[bestc2++] = cand;
            }
        }

        int pick = bests2[rand() % bestc2];
        if (bestc2 > 1) {
            double best_comb = -DBL_MAX;
            int topk = bestc2;
            if (topk > 4) topk = 4;
            for (int t = 0; t < topk; t++) {
                int cand = bests2[t];
                copy_board(board_sim, board_snapshot, gwidth * gheight);
                memcpy(players_sim, players_snapshot, sizeof(sim_player_t) * gplayer_count);
                sim_apply_move(board_sim, gwidth, gheight, players_sim, my_index, cand);
                compute_voronoi_potential_buf(board_sim, gwidth, gheight, players_sim, gplayer_count, vor_tmp, dist, owner, qx, qy, qo);
                double my_vor = (double)vor_tmp[my_index];
                double gamma = 0.03;
                double avg = candidate_avgs[t];
                double combined = avg + gamma * my_vor;
                if (combined > best_comb) {
                    best_comb = combined;
                    pick = cand;
                }
            }
        }

        unsigned char move = (unsigned char)pick;

        if (sem_wait(&game_sync->state_mutex) == -1) {
            if (errno == EINTR) {
                sem_post(&game_sync->player_mutex[my_index]);
                continue;
            }
            break;
        }
        if (game_state->game_over) {
            sem_post(&game_sync->state_mutex);
            break;
        }
        if ((int)game_state->players[my_index].x != gx || (int)game_state->players[my_index].y != gy || game_state->players[my_index].blocked) {
            sem_post(&game_sync->state_mutex);
            continue;
        }
        ssize_t written = write(STDOUT_FILENO, &move, 1);
        sem_post(&game_sync->state_mutex);
        if (written != 1) {
            if (written == -1 && errno == EPIPE) {
                break;
            }
            break;
        }
    }

    free(board_snapshot);
    free(board_sim);
    free(players_snapshot);
    free(players_sim);
    free(vor_tmp);
    free(dist);
    free(owner);
    free(qx);
    free(qy);
    free(qo);
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

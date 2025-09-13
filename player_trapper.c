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
 * player_trapper.c
 *
 * Objective: aggressively trap other players and capture the remaining free
 * territory. Strategy summary:
 *  - For each legal candidate move, compute a Voronoi / reachability partition
 *    (multi-source BFS) to estimate which empty cells are uniquely reachable by
 *    each player after making that move.
 *  - Score a candidate by: immediate reward + alpha * my_unique - beta * sum(opponents_unique)
 *    (i.e. prefer moves that increase your uniquely reachable territory while
 *    decreasing opponents'). This directly targets trapping opponents.
 *  - Use candidate pruning (top-K by immediate value) to limit expensive BFS
 *    work. Optionally run a small number of playout sims for final tie-breaking.
 *  - Preallocate all buffers; no malloc inside the main loop for speed.
 *
 * Tunables (can be changed at compile-time or by editing variables below):
 *  - TOP_K: how many best immediate-value candidates to simulate/score deeply.
 *  - ALPHA / BETA: weights for the territory heuristic.
 *  - DO_SIMS: whether to run short playouts to break ties (may be disabled for speed).
 */

#ifndef TOP_K
#define TOP_K 3
#endif
#ifndef ALPHA
#define ALPHA 1.0
#endif
#ifndef BETA
#define BETA 1.4
#endif
#ifndef DO_SIMS
#define DO_SIMS 1
#endif

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

typedef struct { int x,y; unsigned int score; bool blocked; } sim_player_t;

static inline int sim_apply_move(int *board,int width,int height,sim_player_t *players,int pid,int d){
    int gx=players[pid].x, gy=players[pid].y, tx,ty; target_from_dir(gx,gy,d,&tx,&ty);
    if(tx<0||tx>=width||ty<0||ty>=height) return -1;
    int idx = ty*width + tx; int reward = board[idx]; if(reward<=0) return -1;
    players[pid].score += (unsigned int)reward; board[idx]=-(pid+1); players[pid].x=tx; players[pid].y=ty; players[pid].blocked=false; return reward;
}

/* Multi-source BFS that writes for each cell the owner index (>=0) if uniquely
   reachable, -2 if contested, -1 if unreachable/occupied. Uses Chebyshev metric.
   Also fills unique_sum[player] = sum of cell values uniquely reachable by player. */
static void reachable_partition_buf(int *board,int width,int height,sim_player_t *players,int player_count,int *owner_buf,int *dist_buf,int *qx,int *qy,int *qo,unsigned int *unique_sum){
    int n = width*height;
    for(int i=0;i<n;i++){ dist_buf[i]=INT_MAX; owner_buf[i]=-1; }
    int qh=0, qt=0;
    /* enqueue player heads */
    for(int p=0;p<player_count;p++){
        if(players[p].blocked) continue;
        int x=players[p].x, y=players[p].y; int idx = y*width + x;
        dist_buf[idx]=0; owner_buf[idx]=p; qx[qt]=x; qy[qt]=y; qo[qt]=p; qt++;
    }
    while(qh<qt){ int x=qx[qh], y=qy[qh], p=qo[qh]; qh++; int base=y*width+x; int dcur=dist_buf[base];
        for(int dir=0; dir<8; dir++){ int nx,ny; target_from_dir(x,y,dir,&nx,&ny); if(nx<0||nx>=width||ny<0||ny>=height) continue; int nidx=ny*width+nx; if(board[nidx]<=0) continue; int nd=dcur+1; if(nd<dist_buf[nidx]){ dist_buf[nidx]=nd; owner_buf[nidx]=p; qx[qt]=nx; qy[qt]=ny; qo[qt]=p; qt++; } else if(nd==dist_buf[nidx] && owner_buf[nidx]!=p){ owner_buf[nidx] = -2; } }
    }
    for(int p=0;p<player_count;p++) unique_sum[p]=0u;
    for(int i=0;i<n;i++){ if(board[i]<=0) continue; int o=owner_buf[i]; if(o>=0) unique_sum[o] += (unsigned int)board[i]; }
}

/* lightweight opponent policy used in short sims: prefer reward + liberties */
static int pick_policy_fast(int *board,int width,int height,sim_player_t *players,int player_count,int pid){
    int best_dirs[8], bestc=0; double bestv=-DBL_MAX;
    for(int d=0; d<8; d++){ int tx,ty; target_from_dir(players[pid].x,players[pid].y,d,&tx,&ty); if(tx<0||tx>=width||ty<0||ty>=height) continue; int v=board[ty*width+tx]; if(v<=0) continue; int saved=v; board[ty*width+tx]=-(pid+1); int oldx=players[pid].x, oldy=players[pid].y; players[pid].x=tx; players[pid].y=ty; int libs=0; for(int dd=0;dd<8;dd++){ int nx,ny; target_from_dir(tx,ty,dd,&nx,&ny); if(nx<0||nx>=width||ny<0||ny>=height) continue; if(board[ny*width+nx]>0) libs++; } players[pid].x=oldx; players[pid].y=oldy; board[ty*width+tx]=saved; double score = (double)saved + 1.2*(double)libs; if(score>bestv){ bestv=score; bestc=0; best_dirs[bestc++]=d; } else if(score==bestv) best_dirs[bestc++]=d; }
    if(bestc==0) return -1; return best_dirs[rand()%bestc];
}

static void simulate_limited(int *board,int width,int height,sim_player_t *players,int player_count,int start_next,int depth_limit){
    int next=start_next; int iter=0; while(iter++<depth_limit){ bool any=false; for(int i=0;i<player_count;i++){ if(!players[i].blocked){ any=true; break; } } if(!any) break; int p=next; next=(next+1)%player_count; if(players[p].blocked) continue; int mv=pick_policy_fast(board,width,height,players,player_count,p); if(mv==-1){ players[p].blocked=true; continue; } sim_apply_move(board,width,height,players,p,mv); }
}

int main(int argc,char *argv[]){
    if(argc!=3){ fprintf(stderr,"Uso: %s <ancho> <alto>\n",argv[0]); return EXIT_FAILURE; }
    int width=atoi(argv[1]), height=atoi(argv[2]); size_t state_size = sizeof(game_state_t) + (size_t)width*height*sizeof(int);
    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE,state_size,0); if(!state_mgr){ perror("shm_manager_open state"); return EXIT_FAILURE; }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr); if(!game_state){ fprintf(stderr,"failed to get game_state pointer\n"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC,sizeof(game_sync_t),0); if(!sync_mgr){ perror("shm_manager_open sync"); shm_manager_close(state_mgr); return EXIT_FAILURE; }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr); if(!game_sync){ fprintf(stderr,"failed to get game_sync pointer\n"); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

    int my_index=-1; const int max_iters=500; int it=0; while(my_index==-1 && !game_state->game_over && it<max_iters){ my_index=find_my_index(game_state,game_sync); if(my_index!=-1) break; struct timespec short_sleep={0,10*1000*1000}; nanosleep(&short_sleep,NULL); it++; }
    if(my_index==-1) my_index=find_my_index(game_state,game_sync); if(my_index==-1){ fprintf(stderr,"player: couldn't determine my index (pid %d)\n",(int)getpid()); shm_manager_close(state_mgr); shm_manager_close(sync_mgr); return EXIT_FAILURE; }

    srand((unsigned int)(getpid() ^ time(NULL)));

    int cells = width*height;
    int *board_snapshot = malloc(cells * sizeof(int));
    int *board_sim = malloc(cells * sizeof(int));
    sim_player_t *players_snapshot = malloc(sizeof(sim_player_t) * game_state->player_count);
    sim_player_t *players_sim = malloc(sizeof(sim_player_t) * game_state->player_count);
    unsigned int *vor_tmp = malloc(sizeof(unsigned int) * game_state->player_count);
    /* preallocated voronoi/bfs buffers */
    int *owner = malloc(sizeof(int)*cells);
    int *dist = malloc(sizeof(int)*cells);
    int *qx = malloc(sizeof(int)*cells);
    int *qy = malloc(sizeof(int)*cells);
    int *qo = malloc(sizeof(int)*cells);
    if(!board_snapshot||!board_sim||!players_snapshot||!players_sim||!vor_tmp||!owner||!dist||!qx||!qy||!qo){ fprintf(stderr,"allocation failed\n"); return EXIT_FAILURE; }

    while(1){
        if(sem_wait(&game_sync->player_mutex[my_index])==-1){ if(errno==EINTR) continue; break; }
        if(game_state->game_over) break; if(game_state->players[my_index].blocked) break;
        if(sem_wait(&game_sync->state_mutex)==-1){ if(errno==EINTR){ sem_post(&game_sync->player_mutex[my_index]); continue; } break; }
        if(game_state->game_over){ sem_post(&game_sync->state_mutex); break; }

        int gx=(int)game_state->players[my_index].x, gy=(int)game_state->players[my_index].y;
        int gwidth=game_state->width, gheight=game_state->height; unsigned int gplayer_count=game_state->player_count;
        copy_board(board_snapshot, game_state->board, gwidth*gheight);
        copy_players_sim(players_snapshot, game_state->players, gplayer_count);
        sem_post(&game_sync->state_mutex);

        /* gather valid moves and immediate values */
        int valid_dirs[8], valid_count=0; int immediate_val[8];
        for(int d=0; d<8; d++){ int tx,ty; target_from_dir(gx,gy,d,&tx,&ty); if(tx<0||tx>=gwidth||ty<0||ty>=gheight) continue; int v = board_snapshot[ty*gwidth+tx]; if(v<=0) continue; valid_dirs[valid_count]=d; immediate_val[valid_count]=v; valid_count++; }
        if(valid_count==0) continue;

        /* opening quick pick: if many free cells, prioritize speed */
        int free_cells=0; for(int i=0;i<cells;i++) if(board_snapshot[i]>0) free_cells++;
        int opening_thresh = (int)(cells * 0.55);
        if(free_cells >= opening_thresh){ double bestv=-DBL_MAX; int bests[8], bc=0; for(int i=0;i<valid_count;i++){ int d=valid_dirs[i]; int tx,ty; target_from_dir(gx,gy,d,&tx,&ty); int neigh=0; for(int dd=0;dd<8;dd++){ int nx,ny; target_from_dir(tx,ty,dd,&nx,&ny); if(nx<0||nx>=gwidth||ny<0||ny>=gheight) continue; int vv=board_snapshot[ny*gwidth+nx]; if(vv>0) neigh+=vv; } double val = (double)immediate_val[i] + 0.25*(double)neigh; if(val>bestv){ bestv=val; bc=0; bests[bc++]=d; } else if(val==bestv) bests[bc++]=d; } int pick = bests[rand()%bc]; if(sem_wait(&game_sync->state_mutex)==-1){ if(errno==EINTR){ sem_post(&game_sync->player_mutex[my_index]); continue; } break; } if(game_state->game_over){ sem_post(&game_sync->state_mutex); break; } if((int)game_state->players[my_index].x!=gx || (int)game_state->players[my_index].y!=gy || game_state->players[my_index].blocked){ sem_post(&game_sync->state_mutex); continue; } unsigned char mv=(unsigned char)pick; ssize_t w=write(STDOUT_FILENO,&mv,1); sem_post(&game_sync->state_mutex); if(w!=1){ if(w==-1 && errno==EPIPE) break; break; } continue; }

        /* prune to top-K by immediate value to reduce expensive BFS/sim */
        int K = TOP_K; if(valid_count < K) K = valid_count;
        int idxs[8]; for(int i=0;i<valid_count;i++) idxs[i]=i;
        for(int i=0;i<K;i++){ int best=i; for(int j=i+1;j<valid_count;j++) if(immediate_val[idxs[j]]>immediate_val[idxs[best]]) best=j; int tmp=idxs[i]; idxs[i]=idxs[best]; idxs[best]=tmp; }

        /* for each top candidate compute reachable partition and score */
        double best_score = -DBL_MAX; int best_pick = valid_dirs[rand()%valid_count];
        for(int t=0;t<K;t++){
            int ci = idxs[t]; int cand = valid_dirs[ci]; copy_board(board_sim, board_snapshot, gwidth*gheight); memcpy(players_sim, players_snapshot, sizeof(sim_player_t)*gplayer_count);
            int imm = sim_apply_move(board_sim,gwidth,gheight,players_sim,my_index,cand); if(imm<0) { players_sim[my_index].blocked=true; }
            /* compute unique sums after our candidate */
            reachable_partition_buf(board_sim,gwidth,gheight,players_sim,gplayer_count,owner,dist,qx,qy,qo,vor_tmp);
            double my_unique = (double)vor_tmp[my_index]; double opp_sum = 0.0; for(unsigned int p=0;p<gplayer_count;p++) if((int)p!=my_index) opp_sum += (double)vor_tmp[p];
            double score = (double)(imm>0?imm:0) + ALPHA * my_unique - BETA * opp_sum;
#if DO_SIMS
            /* optional short sims to refine the estimate (small number) */
            int sims = 10; double sim_sum = 0.0;
            for(int s=0;s<sims;s++){
                copy_board(board_sim, board_snapshot, gwidth*gheight); memcpy(players_sim, players_snapshot, sizeof(sim_player_t)*gplayer_count);
                sim_apply_move(board_sim,gwidth,gheight,players_sim,my_index,cand);
                simulate_limited(board_sim,gwidth,gheight,players_sim,gplayer_count,(my_index+1)%gplayer_count,8);
                sim_sum += (double)players_sim[my_index].score;
            }
            double sim_avg = sim_sum / (double)sims;
            /* blend sim_avg (scaled) with partition score */
            score = 0.6 * score + 0.4 * sim_avg;
#endif
            if(score > best_score){ best_score = score; best_pick = cand; }
        }

        unsigned char move = (unsigned char)best_pick;
        if(sem_wait(&game_sync->state_mutex)==-1){ if(errno==EINTR){ sem_post(&game_sync->player_mutex[my_index]); continue; } break; }
        if(game_state->game_over){ sem_post(&game_sync->state_mutex); break; }
        if((int)game_state->players[my_index].x!=gx || (int)game_state->players[my_index].y!=gy || game_state->players[my_index].blocked){ sem_post(&game_sync->state_mutex); continue; }
        ssize_t written = write(STDOUT_FILENO,&move,1);
        sem_post(&game_sync->state_mutex);
        if(written!=1){ if(written==-1 && errno==EPIPE) break; break; }
    }

    free(board_snapshot); free(board_sim); free(players_snapshot); free(players_sim); free(vor_tmp); free(owner); free(dist); free(qx); free(qy); free(qo);
    shm_manager_close(state_mgr); shm_manager_close(sync_mgr);
    return EXIT_SUCCESS;
}

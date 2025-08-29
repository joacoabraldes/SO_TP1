#define _XOPEN_SOURCE 700
#include "common.h"
#include "shm_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>

static game_state_t *gstate_for_sort = NULL;
static int cmp_players(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const player_t *pa = &gstate_for_sort->players[ia];
    const player_t *pb = &gstate_for_sort->players[ib];

    if (pa->score != pb->score) return (pa->score < pb->score) ? 1 : -1;
    if (pa->valid_moves != pb->valid_moves) return (pa->valid_moves > pb->valid_moves) ? 1 : -1;
    if (pa->invalid_moves != pb->invalid_moves) return (pa->invalid_moves > pb->invalid_moves) ? 1 : -1;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    setlocale(LC_ALL, "");

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    size_t state_size = sizeof(game_state_t) + width * height * sizeof(int);

    // Open existing shared regions (master already created them)
    shm_manager_t *state_mgr = shm_manager_open(SHM_GAME_STATE, state_size, 0);
    if (!state_mgr) { perror("shm_manager_open state"); exit(EXIT_FAILURE); }
    game_state_t *game_state = (game_state_t *)shm_manager_data(state_mgr);

    shm_manager_t *sync_mgr = shm_manager_open(SHM_GAME_SYNC, sizeof(game_sync_t), 0);
    if (!sync_mgr) { perror("shm_manager_open sync"); shm_manager_close(state_mgr); exit(EXIT_FAILURE); }
    game_sync_t *game_sync = (game_sync_t *)shm_manager_data(sync_mgr);

    const char *bg_colors[] = {
        "\x1b[41m", "\x1b[42m", "\x1b[43m", "\x1b[44m",
        "\x1b[45m", "\x1b[46m", "\x1b[101m", "\x1b[102m", "\x1b[103m"
    };
    const char *reset = "\x1b[0m";
    const char *dim = "\x1b[90m";
    const char *fg_head = "\x1b[97m";
    const char *head_glyph = "☺";
    const int CELL_W = 3;

    while (!game_state->game_over) {
        sem_wait(&game_sync->master_to_view);

        printf("\033[2J\033[H");

        // top border
        printf("╔");
        for (int c = 0; c < width; c++) for (int k = 0; k < CELL_W; k++) printf("═");
        printf("╗\n");

        for (int r = 0; r < height; r++) {
            printf("║");
            for (int c = 0; c < width; c++) {
                int cell = game_state->board[r * width + c];
                if (cell > 0) {
                    printf("%s %d %s", dim, cell, reset);
                } else {
                    int pidx = -cell - 1;
                    const char *bg = bg_colors[pidx % (sizeof(bg_colors)/sizeof(bg_colors[0]))];
                    int is_head = (game_state->players[pidx].x == c && game_state->players[pidx].y == r);
                    if (is_head) printf("%s%s %s %s", bg, fg_head, head_glyph, reset);
                    else printf("%s   %s", bg, reset);
                }
            }
            printf("║\n");
        }

        // bottom border
        printf("╚");
        for (int c = 0; c < width; c++) for (int k = 0; k < CELL_W; k++) printf("═");
        printf("╝\n");

        // sorted scoreboard
        unsigned int pc = game_state->player_count;
        int *order = malloc(pc * sizeof(int));
        if (!order) order = NULL;
        for (unsigned int i = 0; i < pc; i++) order[i] = i;
        gstate_for_sort = game_state;
        qsort(order, pc, sizeof(int), cmp_players);

        printf("\n");
        printf("  Players:                         Puntos   Válidos  Inválidos\n");
        printf("  ------------------------------------------------------------\n");
        for (unsigned int idx = 0; idx < pc; idx++) {
            int i = order[idx];
            const char *bg = bg_colors[i % (sizeof(bg_colors)/sizeof(bg_colors[0]))];
            printf("  %s  %s %-12s %20u %9u %11u\n",
                   bg, reset,
                   game_state->players[i].name,
                   game_state->players[i].score,
                   game_state->players[i].valid_moves,
                   game_state->players[i].invalid_moves);
        }
        if (order) free(order);

        sem_post(&game_sync->view_to_master);

        if (game_state->game_over) break;
    }

    // cleanup: close the mapped regions (do NOT unlink)
    shm_manager_close(state_mgr);
    shm_manager_close(sync_mgr);

    printf("\n=== Juego Terminado ===\n");
    return 0;
}

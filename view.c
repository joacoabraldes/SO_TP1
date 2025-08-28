#define _XOPEN_SOURCE 700
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>   // for setlocale

// helper for sorting players
static game_state_t *gstate_for_sort = NULL;
static int cmp_players(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const player_t *pa = &gstate_for_sort->players[ia];
    const player_t *pb = &gstate_for_sort->players[ib];

    // primary: score descending
    if (pa->score != pb->score) return (pa->score < pb->score) ? 1 : -1;
    // secondary: valid_moves ascending (fewer is better)
    if (pa->valid_moves != pb->valid_moves) return (pa->valid_moves > pb->valid_moves) ? 1 : -1;
    // tertiary: invalid_moves ascending (fewer is better)
    if (pa->invalid_moves != pb->invalid_moves) return (pa->invalid_moves > pb->invalid_moves) ? 1 : -1;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Enable locale so terminals handle multi-byte/Unicode properly
    setlocale(LC_ALL, "");

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    // Abrir memoria compartida del estado (solo lectura)
    int shm_state_fd = shm_open(SHM_GAME_STATE, O_RDONLY, 0666);
    if (shm_state_fd == -1) {
        perror("shm_open state");
        exit(EXIT_FAILURE);
    }

    size_t state_size = sizeof(game_state_t) + width * height * sizeof(int);
    game_state_t *game_state = mmap(NULL, state_size, PROT_READ, MAP_SHARED, shm_state_fd, 0);
    if (game_state == MAP_FAILED) {
        perror("mmap state");
        close(shm_state_fd);
        exit(EXIT_FAILURE);
    }

    // Abrir memoria compartida de sincronización
    int shm_sync_fd = shm_open(SHM_GAME_SYNC, O_RDWR, 0666);
    if (shm_sync_fd == -1) {
        perror("shm_open sync");
        munmap(game_state, state_size);
        close(shm_state_fd);
        exit(EXIT_FAILURE);
    }

    game_sync_t *game_sync = mmap(NULL, sizeof(game_sync_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_sync_fd, 0);
    if (game_sync == MAP_FAILED) {
        perror("mmap sync");
        munmap(game_state, state_size);
        close(shm_state_fd);
        close(shm_sync_fd);
        exit(EXIT_FAILURE);
    }

    // Background color palette (ANSI background colors)
    const char *bg_colors[] = {
        "\x1b[41m",  // bg red
        "\x1b[42m",  // bg green
        "\x1b[43m",  // bg yellow
        "\x1b[44m",  // bg blue
        "\x1b[45m",  // bg magenta
        "\x1b[46m",  // bg cyan
        "\x1b[101m", // bg bright red
        "\x1b[102m", // bg bright green
        "\x1b[103m"  // bg bright yellow
    };
    int n_colors = sizeof(bg_colors) / sizeof(bg_colors[0]);
    const char *reset = "\x1b[0m";
    const char *dim = "\x1b[90m";   // dim color for rewards
    const char *fg_head = "\x1b[97m"; // bright foreground for the head glyph

    // Characters
    const char *head_glyph = "☺"; // if misaligned on some terminals, change to "O" or a digit
    const int CELL_W = 3; // visible chars per cell

    while (!game_state->game_over) {
        // Wait for master
        sem_wait(&game_sync->master_to_view);

        // Clear screen and move cursor to home
        printf("\033[2J\033[H");

        // TOP BORDER (continuous, no inner separators so squares touch)
        printf("╔");
        for (int c = 0; c < width; c++) {
            for (int k = 0; k < CELL_W; k++) printf("═");
        }
        printf("╗\n");

        // Print board rows (no vertical separators between cells so colored blocks touch)
        for (int r = 0; r < height; r++) {
            printf("║");
            for (int c = 0; c < width; c++) {
                int cell = game_state->board[r * width + c];

                if (cell > 0) {
                    // reward cell: center the digit in CELL_W columns, dim color
                    printf("%s %d %s", dim, cell, reset);
                } else {
                    // occupied by player (body or head). Determine which player.
                    int pidx = -cell - 1; // 0-based
                    const char *bg = bg_colors[pidx % n_colors];

                    // Check if this coord is the player's head (current position)
                    int is_head = 0;
                    if ((int)game_state->players[pidx].x == c &&
                        (int)game_state->players[pidx].y == r) {
                        is_head = 1;
                    }

                    if (is_head) {
                        // Print head as colored cell with centered glyph (fg_head)
                        printf("%s%s %s %s", bg, fg_head, head_glyph, reset);
                    } else {
                        // Print normal colored block (three spaces with bg)
                        printf("%s   %s", bg, reset);
                    }
                }
            }
            printf("║\n");
        }

        // BOTTOM BORDER
        printf("╚");
        for (int c = 0; c < width; c++) {
            for (int k = 0; k < CELL_W; k++) printf("═");
        }
        printf("╝\n");

        // Build sorted order of players by score (and tie-breakers)
        unsigned int pc = game_state->player_count;
        int *order = malloc(pc * sizeof(int));
        if (!order) order = NULL;
        for (unsigned int i = 0; i < pc; i++) order[i] = i;

        gstate_for_sort = game_state;
        qsort(order, pc, sizeof(int), cmp_players);

        // Legend / players info (aligned scoreboard beneath the board), sorted
        printf("\n");
        printf("  Players:                         Puntos   Válidos  Inválidos\n");
        printf("  ------------------------------------------------------------\n");
        for (unsigned int idx = 0; idx < pc; idx++) {
            int i = order[idx]; // original player index
            const char *bg = bg_colors[i % n_colors];
            printf("  %s  %s %-12s %20u %9u %11u\n",
                   bg, reset,
                   game_state->players[i].name,
                   game_state->players[i].score,
                   game_state->players[i].valid_moves,
                   game_state->players[i].invalid_moves);
        }
        if (order) free(order);

        // Footer hint
        printf("\n(Colores = jugadores, números = recompensas. Head = ☺)\n");

        // Notify master we finished drawing
        sem_post(&game_sync->view_to_master);

        // If game is over, break after notifying
        if (game_state->game_over) break;
    }

    // Final message repeated (master already posted final state)
    printf("\n=== Juego Terminado ===\n");

    // Clean up
    munmap(game_state, state_size);
    munmap(game_sync, sizeof(game_sync_t));
    close(shm_state_fd);
    close(shm_sync_fd);
    return 0;
}

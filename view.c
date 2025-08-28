#define _XOPEN_SOURCE 700
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>   // for setlocale

// NOTE: view only needs read access to shared state
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
    // You asked for "☺" — it's a multi-byte Unicode glyph. If it misaligns in your terminal,
    // replace with "O" or a digit.
    const char *head_glyph = "☺";
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
                    // rewards are 1..9 so single-digit
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
                        // Print head as colored cell with centered glyph (fg_head).
                        // Keep bg and fg applied to the entire CELL_W field so block looks contiguous.
                        // Sequence: bg + fg + " " + glyph + " " + reset
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

        // Legend / players info (aligned scoreboard beneath the board)
        printf("\n");
        // Header for stats
        printf("  Players:                         Puntos   Válidos  Inválidos\n");
        printf("  ------------------------------------------------------------\n");
        for (unsigned int i = 0; i < game_state->player_count; i++) {
            const char *bg = bg_colors[i % n_colors];
            // small colored square (2 spaces) as legend indicator + reset
            // format columns nicely: name left-aligned, numbers right-aligned
            printf("  %s  %s %-12s %20u %9u %11u\n",
                   bg, reset,
                   game_state->players[i].name,
                   game_state->players[i].score,
                   game_state->players[i].valid_moves,
                   game_state->players[i].invalid_moves);
        }

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

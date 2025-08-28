#define _XOPEN_SOURCE 700
#include "common.h"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    
    // Abrir memoria compartida del estado
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
    
    while (!game_state->game_over) {
        // Esperar notificación del máster
        sem_wait(&game_sync->master_to_view);

        // Limpiar pantalla
        printf("\033[2J\033[H");

        // Imprimir tablero
        printf("Tablero:\n");
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                int cell = game_state->board[i * width + j];
                if (cell > 0) {
                    printf("%2d ", cell); // Recompensa
                } else {
                    printf("P%d ", -cell); // Jugador
                }
            }
            printf("\n");
        }

        // Imprimir información de jugadores
        printf("\nJugadores:\n");
        for (unsigned int i = 0; i < game_state->player_count; i++) {
            printf("%s: Puntos=%u, Válidos=%u, Inválidos=%u, Posición=(%d,%d), %s\n",
                   game_state->players[i].name,
                   game_state->players[i].score,
                   game_state->players[i].valid_moves,
                   game_state->players[i].invalid_moves,
                   game_state->players[i].x,
                   game_state->players[i].y,
                   game_state->players[i].blocked ? "BLOQUEADO" : "ACTIVO");
        }

        // Notificar al máster que hemos terminado
        sem_post(&game_sync->view_to_master);

        if (game_state->game_over) {
            break;
        }
    }

    // Mostrar estado final y ganador
    printf("\n=== Juego Terminado ===\n");
    printf("Tablero final:\n");
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int cell = game_state->board[i * width + j];
            if (cell > 0) {
                printf("%2d ", cell); // Recompensa
            } else {
                printf("P%d ", -cell); // Jugador
            }
        }
        printf("\n");
    }
    printf("\nJugadores:\n");
    for (unsigned int i = 0; i < game_state->player_count; i++) {
        printf("%s: Puntos=%u, Válidos=%u, Inválidos=%u, Posición=(%d,%d), %s\n",
               game_state->players[i].name,
               game_state->players[i].score,
               game_state->players[i].valid_moves,
               game_state->players[i].invalid_moves,
               game_state->players[i].x,
               game_state->players[i].y,
               game_state->players[i].blocked ? "BLOQUEADO" : "ACTIVO");
    }

    // Determinar ganador
    int winner = -1;
    unsigned int max_score = 0;
    unsigned int min_valid_moves = 99999;
    unsigned int min_invalid_moves = 99999;
    for (unsigned int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].score > max_score) {
            max_score = game_state->players[i].score;
            winner = i;
            min_valid_moves = game_state->players[i].valid_moves;
            min_invalid_moves = game_state->players[i].invalid_moves;
        } else if (game_state->players[i].score == max_score) {
            if (game_state->players[i].valid_moves < min_valid_moves) {
                winner = i;
                min_valid_moves = game_state->players[i].valid_moves;
                min_invalid_moves = game_state->players[i].invalid_moves;
            } else if (game_state->players[i].valid_moves == min_valid_moves) {
                if (game_state->players[i].invalid_moves < min_invalid_moves) {
                    winner = i;
                    min_invalid_moves = game_state->players[i].invalid_moves;
                }
            }
        }
    }
    if (winner != -1) {
        printf("Ganador: %s con %u puntos\n", game_state->players[winner].name, game_state->players[winner].score);
    } else {
        printf("Empate\n");
    }

    // Limpiar recursos
    munmap(game_state, state_size);
    munmap(game_sync, sizeof(game_sync_t));
    close(shm_state_fd);
    close(shm_sync_fd);
    
    return 0;
}
#define _XOPEN_SOURCE 700
#include "common.h"
#include "shm_manager.h"
#include <getopt.h>
#include <errno.h>

/*
 master.c modificado para:
  - inicializar semáforos player_mutex en 0 (los jugadores esperan)
  - operar en round-robin: post player_mutex[i], esperar el byte del jugador i
  - procesar cada movimiento bajo state_mutex
  - timeout por jugador igual a delay_ms (configurable)
*/

 // Variables globales
game_state_t *game_state = NULL;
game_sync_t *game_sync = NULL;
shm_manager_t *state_mgr = NULL;
shm_manager_t *sync_mgr = NULL;
int player_pipes[MAX_PLAYERS][2];

// Función para limpiar recursos
void cleanup() {
    // Destroy/unlink shared regions (master created them, so it should destroy)
    if (state_mgr != NULL) {
        shm_manager_destroy(state_mgr);
        state_mgr = NULL;
        game_state = NULL;
    }
    if (sync_mgr != NULL) {
        shm_manager_destroy(sync_mgr);
        sync_mgr = NULL;
        game_sync = NULL;
    }

    // Cerrar pipes
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_pipes[i][PIPE_READ] != -1) close(player_pipes[i][PIPE_READ]);
        if (player_pipes[i][PIPE_WRITE] != -1) close(player_pipes[i][PIPE_WRITE]);
    }
}

// Manejador de señales
void signal_handler(int sig) {
    (void)sig;
    cleanup();
    exit(EXIT_FAILURE);
}

void initialize_board(int seed) {
    srand(seed);
    for (int i = 0; i < game_state->height; i++) {
        for (int j = 0; j < game_state->width; j++) {
            game_state->board[i * game_state->width + j] = (rand() % 9) + 1;
        }
    }
}

void place_players() {
    int positions[MAX_PLAYERS][2] = {
        {0, 0},
        {0, game_state->width - 1},
        {game_state->height - 1, 0},
        {game_state->height - 1, game_state->width - 1},
        {game_state->height / 2, game_state->width / 2},
        {0, game_state->width / 2},
        {game_state->height - 1, game_state->width / 2},
        {game_state->height / 2, 0},
        {game_state->height / 2, game_state->width - 1}
    };
    
    for (unsigned int i = 0; i < game_state->player_count; i++) {
        game_state->players[i].x = positions[i][1];
        game_state->players[i].y = positions[i][0];
        game_state->board[positions[i][0] * game_state->width + positions[i][1]] = -(i+1);
    }
}

bool is_valid_move(int player_id, direction_t direction) {
    int x = game_state->players[player_id].x;
    int y = game_state->players[player_id].y;
    int new_x = x, new_y = y;

    switch (direction) {
        case UP: new_y--; break;
        case UP_RIGHT: new_y--; new_x++; break;
        case RIGHT: new_x++; break;
        case DOWN_RIGHT: new_y++; new_x++; break;
        case DOWN: new_y++; break;
        case DOWN_LEFT: new_y++; new_x--; break;
        case LEFT: new_x--; break;
        case UP_LEFT: new_y--; new_x--; break;
    }

    if (new_x < 0 || new_x >= game_state->width || new_y < 0 || new_y >= game_state->height) {
        return false;
    }

    int cell_value = game_state->board[new_y * game_state->width + new_x];
    if (cell_value <= 0) {
        return false;
    }

    return true;
}

void apply_move(int player_id, direction_t direction) {
    int x = game_state->players[player_id].x;
    int y = game_state->players[player_id].y;
    int new_x = x, new_y = y;
    
    switch (direction) {
        case UP: new_y--; break;
        case UP_RIGHT: new_y--; new_x++; break;
        case RIGHT: new_x++; break;
        case DOWN_RIGHT: new_y++; new_x++; break;
        case DOWN: new_y++; break;
        case DOWN_LEFT: new_y++; new_x--; break;
        case LEFT: new_x--; break;
        case UP_LEFT: new_y--; new_x--; break;
    }
    
    int reward = game_state->board[new_y * game_state->width + new_x];
    game_state->players[player_id].score += reward;
    game_state->board[new_y * game_state->width + new_x] = -(player_id+1);
    game_state->players[player_id].x = new_x;
    game_state->players[player_id].y = new_y;
    game_state->players[player_id].valid_moves++;
}

bool any_player_has_valid_move() {
    for (unsigned int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].blocked) continue;
        for (int d = 0; d < 8; d++) {
            if (is_valid_move(i, (direction_t)d)) return true;
        }
    }
    return false;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int width = 10;
    int height = 10;
    int delay_ms = 200;
    int timeout_sec = 10;
    int seed = time(NULL);
    char *view_path = NULL;
    char *player_paths[MAX_PLAYERS];
    int player_count = 0;

    int max_fd = 0;
    struct timeval last_valid_move_time, current_time;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player_pipes[i][PIPE_READ] = -1;
        player_pipes[i][PIPE_WRITE] = -1;
    }
    
    int opt;
    extern char *optarg;
    extern int optind;
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1) {
        switch (opt) {
            case 'w': width = atoi(optarg); break;
            case 'h': height = atoi(optarg); break;
            case 'd': delay_ms = atoi(optarg); break;
            case 't': timeout_sec = atoi(optarg); break;
            case 's': seed = atoi(optarg); break;
            case 'v': view_path = optarg; break;
            case 'p':
                if (player_count < MAX_PLAYERS) {
                    player_paths[player_count++] = optarg;
                } else {
                    fprintf(stderr, "Máximo de jugadores alcanzado (%d)\n", MAX_PLAYERS);
                }
                break;
            default:
                fprintf(stderr, "Uso: %s [-w width] [-h height] [-d delay] [-t timeout] [-s seed] [-v view] -p player1 [player2 ...]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    while (optind < argc && player_count < MAX_PLAYERS) {
        player_paths[player_count++] = argv[optind++];
    }
    
    if (player_count == 0) {
        fprintf(stderr, "Debe especificar al menos un jugador\n");
        exit(EXIT_FAILURE);
    }
    
    // --- use shm_manager to create and map shared regions ---
    size_t state_size = sizeof(game_state_t) + width * height * sizeof(int);
    state_mgr = shm_manager_create(SHM_GAME_STATE, state_size, 0666, 0 /*no front sem*/, 0);
    if (!state_mgr) {
        perror("shm_manager_create state");
        exit(EXIT_FAILURE);
    }
    game_state = (game_state_t *)shm_manager_data(state_mgr);

    sync_mgr = shm_manager_create(SHM_GAME_SYNC, sizeof(game_sync_t), 0666, 0 /*no front sem*/, 0);
    if (!sync_mgr) {
        perror("shm_manager_create sync");
        shm_manager_destroy(state_mgr);
        exit(EXIT_FAILURE);
    }
    game_sync = (game_sync_t *)shm_manager_data(sync_mgr);
    // -----------------------------------------------------------

    // Inicializar game_state
    game_state->width = width;
    game_state->height = height;
    game_state->player_count = player_count;
    game_state->game_over = false;
    
    for (int i = 0; i < player_count; i++) {
        snprintf(game_state->players[i].name, 16, "Player%d", i+1);
        game_state->players[i].score = 0;
        game_state->players[i].invalid_moves = 0;
        game_state->players[i].valid_moves = 0;
        game_state->players[i].blocked = false;
        game_state->players[i].pid = 0;
    }
    
    initialize_board(seed);
    place_players();

    // Inicializar semáforos del game_sync en el area mapeada
    if (sem_init(&game_sync->master_to_view, 1, 0) == -1) {
        perror("sem_init master_to_view");
        cleanup();
        exit(EXIT_FAILURE);
    }
    if (sem_init(&game_sync->view_to_master, 1, 0) == -1) {
        perror("sem_init view_to_master");
        cleanup();
        exit(EXIT_FAILURE);
    }
    if (sem_init(&game_sync->master_mutex, 1, 1) == -1) {
        perror("sem_init master_mutex");
        cleanup();
        exit(EXIT_FAILURE);
    }
    if (sem_init(&game_sync->state_mutex, 1, 1) == -1) {
        perror("sem_init state_mutex");
        cleanup();
        exit(EXIT_FAILURE);
    }
    if (sem_init(&game_sync->reader_count_mutex, 1, 1) == -1) {
        perror("sem_init reader_count_mutex");
        cleanup();
        exit(EXIT_FAILURE);
    }
    game_sync->reader_count = 0;
    /* IMPORTANT: initialize per-player semaphores to 0 (players must wait for master) */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (sem_init(&game_sync->player_mutex[i], 1, 0) == -1) {
            perror("sem_init player_mutex");
            cleanup();
            exit(EXIT_FAILURE);
        }
    }

    // Crear proceso de vista si se especificó
    pid_t view_pid = -1;
    if (view_path != NULL) {
        view_pid = fork();
        if (view_pid == -1) {
            perror("fork view");
            cleanup();
            exit(EXIT_FAILURE);
        } else if (view_pid == 0) {
            char width_str[10], height_str[10];
            snprintf(width_str, sizeof(width_str), "%d", width);
            snprintf(height_str, sizeof(height_str), "%d", height);
            execl(view_path, view_path, width_str, height_str, NULL);
            perror("execl view");
            exit(EXIT_FAILURE);
        }
    }

    if (view_path != NULL) {
        sem_post(&game_sync->master_to_view);
        sem_wait(&game_sync->view_to_master);
    }

    // Crear pipes para los jugadores
    for (int i = 0; i < player_count; i++) {
        if (pipe(player_pipes[i]) == -1) {
            perror("pipe");
            cleanup();
            exit(EXIT_FAILURE);
        }
    }

    // Crear procesos de jugadores
    for (int i = 0; i < player_count; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            cleanup();
            exit(EXIT_FAILURE);
        } else if (pid == 0) {
            // Proceso hijo (jugador)
            close(player_pipes[i][PIPE_READ]);
            dup2(player_pipes[i][PIPE_WRITE], STDOUT_FILENO);
            close(player_pipes[i][PIPE_WRITE]);

            char width_str[10], height_str[10];
            snprintf(width_str, sizeof(width_str), "%d", width);
            snprintf(height_str, sizeof(height_str), "%d", height);

            execl(player_paths[i], player_paths[i], width_str, height_str, NULL);
            perror("execl");
            exit(EXIT_FAILURE);
        } else {
            // Padre
            close(player_pipes[i][PIPE_WRITE]);
            game_state->players[i].pid = pid;
            if (player_pipes[i][PIPE_READ] > max_fd) max_fd = player_pipes[i][PIPE_READ];
        }
    }

    gettimeofday(&last_valid_move_time, NULL);
    
    /* Round-robin loop: for each player, post its semaphore so it can submit one move,
       wait up to delay_ms for that player's byte, process it, then move to next player. */
    while (!game_state->game_over) {
        for (int i = 0; i < player_count; i++) {
            if (player_pipes[i][PIPE_READ] == -1) continue;      // pipe cerrado
            if (game_state->players[i].blocked) continue;       // jugador desconectado

            /* allow player i to make exactly one move */
            sem_post(&game_sync->player_mutex[i]);

            /* wait for up to delay_ms for the player's move */
            struct timeval timeout;
            timeout.tv_sec = delay_ms / 1000;
            timeout.tv_usec = (delay_ms % 1000) * 1000;

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(player_pipes[i][PIPE_READ], &rfds);

            int ready = select(player_pipes[i][PIPE_READ] + 1, &rfds, NULL, NULL, &timeout);
            if (ready == -1) { perror("select"); break; }
            if (ready == 0) {
                /* player did not send a move in time; skip */
                continue;
            }

            if (FD_ISSET(player_pipes[i][PIPE_READ], &rfds)) {
                unsigned char move;
                ssize_t bytes_read = read(player_pipes[i][PIPE_READ], &move, 1);
                if (bytes_read == 0) {
                    /* player closed pipe -> mark blocked */
                    game_state->players[i].blocked = true;
                    close(player_pipes[i][PIPE_READ]);
                    player_pipes[i][PIPE_READ] = -1;
                } else if (bytes_read == 1) {
                    if (sem_wait(&game_sync->state_mutex) == -1) {
                        perror("sem_wait state_mutex");
                        break;
                    }

                    if (move > 7) {
                        game_state->players[i].invalid_moves++;
                    } else if (is_valid_move(i, (direction_t)move)) {
                        apply_move(i, (direction_t)move);
                        gettimeofday(&last_valid_move_time, NULL);
                    } else {
                        game_state->players[i].invalid_moves++;
                    }

                    sem_post(&game_sync->state_mutex);

                    if (view_path != NULL) {
                        sem_post(&game_sync->master_to_view);
                        sem_wait(&game_sync->view_to_master);
                    }

                    struct timespec ts = {0, delay_ms * 1000000};
                    nanosleep(&ts, NULL);
                }
            }
        }

        if (!any_player_has_valid_move()) {
            game_state->game_over = true;
            break;
        }

        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - last_valid_move_time.tv_sec) +
                        (current_time.tv_usec - last_valid_move_time.tv_usec) / 1000000.0;
        if (elapsed >= timeout_sec) { game_state->game_over = true; break; }

        bool all_blocked = true;
        for (int i = 0; i < player_count; i++) {
            if (!game_state->players[i].blocked) { all_blocked = false; break; }
        }
        if (all_blocked) { game_state->game_over = true; break; }
    }
    
    if (view_path != NULL) {
        sem_post(&game_sync->master_to_view);
        sem_wait(&game_sync->view_to_master);
    }

    for (int i = 0; i < player_count; i++) {
        int status;
        waitpid(game_state->players[i].pid, &status, 0);
        printf("Jugador %s: ", game_state->players[i].name);
        if (WIFEXITED(status)) printf("exit code %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) printf("señal %d", WTERMSIG(status));
        printf(", Puntaje: %u\n", game_state->players[i].score);
    }
    
    if (view_path != NULL) waitpid(view_pid, NULL, 0);
    
    // decide winner (unchanged)
    int winner = -1;
    unsigned int max_score = 0;
    unsigned int min_valid_moves = 99999;
    unsigned int min_invalid_moves = 99999;
    for (int i = 0; i < player_count; i++) {
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
    if (winner != -1) printf("Ganador: %s con %u puntos\n", game_state->players[winner].name, game_state->players[winner].score);
    else printf("Empate\n");
    
    cleanup();
    return 0;
}

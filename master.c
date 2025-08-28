#define _XOPEN_SOURCE 700
#include "common.h"
#include <getopt.h>

// Variables globales
game_state_t *game_state = NULL;
game_sync_t *game_sync = NULL;
int shm_state_fd = -1, shm_sync_fd = -1;
int player_pipes[MAX_PLAYERS][2];

// Función para limpiar recursos
void cleanup() {
    // Cerrar y unlink memorias compartidas
    if (game_state != NULL && game_state != MAP_FAILED) {
        size_t state_size = sizeof(game_state_t) + game_state->width * game_state->height * sizeof(int);
        munmap(game_state, state_size);
    }
    if (game_sync != NULL && game_sync != MAP_FAILED) {
        munmap(game_sync, sizeof(game_sync_t));
    }
    if (shm_state_fd != -1) {
        close(shm_state_fd);
        shm_unlink(SHM_GAME_STATE);
    }
    if (shm_sync_fd != -1) {
        close(shm_sync_fd);
        shm_unlink(SHM_GAME_SYNC);
    }
    
    // Cerrar pipes
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_pipes[i][PIPE_READ] != -1) close(player_pipes[i][PIPE_READ]);
        if (player_pipes[i][PIPE_WRITE] != -1) close(player_pipes[i][PIPE_WRITE]);
    }
}

// Manejador de señales
void signal_handler(int sig) {
    (void)sig; // Silenciar advertencia de parámetro no utilizado
    cleanup();
    exit(EXIT_FAILURE);
}

// Inicializar el tablero con recompensas
void initialize_board(int seed) {
    srand(seed);
    for (int i = 0; i < game_state->height; i++) {
        for (int j = 0; j < game_state->width; j++) {
            game_state->board[i * game_state->width + j] = (rand() % 9) + 1;
        }
    }
}

// Colocar jugadores en el tablero
void place_players() {
    // Distribuir jugadores en posiciones iniciales
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
        // Marcar celda como capturada
        game_state->board[positions[i][0] * game_state->width + positions[i][1]] = -(i+1);
    }
}

// Verificar si un movimiento es válido
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

    // Verificar límites
    if (new_x < 0 || new_x >= game_state->width || new_y < 0 || new_y >= game_state->height) {
        return false;
    }

    // Verificar que la celda esté libre (debe ser >0 para poder moverse)
    int cell_value = game_state->board[new_y * game_state->width + new_x];
    if (cell_value <= 0) {
        return false;
    }

    return true;
}

// Aplicar un movimiento válido
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
    
    // Obtener recompensa
    int reward = game_state->board[new_y * game_state->width + new_x];
    game_state->players[player_id].score += reward;
    
    // Marcar celda como capturada
    game_state->board[new_y * game_state->width + new_x] = -(player_id+1);
    
    // Actualizar posición
    game_state->players[player_id].x = new_x;
    game_state->players[player_id].y = new_y;
    
    // Incrementar contador de movimientos válidos
    game_state->players[player_id].valid_moves++;
}

int main(int argc, char *argv[]) {
    // Configurar manejador de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Valores por defecto
    int width = 10;
    int height = 10;
    int delay_ms = 200;
    int timeout_sec = 10;
    int seed = time(NULL);
    char *view_path = NULL;
    char *player_paths[MAX_PLAYERS];
    int player_count = 0;

    // Declaraciones para select y tiempo
    fd_set read_fds;
    int max_fd = 0;
    struct timeval last_valid_move_time, current_time;
    
    // Inicializar pipes
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player_pipes[i][PIPE_READ] = -1;
        player_pipes[i][PIPE_WRITE] = -1;
    }
    
    // Parsear argumentos
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
                // Store the player path provided as optarg.
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
    // Recoger los paths de los jugadores después de getopt (so both styles work)
    while (optind < argc && player_count < MAX_PLAYERS) {
        player_paths[player_count++] = argv[optind++];
    }
    
    if (player_count == 0) {
        fprintf(stderr, "Debe especificar al menos un jugador\n");
        exit(EXIT_FAILURE);
    }
    
    // Crear memorias compartidas
    shm_state_fd = shm_open(SHM_GAME_STATE, O_CREAT | O_RDWR, 0666);
    if (shm_state_fd == -1) {
        perror("shm_open state");
        exit(EXIT_FAILURE);
    }
    
    size_t state_size = sizeof(game_state_t) + width * height * sizeof(int);
    if (ftruncate(shm_state_fd, state_size) == -1) {
        perror("ftruncate state");
        cleanup();
        exit(EXIT_FAILURE);
    }
    
    game_state = mmap(NULL, state_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_state_fd, 0);
    if (game_state == MAP_FAILED) {
        perror("mmap state");
        cleanup();
        exit(EXIT_FAILURE);
    }
    
    shm_sync_fd = shm_open(SHM_GAME_SYNC, O_CREAT | O_RDWR, 0666);
    if (shm_sync_fd == -1) {
        perror("shm_open sync");
        cleanup();
        exit(EXIT_FAILURE);
    }
    
    if (ftruncate(shm_sync_fd, sizeof(game_sync_t)) == -1) {
        perror("ftruncate sync");
        cleanup();
        exit(EXIT_FAILURE);
    }
    
    game_sync = mmap(NULL, sizeof(game_sync_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_sync_fd, 0);
    if (game_sync == MAP_FAILED) {
        perror("mmap sync");
        cleanup();
        exit(EXIT_FAILURE);
    }
    
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

    // Inicializar semáforos
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
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (sem_init(&game_sync->player_mutex[i], 1, 1) == -1) {
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

    // Notificar a la vista del estado inicial (después de fork)
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
            close(player_pipes[i][PIPE_READ]); // Cierra el extremo de lectura en el hijo
            dup2(player_pipes[i][PIPE_WRITE], STDOUT_FILENO); // Redirige stdout al pipe
            close(player_pipes[i][PIPE_WRITE]); // Cierra el extremo de escritura duplicado

            char width_str[10], height_str[10];
            snprintf(width_str, sizeof(width_str), "%d", width);
            snprintf(height_str, sizeof(height_str), "%d", height);

            execl(player_paths[i], player_paths[i], width_str, height_str, NULL);
            perror("execl");
            exit(EXIT_FAILURE);
        } else {
            // Proceso padre (máster)
            close(player_pipes[i][PIPE_WRITE]); // Cierra el extremo de escritura en el padre
            game_state->players[i].pid = pid;
        }
    }

    // Inicializar conjunto de file descriptors
    FD_ZERO(&read_fds);
    for (int i = 0; i < player_count; i++) {
        if (player_pipes[i][PIPE_READ] != -1) {
            FD_SET(player_pipes[i][PIPE_READ], &read_fds);
            if (player_pipes[i][PIPE_READ] > max_fd) {
                max_fd = player_pipes[i][PIPE_READ];
            }
        }
    }

    // IMPORTANT: inicializar last_valid_move_time para evitar timeout inmediato
    gettimeofday(&last_valid_move_time, NULL);
    
    while (!game_state->game_over) {
        // Configurar timeout
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms
        
        fd_set tmp_fds = read_fds;
        int ready = select(max_fd + 1, &tmp_fds, NULL, NULL, &timeout);
        
        if (ready == -1) {
            perror("select");
            break;
        }
        
        // Procesar movimientos de jugadores
        for (int i = 0; i < player_count; i++) {
            if (player_pipes[i][PIPE_READ] != -1 && FD_ISSET(player_pipes[i][PIPE_READ], &tmp_fds)) {
                unsigned char move;
                ssize_t bytes_read = read(player_pipes[i][PIPE_READ], &move, 1);

                if (bytes_read == 0) {
                    // EOF - jugador bloqueado
                    game_state->players[i].blocked = true;
                    FD_CLR(player_pipes[i][PIPE_READ], &read_fds);
                    close(player_pipes[i][PIPE_READ]);
                    player_pipes[i][PIPE_READ] = -1;
                } else if (bytes_read == 1) {
                    // Procesar movimiento
                    sem_wait(&game_sync->state_mutex);

                    if (move > 7) {
                        // Movimiento inválido
                        game_state->players[i].invalid_moves++;
                    } else if (is_valid_move(i, (direction_t)move)) {
                        // Movimiento válido
                        apply_move(i, (direction_t)move);
                        // NOTE: apply_move already increments valid_moves
                        gettimeofday(&last_valid_move_time, NULL);
                    } else {
                        // Movimiento inválido
                        game_state->players[i].invalid_moves++;
                    }

                    sem_post(&game_sync->state_mutex);

                    // Notificar a la vista si está presente
                    if (view_path != NULL) {
                        sem_post(&game_sync->master_to_view);
                        sem_wait(&game_sync->view_to_master);
                    }

                    // Pequeña espera para simular el delay
                    struct timespec ts = {0, delay_ms * 1000000};
                    nanosleep(&ts, NULL);
                }
            }
        }
        
        // Verificar timeout
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - last_valid_move_time.tv_sec) +
                        (current_time.tv_usec - last_valid_move_time.tv_usec) / 1000000.0;
        
        if (elapsed >= timeout_sec) {
            game_state->game_over = true;
            break;
        }
        
        // Verificar si todos los jugadores están bloqueados
        bool all_blocked = true;
        for (int i = 0; i < player_count; i++) {
            if (!game_state->players[i].blocked) {
                all_blocked = false;
                break;
            }
        }
        
        if (all_blocked) {
            game_state->game_over = true;
            break;
        }
    }
    
    // Notificar a la vista del estado final antes de terminar
    if (view_path != NULL) {
        sem_post(&game_sync->master_to_view);
        sem_wait(&game_sync->view_to_master);
    }

    // Juego terminado - esperar a que los procesos hijos terminen
    for (int i = 0; i < player_count; i++) {
        int status;
        waitpid(game_state->players[i].pid, &status, 0);
        
        printf("Jugador %s: ", game_state->players[i].name);
        if (WIFEXITED(status)) {
            printf("exit code %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("señal %d", WTERMSIG(status));
        }
        printf(", Puntaje: %u\n", game_state->players[i].score);
    }
    
    if (view_path != NULL) {
        waitpid(view_pid, NULL, 0);
    }
    
    // Determinar ganador
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
    
    if (winner != -1) {
        printf("Ganador: %s con %u puntos\n", game_state->players[winner].name, game_state->players[winner].score);
    } else {
        printf("Empate\n");
    }
    
    cleanup();
    return 0;
}

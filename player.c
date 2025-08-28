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
    
    // IA simple: siempre intenta moverse hacia arriba
    unsigned char move = UP;
    
    while (!game_state->game_over) {
        // Pequeña espera para evitar saturación
        struct timespec ts = {0, 10000000}; // 10ms
        nanosleep(&ts, NULL);
        
        // Enviar movimiento
        ssize_t bytes_written = write(STDOUT_FILENO, &move, 1);
        if (bytes_written != 1) {
            break; // Error o EOF
        }
    }
    
    // Limpiar recursos
    munmap(game_state, state_size);
    munmap(game_sync, sizeof(game_sync_t));
    close(shm_state_fd);
    close(shm_sync_fd);
    
    return 0;
}
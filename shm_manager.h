#ifndef SHM_MANAGER_H
#define SHM_MANAGER_H

#include <semaphore.h>
#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shm_manager shm_manager_t;

shm_manager_t *shm_manager_create(const char *name, size_t data_size, mode_t mode,
                                  int with_front_sem, unsigned int sem_init_value);

shm_manager_t *shm_manager_open(const char *name, size_t data_size, int with_front_sem);

int shm_manager_close(shm_manager_t *mgr);

int shm_manager_destroy(shm_manager_t *mgr);

/* Accessors */
void *shm_manager_data(shm_manager_t *mgr);   
size_t shm_manager_size(shm_manager_t *mgr);
sem_t *shm_manager_front_sem(shm_manager_t *mgr); 
const char *shm_manager_name(shm_manager_t *mgr);

#ifdef __cplusplus
}
#endif

#endif 

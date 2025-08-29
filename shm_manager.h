#ifndef SHM_MANAGER_H
#define SHM_MANAGER_H

#include <semaphore.h>
#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shm_manager shm_manager_t;

/**
 * Create and map a shared memory region.
 *
 * name: name used for shm_open (e.g. "/game_state")
 * data_size: number of bytes of usable data you want available to the caller
 * mode: permissions (e.g. 0666)
 * with_front_sem: if non-zero, the mapping will reserve space for an unnamed
 *                 sem_t at the front of the mapping (the library will initialize it).
 * sem_init_value: initial value for the front semaphore (only used if with_front_sem != 0)
 *
 * Returns pointer to shm_manager_t on success, NULL on failure (errno set).
 *
 * Layout when with_front_sem != 0:
 *   [ sem_t ][ data_size bytes... ]
 *
 * When with_front_sem == 0:
 *   [ data_size bytes... ]
 *
 * Use shm_manager_data() to get pointer to usable data area.
 */
shm_manager_t *shm_manager_create(const char *name, size_t data_size, mode_t mode,
                                  int with_front_sem, unsigned int sem_init_value);

/**
 * Open and map an existing shared memory region (created previously).
 * Provide the same data_size and with_front_sem you used when creating.
 *
 * Returns pointer to shm_manager_t on success, NULL on failure.
 */
shm_manager_t *shm_manager_open(const char *name, size_t data_size, int with_front_sem);

/**
 * Unmap and close (but does NOT unlink) the shared memory region.
 * After close, the returned pointer becomes invalid and should not be used.
 * Returns 0 on success, -1 on failure (errno set).
 */
int shm_manager_close(shm_manager_t *mgr);

/**
 * Destroy a shared memory region previously created with shm_manager_create:
 * - sem_destroy (if front sem present)
 * - munmap
 * - shm_unlink
 * Returns 0 on success, -1 on failure (errno set).
 *
 * Note: calling shm_manager_destroy on a region opened with shm_manager_open is allowed,
 * but will unlink the underlying shared memory object (so don't call destroy from multiple processes).
 */
int shm_manager_destroy(shm_manager_t *mgr);

/* Accessors */
void *shm_manager_data(shm_manager_t *mgr);   /* pointer to usable data area */
size_t shm_manager_size(shm_manager_t *mgr);
sem_t *shm_manager_front_sem(shm_manager_t *mgr); /* NULL if front sem not present */
const char *shm_manager_name(shm_manager_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* SHM_MANAGER_H */

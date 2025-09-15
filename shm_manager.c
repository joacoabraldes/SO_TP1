#include "shm_manager.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <semaphore.h> 

struct shm_manager {
    char *name;        
    void *map;         
    size_t map_size;   
    size_t data_size;  
    int fd;            
    int has_front_sem; 
    int read_only;     
};

shm_manager_t *shm_manager_create(const char *name, size_t data_size, mode_t mode,
                                  int with_front_sem, unsigned int sem_init_value)
{
    if (!name || data_size == 0) {
        errno = EINVAL;
        return NULL;
    }

    int flags = O_CREAT | O_RDWR;
    int fd = shm_open(name, flags, mode);
    if (fd == -1) return NULL;

    size_t map_size = data_size + (with_front_sem ? sizeof(sem_t) : 0);

    if (ftruncate(fd, (off_t)map_size) == -1) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    if (with_front_sem) {
        sem_t *s = (sem_t *)map;
        if (sem_init(s, 1, sem_init_value) == -1) {
            int saved = errno;
            munmap(map, map_size);
            close(fd);
            errno = saved;
            return NULL;
        }
    }

    shm_manager_t *r = calloc(1, sizeof(shm_manager_t));
    if (!r) {
        int saved = errno;
        if (with_front_sem) sem_destroy((sem_t *)map);
        munmap(map, map_size);
        close(fd);
        errno = saved;
        return NULL;
    }

    r->name = strdup(name);
    r->map = map;
    r->map_size = map_size;
    r->data_size = data_size;
    r->fd = fd;
    r->has_front_sem = with_front_sem ? 1 : 0;
    r->read_only = 0;

    return r;
}

shm_manager_t *shm_manager_open(const char *name, size_t data_size, int with_front_sem)
{
    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    int fd = shm_open(name, O_RDWR, 0);
    int read_only = 0;

    if (fd == -1) {
        if (errno == EACCES && !with_front_sem) {
            fd = shm_open(name, O_RDONLY, 0);
            if (fd != -1) read_only = 1;
        }
    }

    if (fd == -1) {
        return NULL;
    }

    size_t map_size;

    if (data_size == 0) {
        struct stat st;
        if (fstat(fd, &st) == -1) {
            int saved = errno;
            close(fd);
            errno = saved;
            return NULL;
        }
        if (st.st_size == 0) {
            close(fd);
            errno = EINVAL;
            return NULL;
        }
        map_size = (size_t)st.st_size;

        if (with_front_sem && map_size < sizeof(sem_t)) {
            close(fd);
            errno = EINVAL;
            return NULL;
        }
    } else {
        map_size = data_size + (with_front_sem ? sizeof(sem_t) : 0);
    }

    int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);

    void *map = mmap(NULL, map_size, prot, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    shm_manager_t *r = calloc(1, sizeof(shm_manager_t));
    if (!r) {
        int saved = errno;
        munmap(map, map_size);
        close(fd);
        errno = saved;
        return NULL;
    }

    r->name = strdup(name);
    r->map = map;
    r->map_size = map_size;
    if (data_size == 0) {
        r->data_size = map_size - (with_front_sem ? sizeof(sem_t) : 0);
    } else {
        r->data_size = data_size;
    }
    r->fd = fd;
    r->has_front_sem = with_front_sem ? 1 : 0;
    r->read_only = read_only ? 1 : 0;

    return r;
}

int shm_manager_close(shm_manager_t *r)
{
    if (!r) {
        errno = EINVAL;
        return -1;
    }

    int rc = 0;
    if (munmap(r->map, r->map_size) == -1) rc = -1;
    if (r->fd != -1) {
        if (close(r->fd) == -1) rc = -1;
        r->fd = -1;
    }
    free(r->name);
    free(r);
    return rc;
}

int shm_manager_destroy(shm_manager_t *r)
{
    if (!r) {
        errno = EINVAL;
        return -1;
    }

    int err = 0;
    if (r->has_front_sem) {
        sem_t *s = (sem_t *)r->map;
        if (sem_destroy(s) == -1) err = errno;
    }

    if (munmap(r->map, r->map_size) == -1 && err == 0) err = errno;

    if (shm_unlink(r->name) == -1 && err == 0) err = errno;

    if (r->fd != -1) close(r->fd);

    free(r->name);
    free(r);

    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

void *shm_manager_data(shm_manager_t *r)
{
    if (!r) return NULL;
    return r->has_front_sem ? (void *)((char *)r->map + sizeof(sem_t)) : r->map;
}

size_t shm_manager_size(shm_manager_t *r)
{
    if (!r) return 0;
    return r->data_size;
}

sem_t *shm_manager_front_sem(shm_manager_t *r)
{
    if (!r || !r->has_front_sem) return NULL;
    return (sem_t *)r->map;
}

const char *shm_manager_name(shm_manager_t *r)
{
    if (!r) return NULL;
    return r->name;
}

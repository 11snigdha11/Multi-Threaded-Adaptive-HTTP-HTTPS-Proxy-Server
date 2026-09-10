#ifndef CACHE_H
#define CACHE_H

#include <pthread.h>
#include <stddef.h>

/* --- Type Definitions --- */
typedef enum { LRU, LFU } CachePolicy;

/* --- Global Variables (declared as extern) --- */
extern void *g_real_cache;
extern void *g_ghost_cache;
extern CachePolicy g_real_policy;
extern CachePolicy g_ghost_policy;
extern long long g_request_count;
extern long long g_real_hits;
extern long long g_ghost_hits;
extern pthread_mutex_t adaptive_mutex;

/* --- Function Prototypes --- */

// Main adaptive cache controller
void init_adaptive_cache(CachePolicy initial_policy);
void run_adaptation_check(void);

// Disk cache helpers
void init_disk_cache(void);
void save_to_disk_cache(const char *key, const char *data, size_t data_size);
char* key_to_filepath(const char *key);

// Generic cache interface (implemented by adaptive logic)
void put_in_cache(const char *key, const char *data, size_t data_size);
void* get_from_cache(const char *key, size_t* data_size);
void ghost_cache_access(const char* key);
void ghost_cache_put(const char* key);

#endif // CACHE_H
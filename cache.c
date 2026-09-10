#include "cache.h"
#include "utils.h" // For logging and config globals
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Global Cache State --- */
void *g_real_cache;
void *g_ghost_cache;
CachePolicy g_real_policy;
CachePolicy g_ghost_policy;
long long g_request_count = 0;
long long g_real_hits = 0;
long long g_ghost_hits = 0;
pthread_mutex_t adaptive_mutex;

/* --- Private Function Prototypes for Cache Implementations --- */
// LRU
void* init_lru_cache(size_t capacity, int table_size, int is_ghost);
void destroy_lru_cache(void* cache);
void lru_put(void* cache, const char *key, const char *data, size_t data_size, int is_ghost);
void* lru_get(void* cache, const char *key, size_t* data_size);
int lru_ghost_get(void* cache, const char *key);
// LFU
void* init_lfu_cache(size_t capacity, int table_size, int is_ghost);
void destroy_lfu_cache(void* cache);
void lfu_put(void* cache, const char *key, const char *data, size_t data_size, int is_ghost);
void* lfu_get(void* cache, const char *key, size_t* data_size);
int lfu_ghost_get(void* cache, const char *key);

/* --- Function Pointers for Generic Cache Interface --- */
void (*put_impl)(void* cache, const char* key, const char* data, size_t size, int is_ghost);
void* (*get_impl)(void* cache, const char* key, size_t* size);
int (*ghost_get_impl)(void* cache, const char* key);
void (*destroy_cache_impl)(void* cache);

static unsigned long hash_func(const char *str) { unsigned long h = 5381; int c; while ((c = *str++)) h = ((h << 5) + h) + c; return h; }

/*
================================================================================
LRU CACHE IMPLEMENTATION
================================================================================
*/
typedef struct LRUCacheNode { char *key; void *data; size_t data_size; struct LRUCacheNode *prev, *next, *h_next; } LRUCacheNode;
typedef struct { size_t capacity, size; int table_size; int is_ghost; LRUCacheNode **table, *head, *tail; } LRUCache;
void lru_detach_node(LRUCache *c, LRUCacheNode *n) { if (n->prev) n->prev->next = n->next; else c->head = n->next; if (n->next) n->next->prev = n->prev; else c->tail = n->prev; }
void lru_attach_to_front(LRUCache *c, LRUCacheNode *n) { n->next = c->head; n->prev = NULL; if (c->head) c->head->prev = n; c->head = n; if (!c->tail) c->tail = n; }
void destroy_lru_cache(void* cache) { LRUCache* c = (LRUCache*)cache; LRUCacheNode* curr = c->head; while(curr) { LRUCacheNode* next = curr->next; free(curr->key); if(!c->is_ghost) free(curr->data); free(curr); curr = next; } free(c->table); free(c); }
void lru_evict(LRUCache* c) { LRUCacheNode *n = c->tail; if (!n) return; lru_detach_node(c, n); unsigned long h = hash_func(n->key) % c->table_size; LRUCacheNode *curr = c->table[h], *prev = NULL; while(curr) { if (curr == n) { if (prev) prev->h_next = curr->h_next; else c->table[h] = curr->h_next; break; } prev = curr; curr = curr->h_next; } size_t item_size = c->is_ghost ? 1 : n->data_size; c->size -= item_size; free(n->key); if(!c->is_ghost) free(n->data); free(n); }
void lru_put(void* cache, const char *key, const char *data, size_t data_size, int is_ghost) { LRUCache* c = (LRUCache*)cache; if (!is_ghost && data_size > g_max_element_size) return; size_t item_size = is_ghost ? 1 : data_size; while (c->size + item_size > c->capacity && c->tail) lru_evict(c); if (c->size + item_size > c->capacity) return; LRUCacheNode *n = (LRUCacheNode*)calloc(1, sizeof(LRUCacheNode)); n->key = strdup(key); if(!is_ghost){ n->data = malloc(data_size); memcpy(n->data, data, data_size); n->data_size = data_size; } lru_attach_to_front(c, n); c->size += item_size; unsigned long h = hash_func(key) % c->table_size; n->h_next = c->table[h]; c->table[h] = n; }
void* lru_get(void* cache, const char *key, size_t* data_size) { LRUCache* c = (LRUCache*)cache; void* data_copy = NULL; unsigned long h = hash_func(key) % c->table_size; LRUCacheNode *n = c->table[h]; while(n) { if (strcmp(n->key, key) == 0) { lru_detach_node(c, n); lru_attach_to_front(c, n); *data_size = n->data_size; data_copy = malloc(*data_size); memcpy(data_copy, n->data, *data_size); break; } n = n->h_next; } return data_copy; }
int lru_ghost_get(void* cache, const char *key) { LRUCache* c = (LRUCache*)cache; int found = 0; unsigned long h = hash_func(key) % c->table_size; LRUCacheNode *n = c->table[h]; while(n) { if (strcmp(n->key, key) == 0) { lru_detach_node(c, n); lru_attach_to_front(c, n); found = 1; break; } n = n->h_next; } return found; }
void* init_lru_cache(size_t capacity, int table_size, int is_ghost) { LRUCache *c = (LRUCache*)malloc(sizeof(LRUCache)); c->capacity = is_ghost ? 2 * CACHE_HASHTABLE_SIZE : capacity; c->size = 0; c->table_size = table_size; c->is_ghost = is_ghost; c->head = c->tail = NULL; c->table = (LRUCacheNode**)calloc(table_size, sizeof(LRUCacheNode*)); return c; }

/*
================================================================================
LFU CACHE IMPLEMENTATION
================================================================================
*/
typedef struct FreqNode FreqNode;
typedef struct LFUCacheNode { char *key; void *data; size_t data_size; struct LFUCacheNode *prev, *next, *h_next; FreqNode *parent_freq; } LFUCacheNode;
struct FreqNode { int freq; struct FreqNode *prev, *next; LFUCacheNode *head, *tail; };
typedef struct { size_t capacity, size; int table_size; int is_ghost; LFUCacheNode **item_table; FreqNode *freq_head; } LFUCache;

static void lfu_detach_cache_node(LFUCacheNode *n) { if (n->prev) n->prev->next = n->next; else n->parent_freq->head = n->next; if (n->next) n->next->prev = n->prev; else n->parent_freq->tail = n->prev; }
static void lfu_attach_cache_node(FreqNode *f, LFUCacheNode *n) { n->next = f->head; n->prev = NULL; if (f->head) f->head->prev = n; f->head = n; if (!f->tail) f->tail = n; n->parent_freq = f; }

static void lfu_update_node_freq(LFUCache *c, LFUCacheNode *n) {
    FreqNode *old_freq = n->parent_freq;
    lfu_detach_cache_node(n);
    int new_freq_val = old_freq->freq + 1;
    FreqNode *next_freq = old_freq->next;
    if (!next_freq || next_freq->freq != new_freq_val) {
        next_freq = (FreqNode*)calloc(1, sizeof(FreqNode));
        next_freq->freq = new_freq_val;
        next_freq->next = old_freq->next; next_freq->prev = old_freq;
        if(old_freq->next) old_freq->next->prev = next_freq;
        old_freq->next = next_freq;
    }
    lfu_attach_cache_node(next_freq, n);
    if (old_freq->head == NULL) {
        if (old_freq->prev) old_freq->prev->next = old_freq->next; else c->freq_head = old_freq->next;
        if (old_freq->next) old_freq->next->prev = old_freq->prev;
        free(old_freq);
    }
}

void* init_lfu_cache(size_t capacity, int table_size, int is_ghost) { LFUCache *c = (LFUCache*)malloc(sizeof(LFUCache)); c->capacity = is_ghost ? 2 * CACHE_HASHTABLE_SIZE : capacity; c->size = 0; c->table_size = table_size; c->is_ghost = is_ghost; c->freq_head = NULL; c->item_table = (LFUCacheNode**)calloc(table_size, sizeof(LFUCacheNode*)); return c; }
void destroy_lfu_cache(void* cache) { LFUCache* c = (LFUCache*)cache; FreqNode* f_curr = c->freq_head; while(f_curr) { FreqNode* f_next = f_curr->next; LFUCacheNode* n_curr = f_curr->head; while(n_curr) { LFUCacheNode* n_next = n_curr->next; free(n_curr->key); if(!c->is_ghost) free(n_curr->data); free(n_curr); n_curr = n_next; } free(f_curr); f_curr = f_next; } free(c->item_table); free(c); }

void lfu_evict(LFUCache* c) {
    FreqNode *f = c->freq_head; if (!f) return;
    LFUCacheNode *n = f->tail; if (!n) return;
    lfu_detach_cache_node(n);
    unsigned long h = hash_func(n->key) % c->table_size;
    LFUCacheNode *curr = c->item_table[h], *prev = NULL;
    while(curr) { if (curr == n) { if (prev) prev->h_next = curr->h_next; else c->item_table[h] = curr->h_next; break; } prev = curr; curr = curr->h_next; }
    size_t item_size = c->is_ghost ? 1 : n->data_size; c->size -= item_size;
    if (f->head == NULL) { if (f->prev) f->prev->next = f->next; else c->freq_head = f->next; if (f->next) f->next->prev = f->prev; free(f); }
    free(n->key); if(!c->is_ghost) free(n->data); free(n);
}

void lfu_put(void* cache, const char *key, const char *data, size_t data_size, int is_ghost) {
    LFUCache *c = (LFUCache*)cache;
    if (!is_ghost && data_size > g_max_element_size) return;
    unsigned long h = hash_func(key) % c->table_size;
    LFUCacheNode *n = c->item_table[h];
    while(n) { if (strcmp(n->key, key) == 0) { if(!is_ghost) { c->size -= n->data_size; free(n->data); n->data = malloc(data_size); memcpy(n->data, data, data_size); n->data_size = data_size; c->size += data_size; } lfu_update_node_freq(c, n); return; } n = n->h_next; }
    size_t item_size = is_ghost ? 1 : data_size;
    while(c->size + item_size > c->capacity) lfu_evict(c);
    if (c->size + item_size > c->capacity) return;
    n = (LFUCacheNode*)calloc(1, sizeof(LFUCacheNode));
    n->key = strdup(key);
    if(!is_ghost) { n->data = malloc(data_size); memcpy(n->data, data, data_size); n->data_size = data_size; }
    c->size += item_size;
    FreqNode *freq1 = c->freq_head;
    if (!freq1 || freq1->freq != 1) { freq1 = (FreqNode*)calloc(1, sizeof(FreqNode)); freq1->freq = 1; freq1->next = c->freq_head; if (c->freq_head) c->freq_head->prev = freq1; c->freq_head = freq1; }
    lfu_attach_cache_node(freq1, n);
    n->h_next = c->item_table[h]; c->item_table[h] = n;
}

void* lfu_get(void* cache, const char *key, size_t* data_size) { LFUCache *c = (LFUCache*)cache; void* data_copy = NULL; unsigned long h = hash_func(key) % c->table_size; LFUCacheNode *n = c->item_table[h]; while(n) { if (strcmp(n->key, key) == 0) { lfu_update_node_freq(c, n); *data_size = n->data_size; data_copy = malloc(*data_size); memcpy(data_copy, n->data, *data_size); break; } n = n->h_next; } return data_copy; }
int lfu_ghost_get(void* cache, const char *key) { LFUCache *c = (LFUCache*)cache; int found = 0; unsigned long h = hash_func(key) % c->table_size; LFUCacheNode *n = c->item_table[h]; while(n) { if (strcmp(n->key, key) == 0) { lfu_update_node_freq(c, n); found = 1; break; } n = n->h_next; } return found; }

/*
================================================================================
ADAPTIVE CACHE CONTROLLER
================================================================================
*/
void init_adaptive_cache(CachePolicy initial_policy) {
    pthread_mutex_init(&adaptive_mutex, NULL);
    g_real_policy = initial_policy;
    g_ghost_policy = (initial_policy == LRU) ? LFU : LRU;

    if (g_real_policy == LRU) { g_real_cache = init_lru_cache(g_max_cache_size, CACHE_HASHTABLE_SIZE, 0); } else { g_real_cache = init_lfu_cache(g_max_cache_size, CACHE_HASHTABLE_SIZE, 0); }
    if (g_ghost_policy == LRU) { g_ghost_cache = init_lru_cache(g_max_cache_size, CACHE_HASHTABLE_SIZE, 1); } else { g_ghost_cache = init_lfu_cache(g_max_cache_size, CACHE_HASHTABLE_SIZE, 1); }
    
    log_message("ADAPT", "Init -> Real: %s, Ghost: %s", g_real_policy==LRU?"LRU":"LFU", g_ghost_policy==LRU?"LRU":"LFU");
}
void run_adaptation_check() {
    log_message("ADAPT", "Window ended. Real Hits: %lld, Ghost Hits: %lld", g_real_hits, g_ghost_hits);
    if (g_ghost_hits > g_real_hits * 1.1) { // Ghost must be 10% better to prevent flapping
        log_message("ADAPT", "PERFORMANCE SWAP! Ghost policy (%s) was better.", g_ghost_policy == LRU ? "LRU" : "LFU");
        CachePolicy old_real_policy = g_real_policy;
        if (old_real_policy == LRU) destroy_lru_cache(g_real_cache); else destroy_lfu_cache(g_real_cache);
        if (g_ghost_policy == LRU) destroy_lru_cache(g_ghost_cache); else destroy_lfu_cache(g_ghost_cache);
        init_adaptive_cache(g_ghost_policy);
    }
    g_request_count = 0; g_real_hits = 0; g_ghost_hits = 0;
}
void put_in_cache(const char *key, const char *data, size_t data_size) {
    pthread_mutex_lock(&adaptive_mutex);
    if(g_real_policy == LRU) lru_put(g_real_cache, key, data, data_size, 0); else lfu_put(g_real_cache, key, data, data_size, 0);
    pthread_mutex_unlock(&adaptive_mutex);
}
void* get_from_cache(const char *key, size_t* data_size) {
    void* data = NULL;
    if(g_real_policy == LRU) data = lru_get(g_real_cache, key, data_size); else data = lfu_get(g_real_cache, key, data_size);
    if(data) g_real_hits++;
    return data;
}
void ghost_cache_access(const char* key) {
    int hit = 0;
    if(g_ghost_policy == LRU) hit = lru_ghost_get(g_ghost_cache, key); else hit = lfu_ghost_get(g_ghost_cache, key);
    if(hit) g_ghost_hits++;
}
void ghost_cache_put(const char* key) {
    if(g_ghost_policy == LRU) lru_put(g_ghost_cache, key, NULL, 0, 1); else lfu_put(g_ghost_cache, key, NULL, 0, 1);
}
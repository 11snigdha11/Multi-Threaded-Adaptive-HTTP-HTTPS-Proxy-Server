#ifndef UTILS_H
#define UTILS_H

#include <pthread.h>
#include <netinet/in.h> // For INET_ADDRSTRLEN

/* --- Type Definitions --- */
typedef enum { CLIENT_REQUEST, PREFETCH_REQUEST } TaskType;

typedef struct {
    TaskType type;
    union {
        struct {
            int socket;
            char ip[INET_ADDRSTRLEN];
        } client;
        char* url;
    } data;
} Task;

/* --- Global Variables (declared as extern) --- */
extern int g_port;
extern int g_thread_pool_size;
extern size_t g_max_cache_size;
extern size_t g_max_element_size;

/* --- Function Prototypes --- */

// Logging & Monitoring
void log_message(const char* level, const char* format, ...);
void init_logging_and_monitoring(void);
void close_logging(void);
void* monitoring_thread(void* arg); // New monitoring thread function

// Configuration
void load_configuration(const char *filename);
void load_blacklist(const char *filename);
int is_blacklisted(const char *host);
void free_blacklist(void);

// Thread Pool / Task Queue
void init_task_queue(int capacity);
void enqueue_task(Task task);
Task dequeue_task(void);
void broadcast_for_shutdown(void);

// Prefetching
void start_prefetching(const char* html_body, const char* base_host, const char* base_path);
void handle_prefetch_request(char* url);
char* find_header(const char* response, size_t response_len, const char* header_name);
char* get_body(const char* response, size_t response_len);

#endif // UTILS_H
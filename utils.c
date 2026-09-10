#include "utils.h"
#include "cache.h"
#include "proxy_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

/* --- Global Variable Definitions --- */
int g_port;
int g_thread_pool_size;
size_t g_max_cache_size;
size_t g_max_element_size;

/* --- Logging & Monitoring --- */
FILE *log_file;
pthread_mutex_t log_mutex;
// --- MONITORING GLOBALS ---
long long mon_requests = 0;
long long mon_hits = 0;
time_t mon_period_start_time;
pthread_mutex_t mon_mutex;

void init_logging_and_monitoring() {
    log_file = fopen("proxy.log", "a");
    if (!log_file) {
        perror("fopen log file");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_init(&log_mutex, NULL);
    pthread_mutex_init(&mon_mutex, NULL);
    mon_period_start_time = time(NULL);
}
void close_logging() {
    fclose(log_file);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&mon_mutex);
}

void* monitoring_thread(void* arg) {
    extern volatile sig_atomic_t server_running;
    while (server_running) {
        sleep(60); // Reporting interval
        if (!server_running) break;

        pthread_mutex_lock(&mon_mutex);
        time_t now = time(NULL);
        double interval = difftime(now, mon_period_start_time);
        
        double hit_ratio = (mon_requests > 0) ? ((double)mon_hits / mon_requests) * 100.0 : 0.0;
        double throughput = (interval > 0) ? (double)mon_requests / interval : 0.0;

        log_message("MONITOR", "Last 60s -> Hit Ratio: %.2f%%, Throughput: %.2f req/s (Total Req: %lld)", hit_ratio, throughput, mon_requests);

        // Reset for the next period
        mon_requests = 0;
        mon_hits = 0;
        mon_period_start_time = time(NULL);
        pthread_mutex_unlock(&mon_mutex);
    }
    return NULL;
}


/* --- Blacklist --- */
char *blacklist[100];
int blacklist_count = 0;
void free_blacklist() { for(int i = 0; i < blacklist_count; i++) free(blacklist[i]); }

/* --- Task Queue --- */
TaskQueue task_queue;
extern volatile sig_atomic_t server_running;
void init_task_queue(int capacity) { task_queue.tasks = (Task*)malloc(sizeof(Task) * capacity); task_queue.capacity = capacity; task_queue.size = 0; task_queue.head = 0; task_queue.tail = 0; pthread_mutex_init(&task_queue.lock, NULL); pthread_cond_init(&task_queue.not_empty, NULL); pthread_cond_init(&task_queue.not_full, NULL); }
void enqueue_task(Task task) { pthread_mutex_lock(&task_queue.lock); while(task_queue.size == task_queue.capacity) pthread_cond_wait(&task_queue.not_full, &task_queue.lock); task_queue.tasks[task_queue.tail] = task; task_queue.tail = (task_queue.tail + 1) % task_queue.capacity; task_queue.size++; pthread_cond_signal(&task_queue.not_empty); pthread_mutex_unlock(&task_queue.lock); }
Task dequeue_task() { pthread_mutex_lock(&task_queue.lock); while(task_queue.size == 0 && server_running) pthread_cond_wait(&task_queue.not_empty, &task_queue.lock); Task task = { .type = -1 }; if(!server_running && task_queue.size == 0) { pthread_mutex_unlock(&task_queue.lock); return task; } task = task_queue.tasks[task_queue.head]; task_queue.head = (task_queue.head + 1) % task_queue.capacity; task_queue.size--; pthread_cond_signal(&task_queue.not_full); pthread_mutex_unlock(&task_queue.lock); return task; }
void broadcast_for_shutdown() { pthread_cond_broadcast(&task_queue.not_empty); }

/* --- Prefetching --- */
char* find_header(const char* response, size_t response_len, const char* header_name) { const char* headers_end = strstr(response, "\r\n\r\n"); if (!headers_end) return NULL; const char* header_start = strcasestr(response, header_name); if (!header_start || header_start > headers_end) return NULL; header_start += strlen(header_name); while (*header_start == ' ' || *header_start == ':') header_start++; const char* header_end = strstr(header_start, "\r\n"); if (!header_end) return NULL; size_t len = header_end - header_start; char* value = malloc(len + 1); strncpy(value, header_start, len); value[len] = '\0'; return value; }
char* get_body(const char* response, size_t response_len) { const char* body_start = strstr(response, "\r\n\r\n"); if (!body_start) return NULL; return (char*)(body_start + 4); }
char* resolve_url(const char* base_host, const char* base_path, const char* relative_url) { if (strstr(relative_url, "://") || strncmp(relative_url, "//", 2) == 0) return NULL; char* resolved = malloc(strlen(base_host) + strlen(base_path) + strlen(relative_url) + 10); if (!resolved) return NULL; if (relative_url[0] == '/') { sprintf(resolved, "%s%s", base_host, relative_url); } else { char path_copy[strlen(base_path) + 1]; strcpy(path_copy, base_path); char* last_slash = strrchr(path_copy, '/'); if (last_slash) *(last_slash + 1) = '\0'; sprintf(resolved, "%s%s%s", base_host, path_copy, relative_url); } return resolved; }
void start_prefetching(const char* html_body, const char* base_host, const char* base_path) { const char* ptr = html_body; const char* tags[] = {"<img src=\"", "<link href=\"", "<script src=\""}; int tag_lens[] = {10, 12, 13}; for (int i = 0; i < 3; i++) { ptr = html_body; while ((ptr = strstr(ptr, tags[i])) != NULL) { ptr += tag_lens[i]; const char* end_ptr = strchr(ptr, '\"'); if (end_ptr) { int url_len = end_ptr - ptr; if(url_len > 0 && url_len < 1024) { char rel_url[url_len + 1]; strncpy(rel_url, ptr, url_len); rel_url[url_len] = '\0'; char* abs_url = resolve_url(base_host, base_path, rel_url); if (abs_url) { Task prefetch_task = { .type = PREFETCH_REQUEST, .data.url = abs_url }; enqueue_task(prefetch_task); } } ptr = end_ptr; } } } }
void handle_prefetch_request(char* url_key) {
    size_t data_size;
    pthread_mutex_lock(&adaptive_mutex);
    void* data = get_from_cache(url_key, &data_size);
    pthread_mutex_unlock(&adaptive_mutex);
    if (data) { free(data); free(url_key); return; }
    char* filepath = key_to_filepath(url_key); if(filepath) { FILE* f = fopen(filepath, "rb"); if(f) { fclose(f); free(filepath); free(url_key); return; } free(filepath); }
    struct ParsedRequest* req = ParsedRequest_create();
    char fake_req[2048]; snprintf(fake_req, sizeof(fake_req), "GET http://%s HTTP/1.0", url_key);
    if (ParsedRequest_parse(req, fake_req, strlen(fake_req)) < 0) { ParsedRequest_destroy(req); free(url_key); return; }
    log_message("INFO", "Prefetch EXEC for: %s%s", req->host, req->path);
    int remote_port = req->port ? atoi(req->port) : 80; struct hostent *host = gethostbyname(req->host); if (!host) { ParsedRequest_destroy(req); free(url_key); return; }
    int remote_socket = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in remote_addr; remote_addr.sin_family = AF_INET; remote_addr.sin_port = htons(remote_port); bcopy((char*)host->h_addr, (char*)&remote_addr.sin_addr.s_addr, host->h_length);
    if (connect(remote_socket, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == 0) {
        char new_req[MAX_REQUEST_LEN]; snprintf(new_req, sizeof(new_req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", req->path, req->host); send(remote_socket, new_req, strlen(new_req), 0);
        char *res_buf = (char*)malloc(g_max_element_size);
        if (res_buf) {
            ssize_t total_size = 0, bytes;
            while (total_size < g_max_element_size && (bytes = recv(remote_socket, res_buf + total_size, g_max_element_size - total_size, 0)) > 0) total_size += bytes;
            if (total_size > 0) { 
                put_in_cache(url_key, res_buf, total_size); 
                ghost_cache_put(url_key);
                save_to_disk_cache(url_key, res_buf, total_size); 
            }
            free(res_buf);
        }
    }
    close(remote_socket); ParsedRequest_destroy(req); free(url_key);
}